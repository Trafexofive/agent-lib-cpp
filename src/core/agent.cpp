// =============================================================================
// agent-lib-MK3 — Agent Implementation
// Core loop: prompt → build messages → LLM generate → parse actions → dispatch
// → loop
// =============================================================================

#include "agent.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "../feeds/feed_engine.hpp"
#include "../tools/dispatch.hpp"
#include "../utils/ansi.hpp"
#include "dispatch.hpp"
#include "manifest_loader.hpp"

namespace cortex::mk3 {

std::atomic<bool> g_running{true};

// ── XML attribute escaping ──────────────────────────────────────────────
static std::string xmlAttr(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':
                out += "&quot;";
                break;
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            default:
                out += c;
        }
    }
    return out;
}

static std::string indentText(const std::string& text, int spaces) {
    std::ostringstream out;
    std::istringstream in(text);
    std::string line;
    std::string pad(spaces, ' ');
    while (std::getline(in, line)) {
        if (!line.empty())
            out << pad << line;
        out << '\n';
    }
    return out.str();
}

// ═══════════════════════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════════════════════

Agent::Agent(AgentConfig cfg, LlmProviderPtr provider)
    : config_(std::move(cfg)), provider_(std::move(provider)) {
    provider_->setModel(config_.model);
    provider_->setTemperature(config_.temperature);
    provider_->setMaxTokens(config_.maxTokens > 0 ? config_.maxTokens : provider_->getMaxTokens());
    provider_->setTopP(config_.topP);
    provider_->setTopK(config_.topK);
    provider_->setPresencePenalty(config_.presencePenalty);
    provider_->setFrequencyPenalty(config_.frequencyPenalty);

    for (auto& [k, v] : config_.environment)
        env_[k] = v;

    // Load system prompt
    if (!config_.systemPromptText.empty()) {
        systemPrompt_ = config_.systemPromptText;
    } else if (!config_.systemPromptPath.empty()) {
        std::ifstream f(config_.systemPromptPath);
        if (f) {
            std::ostringstream ss;
            ss << f.rdbuf();
            systemPrompt_ = ss.str();
        }
    }

    // Cache harness file once (doesn't change at runtime).
    // Pre-indent every line so buildSystemPrompt doesn't redo O(n) work per
    // turn.
    {
        std::ifstream hf(config_.harnessPath);
        if (hf.is_open()) {
            std::ostringstream oss;
            std::string line;
            while (std::getline(hf, line))
                oss << "    " << line << "\n";
            harnessText_ = oss.str();
        } else if (!config_.harnessPath.empty()) {
            throw std::runtime_error("harness prompt not found: " + config_.harnessPath);
        }
    }

    // Load persona prompt (identity/values)
    if (!config_.personaPath.empty()) {
        std::ifstream pf(config_.personaPath);
        if (pf) {
            std::ostringstream ss;
            ss << pf.rdbuf();
            personaText_ = ss.str();
        }
    }

    // Built-ins are registered in the backend registry below, but NOT granted
    // to this agent by default. Capabilities are declarative: a tool appears in
    // tools_ only when the active manifest imports it.

    // Register internal tool implementations and feeds
    tools::registerDefaults();
    feeds::registerFeeds();

    // Do not restore ./manifests/_session/tools.json automatically here.
    // Capabilities are declarative: the active manifest import block is the
    // runtime tool surface. Session tool files are legacy/reload artifacts and
    // must not silently leak stale tools into a fresh agent.
}

// ═══════════════════════════════════════════════════════════════════════
// Execution Entry Points
// ═══════════════════════════════════════════════════════════════════════

std::string Agent::prompt(const std::string& input, const std::string& sessionId, bool ephemeral) {
    return prompt(input, nullptr, sessionId, ephemeral);
}

std::string Agent::prompt(const std::string& input, StreamCallback onToken,
                          const std::string& sessionId, bool ephemeral) {
    AgentContext ctx;
    ctx.userInput = input;
    ctx.sessionId = sessionId;
    ctx.streaming = (onToken != nullptr);
    ctx.onToken = std::move(onToken);
    ctx.ephemeral = ephemeral;
    ctx.raw = raw_;
    ctx.verbose = verbose_;
    ctx.debug = (env_.count("__DEBUG_MODE__") && env_["__DEBUG_MODE__"] == "true");

    if (!ephemeral && !sessionId.empty()) {
        loadSession(sessionId);
        loadStateCheckpoint(sessionId);
    }

    std::string result = runLoop(ctx);

    dumpSessionArtifacts();

    return result;
}

// ═══════════════════════════════════════════════════════════════════════
// Core Loop
static std::string stripModelOwnedRuntimeTags(const std::string& s) {
    static const std::regex responseRe(R"(<response\b[^>]*>[\s\S]*?</response>)");
    static const std::regex resultRe(R"(<result\b[^>]*>[\s\S]*?</result>)");
    return std::regex_replace(std::regex_replace(s, responseRe, ""), resultRe, "");
}

static std::string formatDelegatedTrace(const std::string& agentName,
                                        const std::string& instruction,
                                        const std::vector<std::string>& prompts,
                                        const std::vector<std::string>& outputs) {
    std::ostringstream os;
    os << "## Delegated Agent: " << agentName << "\n\n";
    os << "### INSTRUCTION\n\n" << instruction << "\n\n";
    for (size_t i = 0; i < prompts.size(); ++i) {
        os << "### SUB-ITERATION " << (i + 1) << " PROMPT\n\n";
        os << prompts[i] << "\n\n";
        if (i < outputs.size()) {
            os << "### SUB-ITERATION " << (i + 1) << " RESPONSE\n\n";
            os << outputs[i] << "\n\n";
        }
    }
    return os.str();
}

static bool jsonBool(const Json::Value& params, const std::string& key, bool def = false) {
    if (!params.isObject() || !params.isMember(key))
        return def;
    const Json::Value& v = params[key];
    if (v.isBool())
        return v.asBool();
    if (v.isString()) {
        std::string s = v.asString();
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s == "true" || s == "1" || s == "yes" || s == "on";
    }
    if (v.isNumeric())
        return v.asInt() != 0;
    return def;
}

static Json::Value makeSubAgentResult(const std::string& output, const std::string& trace,
                                      bool dumpContext) {
    Json::Value r;
    r["success"] = true;
    r["output"] = output;
    if (dumpContext)
        r["trace"] = trace;
    return r;
}

static std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string item;
    while (std::getline(ss, item, '.')) {
        if (!item.empty())
            parts.push_back(item);
    }
    return parts;
}

static const Json::Value* lookupResultPath(const std::map<std::string, Json::Value>& results,
                                           const std::string& id, const std::string& path) {
    auto it = results.find(id);
    if (it == results.end())
        return nullptr;
    const Json::Value* cur = &it->second;
    for (const auto& part : splitPath(path)) {
        if (cur->isObject() && cur->isMember(part)) {
            cur = &((*cur)[part]);
        } else if (cur->isArray()) {
            char* end = nullptr;
            long idx = std::strtol(part.c_str(), &end, 10);
            if (!end || *end != '\0' || idx < 0 || idx >= (long)cur->size())
                return nullptr;
            cur = &((*cur)[(Json::ArrayIndex)idx]);
        } else {
            return nullptr;
        }
    }
    return cur;
}

static std::string jsonValueToInlineString(const Json::Value& v) {
    if (v.isString())
        return v.asString();
    if (v.isBool())
        return v.asBool() ? "true" : "false";
    if (v.isInt64())
        return std::to_string(v.asInt64());
    if (v.isUInt64())
        return std::to_string(v.asUInt64());
    if (v.isDouble()) {
        std::ostringstream os;
        os << v.asDouble();
        return os.str();
    }
    if (v.isNull())
        return "null";
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

static std::string safeSessionPart(std::string s) {
    for (char& c : s) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_' || c == '.';
        if (!ok)
            c = '_';
    }
    return s;
}

static bool subAgentSessionPersistenceEnabled(const std::string& value) {
    return value == "session" || value == "sessions" || value == "persistent" || value == "disk" ||
           value == "true";
}

static std::string deriveSubAgentSessionId(const AgentContext& ctx, const AgentConfig& cfg,
                                           const std::string& agentName) {
    if (ctx.sessionId.empty())
        return "";
    if (!subAgentSessionPersistenceEnabled(cfg.subAgentPersistence))
        return "";
    return safeSessionPart(ctx.sessionId) + "__subagent__" + safeSessionPart(agentName);
}

static Json::Value expansionResultView(const Json::Value& result) {
    Json::Value view = result;
    if (result.isMember("output") && result["output"].isString()) {
        Json::Value parsed;
        Json::CharReaderBuilder r;
        std::string errs;
        std::istringstream ss(result["output"].asString());
        if (Json::parseFromStream(r, ss, &parsed, &errs)) {
            view["json"] = parsed;
            if (parsed.isObject()) {
                for (const auto& key : parsed.getMemberNames()) {
                    if (!view.isMember(key))
                        view[key] = parsed[key];
                }
            }
        }
    }
    return view;
}

static Json::Value expandValueRefs(const Json::Value& value,
                                   const std::map<std::string, Json::Value>& results) {
    static const std::regex refRe(R"(\$\{([A-Za-z_][A-Za-z0-9_-]*)(?:\.([^}]+))?\})");

    if (value.isObject()) {
        Json::Value out(Json::objectValue);
        for (const auto& key : value.getMemberNames()) {
            out[key] = expandValueRefs(value[key], results);
        }
        return out;
    }
    if (value.isArray()) {
        Json::Value out(Json::arrayValue);
        for (Json::ArrayIndex i = 0; i < value.size(); ++i) {
            out.append(expandValueRefs(value[i], results));
        }
        return out;
    }
    if (!value.isString())
        return value;

    const std::string s = value.asString();
    std::smatch exact;
    if (std::regex_match(s, exact, refRe)) {
        std::string id = exact[1].str();
        std::string path = exact.size() > 2 ? exact[2].str() : "";
        const Json::Value* resolved = lookupResultPath(results, id, path);
        if (resolved)
            return *resolved;
        return value;
    }

    std::string out;
    std::string::const_iterator start = s.cbegin();
    std::smatch m;
    while (std::regex_search(start, s.cend(), m, refRe)) {
        out += m.prefix().str();
        std::string id = m[1].str();
        std::string path = m.size() > 2 ? m[2].str() : "";
        const Json::Value* resolved = lookupResultPath(results, id, path);
        out += resolved ? jsonValueToInlineString(*resolved) : m[0].str();
        start = m.suffix().first;
    }
    out.append(start, s.cend());
    return out;
}

static std::string buildResultTag(const std::string& id, const Json::Value& result,
                                  bool compact = false) {
    std::ostringstream os;
    bool ok = result.isMember("success") && result["success"].asBool();
    int exit = result.isMember("exit_code") ? result["exit_code"].asInt() : (ok ? 0 : -1);
    double ms = result.isMember("_elapsed_ms") ? result["_elapsed_ms"].asDouble() : 0;

    os << "<result id=\"" << id << "\" ok=\"" << (ok ? "true" : "false") << "\"";
    if (exit != 0)
        os << " exit=\"" << exit << "\"";
    if (ms > 0)
        os << " ms=\"" << std::fixed << std::setprecision(1) << ms << "\"";

    // Extract primary output body
    std::string body;
    for (const char* key : {"content", "output", "stdout", "result", "results", "data", "value"}) {
        if (!result.isMember(key))
            continue;
        if (result[key].isString()) {
            body = result[key].asString();
        } else {
            Json::StreamWriterBuilder w;
            w["indentation"] = "";
            body = Json::writeString(w, result[key]);
        }
        break;
    }
    if (body.empty() && result.isMember("error") && result["error"].isString())
        body = "error: " + result["error"].asString();

    if (!body.empty()) {
        size_t bytes = body.size();
        if (compact && bytes > 2000) {
            body = body.substr(0, 2000);
            os << " bytes=\"" << bytes << "\" truncated=\"true\"";
        } else if (bytes > 0) {
            os << " bytes=\"" << bytes << "\"";
        }
        os << ">" << body << "</result>";
    } else {
        os << "/>";
    }
    return os.str();
}

// ═══════════════════════════════════════════════════════════════════════
// Tool Dispatch
// ═══════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════

std::string Agent::runLoop(AgentContext& ctx) {
    std::string fullResponse;
    std::string rawOutput;
    rawLlOutput_.clear();
    responseOutput_.clear();
    thoughtOutput_.clear();
    iterationPrompts_.clear();
    iterationOutputs_.clear();
    subAgentTraces_.clear();
    protocolActions_.clear();
    protocolResults_.clear();
    protocolEvents_.clear();

    // Push user input to history once at start (NOT per-iteration)
    history_.push_back("User: " + ctx.userInput);

    // Parser lives across iterations — usedActionIds_ and finalResponseSeen_
    // persist so duplicate-ID and post-final enforcement works cross-turn.
    protocol::Parser parser;
    for (ctx.iteration = 1; ctx.iteration <= config_.iterationCap; ctx.iteration++) {
        if (!g_running) {
            fullResponse = "[cancelled]";
            break;
        }
        ChatMessages msgs = buildChatPrompt(ctx);
        // Save full prompt for /prompts toggle
        lastPrompt_ = msgs.size() > 0 ? msgs[0].content : "";
        {
            std::ostringstream pd;
            for (size_t i = 0; i < msgs.size(); i++) {
                const char* role = ChatMessage::roleName(msgs[i].role);
                if (msgs[i].role == ChatRole::SYSTEM) {
                    if (i == 0) {
                        pd << msgs[i].content;
                        if (!msgs[i].content.empty() && msgs[i].content.back() != '\n')
                            pd << '\n';
                    } else {
                        pd << "<dynamic_context role=\"system\">\n" << msgs[i].content;
                        if (!msgs[i].content.empty() && msgs[i].content.back() != '\n')
                            pd << '\n';
                        pd << "</dynamic_context>\n";
                    }
                } else if (msgs[i].role == ChatRole::USER) {
                    pd << "<user current=\"true\" iteration=\"" << ctx.iteration << "\"";
                    if (!ctx.sessionId.empty())
                        pd << " session=\"" << xmlAttr(ctx.sessionId) << "\"";
                    pd << ">\n" << msgs[i].content;
                    if (!msgs[i].content.empty() && msgs[i].content.back() != '\n')
                        pd << '\n';
                    pd << "</user>\n";
                } else {
                    pd << "<message role=\"" << role << "\">\n" << msgs[i].content;
                    if (!msgs[i].content.empty() && msgs[i].content.back() != '\n')
                        pd << '\n';
                    pd << "</message>\n";
                }
            }
            iterationPrompts_.push_back(pd.str());
        }

        // Last iteration — force response
        if (ctx.iteration == config_.iterationCap) {
            msgs.push_back(
                ChatMessage::user("Respond NOW with <response final=\"true\">. "
                                  "Do not call any more tools."));
        }

        if (ctx.debug || ctx.verbose) {
            std::cerr << "[MK3:DEBUG] iter " << ctx.iteration << " — " << msgs.size() << " msgs";
        }

        // Verbose: dump prompt
        if (ctx.verbose) {
            std::cerr << "\n─── PROMPT iter " << ctx.iteration << " ───\n";
            for (size_t i = 0; i < msgs.size(); i++) {
                const char* role = ChatMessage::roleName(msgs[i].role);
                std::string content = msgs[i].content;
                std::cerr << "[" << role << "] " << content << "\n";
            }
            std::cerr << "─── END PROMPT ───\n";
        }

        dispatch::ActionDispatcher d;
        // Wire agent delegation to sub-agent prompt
        d.agentDelegate = [this, &ctx](const protocol::ParsedAction& action,
                                       const std::string& instruction) -> Json::Value {
            const std::string& agentName = action.name;
            auto it = subAgents_.find(agentName);
            if (it == subAgents_.end()) {
                Json::Value err;
                err["success"] = false;
                err["error"] = "Unknown sub-agent: " + agentName;
                return err;
            }

            bool forceEphemeral = jsonBool(action.params, "ephemeral", false);
            bool dumpContext = jsonBool(action.params, "dump_context", false);
            std::string childSessionId =
                forceEphemeral ? "" : deriveSubAgentSessionId(ctx, config_, agentName);
            std::string result = childSessionId.empty()
                                     ? it->second->prompt(instruction, "", forceEphemeral)
                                     : it->second->prompt(instruction, childSessionId, false);
            std::string trace;
            if (dumpContext) {
                trace = formatDelegatedTrace(agentName, instruction, it->second->iterationPrompts(),
                                             it->second->iterationOutputs());
                subAgentTraces_.push_back(trace);
            }
            return makeSubAgentResult(result, trace, dumpContext);
        };

        // Wire workflow execution — creates a WorkflowRuntime with tool + agent callbacks
        d.workflowDelegate = [this, &ctx](const std::string& workflowName,
                                          const Json::Value& params) -> workflows::WorkflowResult {
            auto wf = workflows::WorkflowEngine::instance().getCached(workflowName);
            workflows::WorkflowRuntime rt;

            // Tool callback: dispatch a tool by name with params
            rt.executeTool = [this](const std::string& name, const Json::Value& p) -> Json::Value {
                protocol::ParsedAction a;
                a.name = name;
                a.type = protocol::ActionType::TOOL;
                a.params = p;
                return dispatchTool(a);
            };

            // Agent callback: delegate to a sub-agent
            rt.executeAgent = [this, &ctx](const std::string& name,
                                           const std::string& instruction) -> Json::Value {
                auto it = subAgents_.find(name);
                if (it == subAgents_.end()) {
                    Json::Value err;
                    err["success"] = false;
                    err["error"] = "Unknown sub-agent: " + name;
                    return err;
                }
                std::string childSessionId = deriveSubAgentSessionId(ctx, config_, name);
                std::string result = childSessionId.empty()
                                         ? it->second->prompt(instruction)
                                         : it->second->prompt(instruction, childSessionId, false);
                Json::Value r;
                r["success"] = true;
                r["output"] = result;
                return r;
            };

            // Recursive workflow call — builds its own runtime, not a copy of rt
            rt.executeWorkflow = [this, &ctx](const std::string& name,
                                              const Json::Value& p) -> workflows::WorkflowResult {
                auto subWf = workflows::WorkflowEngine::instance().getCached(name);
                workflows::WorkflowRuntime subRt;
                subRt.executeTool = [this](const std::string& tn,
                                           const Json::Value& tp) -> Json::Value {
                    protocol::ParsedAction a;
                    a.name = tn;
                    a.type = protocol::ActionType::TOOL;
                    a.params = tp;
                    return dispatchTool(a);
                };
                subRt.executeAgent = [this, &ctx](const std::string& an,
                                                  const std::string& instr) -> Json::Value {
                    auto it = subAgents_.find(an);
                    if (it == subAgents_.end()) {
                        Json::Value err;
                        err["success"] = false;
                        err["error"] = "Unknown sub-agent: " + an;
                        return err;
                    }
                    std::string childSessionId = deriveSubAgentSessionId(ctx, config_, an);
                    std::string result = childSessionId.empty()
                                             ? it->second->prompt(instr)
                                             : it->second->prompt(instr, childSessionId, false);
                    Json::Value r;
                    r["success"] = true;
                    r["output"] = result;
                    return r;
                };
                return workflows::WorkflowEngine::instance().execute(subWf, subRt, p);
            };

            return workflows::WorkflowEngine::instance().execute(wf, rt, params);
        };

        std::string iterationRawOutput;
        std::string iterationRuntimeOutput;

        parser.setExecutor([this, &d, &ctx, &iterationRuntimeOutput](
                               const protocol::ParsedAction& action) -> Json::Value {
            protocol::ParsedAction expandedAction = action;
            expandedAction.params = expandValueRefs(action.params, actionResults_);
            if (!action.content.empty()) {
                Json::Value contentVal(action.content);
                Json::Value expandedContent = expandValueRefs(contentVal, actionResults_);
                if (expandedContent.isString())
                    expandedAction.content = expandedContent.asString();
            }

            // Dedup by (name + resolved params + resolved content). Mutating actions must not use
            // or preserve cache: a successful write invalidates prior
            // reads/tests.
            bool mutatesState = (expandedAction.type == protocol::ActionType::TOOL &&
                                 expandedAction.name == "fs_write");
            std::string key = dispatch::dedupKey(expandedAction);
            if (!mutatesState) {
                auto it = executedActions_.find(key);
                if (it != executedActions_.end()) {
                    Json::Value cached;
                    Json::CharReaderBuilder r;
                    std::string errs;
                    std::istringstream ss(it->second);
                    if (Json::parseFromStream(r, ss, &cached, &errs)) {
                        actionResults_[expandedAction.id] = expansionResultView(cached);
                        return cached;
                    }
                }
            }

            Json::Value result;
            auto t0 = std::chrono::steady_clock::now();
            // Route TOOL actions through agent (supports script tools +
            // sandbox) Other action types (agent/relic/feed) go through the
            // dispatcher
            if (expandedAction.type == protocol::ActionType::TOOL) {
                result = this->dispatchTool(expandedAction);
            } else {
                result = d.dispatch(expandedAction);
            }
            auto t1 = std::chrono::steady_clock::now();
            double elapsedMs =
                std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
            result["_elapsed_ms"] = elapsedMs;  // metadata for renderer
            actionResults_[expandedAction.id] = expansionResultView(result);
            if (mutatesState && result.get("success", false).asBool()) {
                executedActions_.clear();
            } else if (!mutatesState) {
                executedActions_[key] = Json::writeString(Json::StreamWriterBuilder(), result);
            }

            // Inject runtime result into cumulative trace and this iteration's trace.
            {
                std::string resultTag = buildResultTag(action.id, result);
                rawLlOutput_ += "\n" + resultTag + "\n";
                iterationRuntimeOutput += resultTag + "\n";
            }

            // Store protocol result for TUI timeline. Debug mode must still show
            // action/result cards; only raw mode suppresses structured UI.
            if (!ctx.raw) {
                bool ok = result.get("success", false).asBool();
                std::string summary;
                if (ok) {
                    std::string out = result.get("stdout", "").asString();
                    if (!out.empty())
                        summary = out;
                    // Check multiple common result field names
                    else if (result.isMember("result"))
                        summary = result["result"].asString();
                    else if (result.isMember("results"))
                        summary = result["results"].asString();
                    else if (result.isMember("output"))
                        summary = result["output"].asString();
                    else if (result.isMember("data"))
                        summary = result["data"].asString();
                    if (summary.empty())
                        summary = action.name;
                } else {
                    summary = action.name + " — " + result.get("error", "?").asString();
                }
                ProtocolResult protocolResult{action.id,
                                              ok,
                                              summary,
                                              action.name,
                                              result.get("exit_code", 0).asInt(),
                                              result.get("_elapsed_ms", 0.0).asDouble(),
                                              (size_t)summary.size()};
                protocolResults_.push_back(protocolResult);
                protocolEvents_.push_back({ProtocolEventKind::RESULT, "", {}, protocolResult});
                // Notify callback so TUI can stream tool results immediately
                if (ctx.onToken && ctx.streaming)
                    ctx.onToken("", false);
            }

            return result;
        });

        // Tracking state
        std::string llmOutput;
        std::string actionTranscriptOutput;  // model actions only, no premature responses
        bool taskComplete = false;
        bool nonFinalProtocolRetry = false;

        parser.onEvent([&](const protocol::TokenEvent& ev) {
            switch (ev.type) {
                case protocol::TokenEvent::TEXT:
                    // Bare text outside XML tags → ordered thought/protocol stream.
                    thoughtOutput_ += ev.content;
                    if (!ev.content.empty()) {
                        if (!protocolEvents_.empty() &&
                            protocolEvents_.back().kind == ProtocolEventKind::THOUGHT) {
                            protocolEvents_.back().text += ev.content;
                        } else {
                            protocolEvents_.push_back({ProtocolEventKind::THOUGHT, ev.content, {}, {}});
                        }
                    }
                    break;

                case protocol::TokenEvent::RESPONSE:
                    llmOutput += ev.content;
                    responseOutput_ += ev.content;
                    if (!ev.content.empty()) {
                        if (!protocolEvents_.empty() &&
                            protocolEvents_.back().kind == ProtocolEventKind::RESPONSE) {
                            protocolEvents_.back().text += ev.content;
                        } else {
                            protocolEvents_.push_back({ProtocolEventKind::RESPONSE, ev.content, {}, {}});
                        }
                    }
                    if (ctx.onToken)
                        ctx.onToken(ev.content, false);
                    if (ev.metadata.count("is_final") && ev.metadata.at("is_final") == "true") {
                        taskComplete = true;
                    }
                    break;

                case protocol::TokenEvent::THOUGHT:
                    thoughtOutput_ += ev.content;
                    if (!ev.content.empty()) {
                        if (!protocolEvents_.empty() &&
                            protocolEvents_.back().kind == ProtocolEventKind::THOUGHT) {
                            protocolEvents_.back().text += ev.content;
                        } else {
                            protocolEvents_.push_back({ProtocolEventKind::THOUGHT, ev.content, {}, {}});
                        }
                    }
                    break;

                case protocol::TokenEvent::ACTION_START:
                    if (ev.action) {
                        // Store protocol action for TUI/timeline regardless of raw/debug;
                        // debug mode must not hide the action/result UI.
                        std::string typeStr;
                        switch (ev.action->type) {
                            case protocol::ActionType::AGENT:
                                typeStr = "agent";
                                break;
                            case protocol::ActionType::RELIC:
                                typeStr = "relic";
                                break;
                            case protocol::ActionType::FEED:
                                typeStr = "feed";
                                break;
                            case protocol::ActionType::WORKFLOW:
                                typeStr = "workflow";
                                break;
                            default:
                                typeStr = "tool";
                                break;
                        }
                        std::string body = ev.action->content;
                        if (body.empty() && !ev.action->params.isNull()) {
                            Json::StreamWriterBuilder wb;
                            wb["indentation"] = "";
                            body = Json::writeString(wb, ev.action->params);
                        }
                        ProtocolAction protocolAction{typeStr, ev.action->name, ev.action->id, body,
                                                      ev.action->mode == protocol::ExecutionMode::SYNC};
                        protocolActions_.push_back(protocolAction);
                        protocolEvents_.push_back(
                            {ProtocolEventKind::ACTION, "", protocolAction, {}});
                        // Notify the TUI immediately on ACTION_START, before
                        // sync dispatch blocks on tools/sub-agents. The action
                        // card must render first; results arrive later.
                        if (ctx.onToken && ctx.streaming)
                            ctx.onToken("", false);
                        std::ostringstream ax;
                        ax << "<action type=\""
                           << (ev.action->type == protocol::ActionType::TOOL       ? "tool"
                               : ev.action->type == protocol::ActionType::AGENT    ? "agent"
                               : ev.action->type == protocol::ActionType::RELIC    ? "relic"
                               : ev.action->type == protocol::ActionType::WORKFLOW ? "workflow"
                                                                                   : "feed")
                           << "\" name=\"" << ev.action->name << "\" id=\"" << ev.action->id
                           << "\" mode=\""
                           << (ev.action->mode == protocol::ExecutionMode::SYNC ? "sync" : "async")
                           << "\"";
                        if (!ev.action->content.empty() && ev.action->params.isObject()) {
                            for (const auto& key : ev.action->params.getMemberNames()) {
                                const auto& v = ev.action->params[key];
                                if (v.isObject() || v.isArray())
                                    continue;
                                std::string val;
                                if (v.isString())
                                    val = v.asString();
                                else {
                                    Json::StreamWriterBuilder aw;
                                    aw["indentation"] = "";
                                    val = Json::writeString(aw, v);
                                }
                                ax << " " << key << "=\"" << xmlAttr(val) << "\"";
                            }
                        }
                        ax << ">";
                        if (!ev.action->content.empty()) {
                            ax << ev.action->content;
                        } else if (!ev.action->params.isNull()) {
                            Json::StreamWriterBuilder w;
                            w["indentation"] = "";
                            ax << Json::writeString(w, ev.action->params);
                        }
                        ax << "</action>";
                        llmOutput += ax.str() + "\n";
                        actionTranscriptOutput += ax.str() + "\n";
                    }
                    break;

                case protocol::TokenEvent::ACTION_RESULT:
                    break;

                case protocol::TokenEvent::ERROR:
                    history_.push_back(
                        "[ERROR] action=" + (ev.action ? ev.action->name : "?") + " id=" +
                        (ev.metadata.count("id") ? ev.metadata.at("id") : "?") + " reason=" +
                        (ev.metadata.count("reason") ? ev.metadata.at("reason") : "?") + ": " +
                        ev.content);
                    break;

                case protocol::TokenEvent::CONTEXT_FEED:
                    break;

                default:
                    break;
            }
        });

        // Call LLM with exponential-backoff retry on transient empty/filtered
        // responses. Network exceptions are surfaced immediately (existing
        // behavior); only successful-but-empty streams are retried.
        ILlmProvider::StreamStats streamStats;
        int maxAttempts = std::max(1, 1 + config_.emptyResponseMaxRetries);
        int attempt = 0;
        int backoffMs = config_.emptyResponseInitialBackoffMs;
        for (;;) {
            if (attempt > 0) {
                // Reset per-iteration state for the retry attempt so the
                // next stream's tokens don't mix with the prior attempt.
                llmOutput.clear();
                actionTranscriptOutput.clear();
                iterationRawOutput.clear();
                iterationRuntimeOutput.clear();
                responseOutput_.clear();
                thoughtOutput_.clear();
                protocolEvents_.clear();
                parser.reset();

                int delay = std::min(backoffMs, config_.emptyResponseMaxBackoffMs);
                if (ctx.debug) {
                    std::cerr << "[MK3:RETRY] empty-response attempt=" << attempt
                              << " delay_ms=" << delay << " finish_reason=\""
                              << streamStats.finishReason << "\" any_content="
                              << (streamStats.anyContent ? "true" : "false") << "\n";
                }
                auto deadline = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(delay);
                while (g_running && std::chrono::steady_clock::now() < deadline) {
                    auto step = std::min(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now()),
                        std::chrono::milliseconds(100));
                    if (step.count() > 0)
                        std::this_thread::sleep_for(step);
                }
                if (!g_running)
                    break;
                backoffMs = std::min(
                    static_cast<int>(backoffMs * config_.emptyResponseBackoffMultiplier),
                    config_.emptyResponseMaxBackoffMs);
            }

            try {
                provider_->generateStream(msgs, [&](const std::string& token, bool isFinal) {
                    if (taskComplete)
                        return;
                    // Route thinking tokens (\x01 prefix) to thought stream —
                    // live dimmed
                    if (!token.empty() && token[0] == '\x01') {
                        std::string thoughtChunk = token.substr(1);
                        thoughtOutput_ += thoughtChunk;
                        if (!thoughtChunk.empty()) {
                            if (!protocolEvents_.empty() &&
                                protocolEvents_.back().kind == ProtocolEventKind::THOUGHT) {
                                protocolEvents_.back().text += thoughtChunk;
                            } else {
                                protocolEvents_.push_back(
                                    {ProtocolEventKind::THOUGHT, thoughtChunk, {}, {}});
                            }
                        }
                        if (ctx.onToken)
                            ctx.onToken("", false);  // trigger render
                        return;
                    }
                    rawLlOutput_ += token;        // cumulative model/runtime trace
                    iterationRawOutput += token;  // exact model bytes this iteration
                    if (ctx.raw)
                        rawOutput += token;
                    parser.feed(token, isFinal);
                    if (ctx.onToken)
                        ctx.onToken("", isFinal);
                });
            } catch (const std::exception& e) {
                std::string err = std::string("Error: ") + e.what();
                rawLlOutput_ += err;
                iterationRawOutput += err;
                iterationOutputs_.push_back(err);
                return err;
            }

            streamStats = provider_ ? provider_->lastStreamStats()
                                    : ILlmProvider::StreamStats{};

            // Decide whether to retry. Don't retry on legitimate content;
            // only on upstream-side transient failures (empty / filtered /
            // length-truncated / configured reasons).
            bool shouldRetry = (attempt + 1 < maxAttempts);
            if (shouldRetry) {
                if (streamStats.anyContent) {
                    bool retryForReason = false;
                    if (config_.retryOnFinishReasonLength &&
                        streamStats.finishReason == "length")
                        retryForReason = true;
                    if (config_.retryOnFinishReasonContentFilter &&
                        (streamStats.finishReason == "content_filter" ||
                         streamStats.finishReason == "empty"))
                        retryForReason = true;
                    for (const auto& r : config_.retryOnFinishReasons) {
                        if (streamStats.finishReason == r) {
                            retryForReason = true;
                            break;
                        }
                    }
                    shouldRetry = retryForReason;
                }
                // else: !anyContent → always retry
            }

            if (!shouldRetry)
                break;
            ++attempt;
        }

        if (!parser.waitForActions(std::chrono::seconds(config_.actionTimeoutSec))) {
            history_.push_back("System: [TIMEOUT] actions did not complete within " +
                               std::to_string(config_.actionTimeoutSec) + "s");
            break;
        }
        parser.flush();

        // Collect context feeds
        auto feeds = parser.contextFeeds();
        for (auto& feed : feeds)
            contextFeeds_.push_back(feed);

        // Determine completion
        auto results = parser.allResults();

        if (ctx.debug || ctx.verbose) {
            std::cerr << " | actions=" << results.size() << " complete=" << taskComplete
                      << " resp=" << responseOutput_.size() << "b"
                      << " text=" << llmOutput.size() << "b";
            if (ctx.verbose && !llmOutput.empty()) {
                std::cerr << " \"" << llmOutput << "\"";
            }
            std::cerr << "\n";
        }

        if (results.empty() && !taskComplete) {
            // No parsed actions and no final response. This is NOT completion.
            // Either the upstream returned no content, or the model emitted
            // bare/non-protocol text. In both cases continue the loop unless
            // we're surfacing a hard runtime failure after retries are exhausted.
            if (!streamStats.anyContent) {
                std::string detail;
                if (!streamStats.finishReason.empty())
                    detail += " (finish_reason=" + streamStats.finishReason + ")";
                if (!streamStats.lastError.empty())
                    detail += " — " + streamStats.lastError.substr(0, 200);
                if (streamStats.httpStatus > 0)
                    detail += " [http " + std::to_string(streamStats.httpStatus) + "]";
                std::string visibleError =
                    "⚠ Model returned an empty response" + detail +
                    ". The agent loop is aborting this turn rather than silently finishing. "
                    "Retry with a different model if this persists.";
                history_.push_back("System: [EMPTY RESPONSE] " + detail);
                history_.push_back("Agent: " + visibleError);
                responseOutput_ = visibleError;
                taskComplete = true;  // runtime failure, not model final
            } else {
                // LLM produced bare/non-protocol text. Record it and force a
                // follow-up. Do NOT complete the turn: only
                // <response final=\"true\"> completes normally.
                history_.push_back("Agent: " + iterationRawOutput);
                if (!bareTextReminded_ && !iterationRawOutput.empty()) {
                    std::string trimmed = iterationRawOutput;
                    size_t first = trimmed.find_first_not_of(" \t\n\r");
                    if (first != std::string::npos && trimmed[first] != '<') {
                        bareTextReminded_ = true;
                        std::string preview =
                            trimmed.size() > 60 ? trimmed.substr(0, 60) + "..." : trimmed;
                        history_.push_back(
                            "System: \342\232\240 BARE TEXT STRIPPED: \"" + preview +
                            "\" — "
                            "This did NOT complete the turn. Emit exactly one of: "
                            "<response final=\"true\">...</response> or "
                            "<action type=\"tool\" ...>...</action>. Bare text is invisible.");
                    }
                } else {
                    history_.push_back(
                        "System: [PROTOCOL RETRY] Previous model output had no action and no "
                        "<response final=\"true\">. Continue now with a valid protocol tag.");
                }
                nonFinalProtocolRetry = true;
                taskComplete = false;
                responseOutput_.clear();
            }
        }

        // Capture exact model output plus runtime-injected results for debugging.
        // Do not add nested markdown headings here; iterations.md already marks sections.
        {
            std::ostringstream os;
            os << iterationRawOutput;
            if (!iterationRawOutput.empty() && iterationRawOutput.back() != '\n')
                os << "\n";
            os << iterationRuntimeOutput;
            iterationOutputs_.push_back(os.str());
        }

        bool forceResultFollowup = taskComplete && !results.empty() &&
                                   iterationRawOutput.find("<action") != std::string::npos;
        // If the model emits action(s) and a final response in the same generation,
        // it cannot have seen the real runtime results yet. Keep only the action
        // transcript for the follow-up prompt; discard premature response text and
        // any model-owned result/prose.
        std::string historyOutput = forceResultFollowup ? actionTranscriptOutput : llmOutput;
        if (forceResultFollowup) {
            // The model cannot consume a sync action result in the same
            // generation that emitted the action. Force one follow-up turn with
            // the real <result> in context instead of accepting a premature final.
            // Also drop the premature response from history so the next turn
            // sees only the action it actually took plus the runtime result.
            taskComplete = false;
            responseOutput_.clear();
        }

        if (taskComplete) {
            Json::Value expandedResponse =
                expandValueRefs(Json::Value(responseOutput_), actionResults_);
            fullResponse =
                expandedResponse.isString() ? expandedResponse.asString() : responseOutput_;
            break;
        }

        // Prepare next iteration — push agent output, then system results.
        // Bare/non-final protocol retries already pushed the raw model output
        // plus a strict system correction above; don't add an empty duplicate.
        if (!nonFinalProtocolRetry)
            history_.push_back("Agent: " + historyOutput);
        if (!results.empty()) {
            for (auto& [id, result] : results) {
                std::ostringstream sysMsg;
                sysMsg << buildResultTag(id, result, true);
                history_.push_back("System: " + sysMsg.str());
            }
        }
        parser.clearResults();  // prevent result leakage to next iteration
        tickContextCycles();    // decrement peek cycles; auto-evict at 0
    }

    if (fullResponse.empty()) {
        fullResponse =
            "⚠ Agent stopped without emitting <response final=\"true\">. "
            "The runtime refused to treat non-final/bare output as completion.";
    }

    if (!ctx.ephemeral && !ctx.sessionId.empty()) {
        saveSession(ctx.sessionId);
        saveStateCheckpoint(ctx.sessionId);
    }

    if (ctx.raw && !rawOutput.empty()) {
        return rawOutput;
    }
    return sanitize(fullResponse);
}

// ═══════════════════════════════════════════════════════════════════════
// Prompt Building
// ═══════════════════════════════════════════════════════════════════════

ChatMessages Agent::buildChatPrompt(const AgentContext& ctx) const {
    ChatMessages msgs;
    msgs.push_back(ChatMessage::system(buildSystemPrompt(ctx)));
    // First iteration: current user request is a real user message, not
    // synthetic history inside the system prompt. Later iterations replay the
    // inline action/result transcript from history, but still need a minimal
    // provider-level user message; some OpenRouter routes reject system-only
    // continuation calls as "No user query found".
    if (ctx.iteration <= 1) {
        msgs.push_back(ChatMessage::user(buildUserPrompt(ctx)));
    } else {
        msgs.push_back(ChatMessage::user(
            "Continue from the inline transcript above. Use runtime results only; "
            "if enough information is available, emit <response final=\"true\">."));
    }
    std::string dynamicTail = buildDynamicContextPrompt();
    if (!dynamicTail.empty()) {
        // Dynamic context is intentionally last for prompt-cache friendliness,
        // but it is runtime/system context, not another user request.
        msgs.push_back(ChatMessage::system(dynamicTail));
    }
    return msgs;
}

std::string Agent::buildSystemPrompt(const AgentContext& ctx) const {
    std::ostringstream ss;

    // ═══ <harness> — protocol spec (pre-indented in constructor) ═══
    ss << "<harness>\n  <protocol>\n";
    if (!harnessText_.empty()) {
        ss << harnessText_;
    } else {
        // Hardcoded fallback
        ss << "    ⚠ ABSOLUTELY REQUIRED: Each turn, output EXACTLY ONE of "
              "these XML formats. Nothing else.\n";
        ss << "    Bare text (not inside <response>...</response>) is "
              "DISCARDED. The user will NOT see it.\n";
        ss << "    \n";
        ss << "    FORMAT A — Final answer:\n";
        ss << "    <response final=\"true\">answer here</response>\n";
        ss << "    \n";
        ss << "    FORMAT B — Call a tool:\n";
        ss << "    <action type=\"tool\" name=\"list\" id=\"ls1\" "
              "mode=\"sync\">{\"path\":\".\"}</action>\n";
        ss << "    id must be unique. Use short ids like ls1, grep1, read1.\n";
        ss << "    \n";
        ss << "    After a tool call, you receive a <result> message. Read the "
              "result, then respond.\n";
        ss << "    Do not call the same tool twice with the same parameters.\n";
        ss << "    Stop after giving your final answer.\n";
    }
    ss << "  </protocol>\n";
    ss << "  <info name=\"" << xmlAttr(config_.name) << "\" version=\"" << xmlAttr(config_.version)
       << "\"/>\n";
    ss << "</harness>\n\n";

    // ═══ <system> — persona, system, tools, relics, context ═══
    ss << "<system>\n";

    // Persona block — identity/values (loaded from personaPath)
    if (!personaText_.empty()) {
        ss << "  <persona>\n";
        ss << indentText(personaText_, 4) << "\n";
        ss << "  </persona>\n";
    }

    // System prompt block — capabilities/tools/behavior (loaded from systemPromptPath)
    if (!systemPrompt_.empty()) {
        ss << "  <system_prompt>\n";
        ss << indentText(systemPrompt_, 4) << "\n";
        ss << "  </system_prompt>\n";
    }

    ss << "  <action_available>\n";
    ss << "    <description>Callable runtime surfaces. Use these with <action type=\"...\"> only "
          "when needed.</description>\n";

    ss << "    <tools>\n        <description>Functions callable with <action type=\"tool\">. "
          "Prefer declared JSON params; if a tool declares text input, small scalar attrs plus a "
          "text body are allowed.</description>\n";
    auto schemaIt = env_.find("__TOOL_SCHEMAS__");
    bool hasSchemas = (schemaIt != env_.end() && !schemaIt->second.empty());
    if (hasSchemas) {
        ss << schemaIt->second << "\n";
    }
    for (const auto& [name, tool] : tools_) {
        // Only emit tools NOT already covered by manifest-loaded schemas.
        if (hasSchemas && schemaIt->second.find("name=\"" + name + "\"") != std::string::npos)
            continue;

        // Session-restored script tools keep scriptPath but historically lost
        // schema context. Recover nearest tool.yml so the model sees params.
        bool emittedRecoveredSchema = false;
        if (!tool.scriptPath().empty()) {
            std::filesystem::path scriptPath(tool.scriptPath());
            std::vector<std::filesystem::path> candidates = {
                scriptPath.parent_path() / "tool.yml",
                scriptPath.parent_path().parent_path() / "tool.yml"};
            for (const auto& candidate : candidates) {
                if (!std::filesystem::exists(candidate))
                    continue;
                auto recovered = ManifestLoader::loadToolManifest(candidate.string());
                if (recovered.name.empty() || recovered.name != name)
                    continue;
                const auto& rc = config_.promptBuilding.runtimeCapabilities;
                ss << ManifestLoader::toolSchemasToXml({recovered}, 8, rc.inputSchemas,
                                                       rc.returnSchemas, rc.usageExamples);
                emittedRecoveredSchema = true;
                break;
            }
        }
        if (emittedRecoveredSchema)
            continue;

        ss << "        <tool name=\"" << xmlAttr(name) << "\"";
        if (!tool.description().empty() && tool.description() != "See input_schema for parameters")
            ss << " desc=\"" << xmlAttr(tool.description()) << "\"";
        ss << ">\n";
        ss << "\n            <params unavailable=\"true\">schema not loaded; do not guess required "
              "JSON keys</params>\n";
        ss << "        </tool>\n";
    }
    ss << "    </tools>\n";

    if (!relics_.empty()) {
        ss << "    <relics>\n        <description>Persistent stores callable with <action "
              "type=\"relic\">.</description>\n";
        for (auto& name : relics_) {
            ss << "        <relic name=\"" << xmlAttr(name) << "\"/>\n";
        }
        ss << "    </relics>\n";
    }

    if (!feedNames().empty()) {
        ss << "    <feeds>\n        <description>Ambient context feeds. Callable with <action "
              "type=\"feed\"> when fresh params are needed.</description>\n";
        for (const auto& name : feedNames()) {
            ss << "        <feed name=\"" << xmlAttr(name) << "\" action=\"feed\"/>\n";
        }
        ss << "    </feeds>\n";
    }

    if (!subAgents_.empty()) {
        ss << "    <sub_agents>\n"
              "        <description>Delegatable agents callable with <action type=\"agent\" "
              "name=\"AGENT_NAME\" id=\"a1\" mode=\"sync\" ephemeral=\"true|false\" "
              "dump_context=\"true|false\">plain text instruction</action>. "
              "Inputs and outputs are plain text unless the sub-agent says otherwise. "
              "Default result contains only the sub-agent final response. Set dump_context=\"true\" "
              "only when you explicitly need its prompts/runtime trace.</description>\n";
        for (const auto& [name, agent] : subAgents_) {
            const auto& cfg = agent->config();
            ss << "        <sub_agent name=\"" << xmlAttr(name) << "\"";
            if (!cfg.version.empty())
                ss << " version=\"" << xmlAttr(cfg.version) << "\"";
            if (!cfg.summary.empty())
                ss << " summary=\"" << xmlAttr(cfg.summary) << "\"";
            if (!cfg.provider.empty())
                ss << " provider=\"" << xmlAttr(cfg.provider) << "\"";
            if (!cfg.model.empty())
                ss << " model=\"" << xmlAttr(cfg.model) << "\"";
            if (!cfg.manifestDir.empty())
                ss << " manifest_dir=\"" << xmlAttr(cfg.manifestDir) << "\"";
            ss << ">\n";

            auto names = agent->toolNames();
            if (!names.empty()) {
                ss << "            <tools>\n";
                for (const auto& toolName : names) {
                    const auto* tool = agent->findTool(toolName);
                    ss << "                <tool name=\"" << xmlAttr(toolName) << "\"";
                    if (tool && !tool->description().empty() &&
                        tool->description() != "See input_schema for parameters")
                        ss << " desc=\"" << xmlAttr(tool->description()) << "\"";
                    ss << "/>\n";
                }
                ss << "            </tools>\n";
            }
            ss << "        </sub_agent>\n";
        }
        ss << "    </sub_agents>\n";
    }

    auto wfIt = env_.find("__WORKFLOW_XML__");
    if (wfIt != env_.end() && !wfIt->second.empty()) {
        ss << "    <workflows>\n";
        ss << indentText(wfIt->second, 8) << "\n";
        ss << "    </workflows>\n";
    }

    ss << "  </action_available>\n";

    // Capability counts — a compact at-a-glance summary of what the active
    // manifest granted. Helps the model self-check before attempting an
    // action: if tools c=0, no <action type="tool"> can succeed.
    ss << "  <manifest_count>";
    ss << "<tools c=" << tools_.size() << ">";
    ss << "<relics c=" << relicNames().size() << ">";
    ss << "<feeds c=" << feedNames().size() << ">";
    ss << "<agents c=" << subAgentNames().size() << ">";
    ss << "</manifest_count>\n";

    ss << "  <cwd>" << std::filesystem::current_path().string() << "</cwd>\n";
    ss << "</system>\n\n";

    // ═══ INLINE EXECUTION TRANSCRIPT ═══
    // Replay what actually happened. Agent/System prefixes are storage detail;
    // the model should see the same inline action → result → response stream.
    if (!history_.empty()) {
        // Apply history cap — only include the most recent N entries
        size_t histStart = 0;
        if (config_.historyCap > 0 && history_.size() > (size_t)config_.historyCap) {
            histStart = history_.size() - config_.historyCap;
        }

        size_t userTurn = 0;
        for (size_t hi = histStart; hi < history_.size(); hi++) {
            const auto& h = history_[hi];
            std::string emitted;
            if (h.rfind("Agent: ", 0) == 0) {
                emitted = h.substr(7);
            } else if (h.rfind("System: ", 0) == 0) {
                emitted = h.substr(8);
            } else if (h.rfind("User: ", 0) == 0) {
                userTurn++;
                // On iteration 1 the current request is sent as a real user
                // message, not replayed inside the system prompt transcript.
                if (ctx.iteration <= 1 && hi + 1 == history_.size() && h.substr(6) == ctx.userInput)
                    continue;
                emitted = "<user turn=\"" + std::to_string(userTurn) + "\"";
                if (!ctx.sessionId.empty())
                    emitted += " session=\"" + xmlAttr(ctx.sessionId) + "\"";
                emitted += ">" + h.substr(6) + "</user>";
            } else {
                emitted = h;
            }
            ss << emitted;
            if (!emitted.empty() && emitted.back() != '\n')
                ss << '\n';
        }
    }

    return ss.str();
}

std::string Agent::buildUserPrompt(const AgentContext& ctx) const {
    return ctx.userInput;
}

std::string Agent::buildDynamicContextPrompt() const {
    std::ostringstream ss;

    if (!pinned_.empty()) {
        ss << "<pinned_context>\n"
              "  <description>Files the agent pinned via context_pin. "
              "Persist until context_unpin.</description>\n";
        for (auto& [key, e] : pinned_) {
            ss << "  <file path=\"" << xmlAttr(e.displayPath) << "\" bytes=\"" << e.bytes
               << "\">\n";
            ss << e.content;
            if (!e.content.empty() && e.content.back() != '\n')
                ss << '\n';
            ss << "  </file>\n";
        }
        ss << "</pinned_context>\n";
    }

    if (!peeking_.empty()) {
        if (ss.tellp() > 0)
            ss << "\n";
        ss << "<ephemeral_context>\n"
              "  <description>Files peeked via context_peek. Auto-evicted "
              "after their cycle count expires.</description>\n";
        for (auto& [key, e] : peeking_) {
            ss << "  <file path=\"" << xmlAttr(e.displayPath) << "\" bytes=\"" << e.bytes
               << "\" cycles_remaining=\"" << e.cyclesRemaining << "\">\n";
            ss << e.content;
            if (!e.content.empty() && e.content.back() != '\n')
                ss << '\n';
            ss << "  </file>\n";
        }
        ss << "</ephemeral_context>\n";
    }

    if (!feeds_.empty()) {
        auto feedResults = feeds::FeedEngine::instance().pollAll();
        bool any = false;
        for (auto& fr : feedResults) {
            if (!feeds_.count(fr.name))
                continue;
            if (!any) {
                if (ss.tellp() > 0)
                    ss << "\n";
                ss << "<feeds>\n  <description>Dynamic system context refreshed each turn. "
                      "Bottom-loaded for prompt-cache stability.</description>\n";
                any = true;
            }
            ss << "  <" << fr.name << ">\n";
            ss << "  " << fr.summary << "\n";
            ss << "  </" << fr.name << ">\n";
        }
        if (any)
            ss << "</feeds>\n";
    }

    if (!contextFeeds_.empty()) {
        if (ss.tellp() > 0)
            ss << "\n";
        ss << "<context_feeds>\n  <description>LLM-requested dynamic context from prior "
              "turns.</description>\n";
        for (auto& feed : contextFeeds_) {
            ss << "  " << feed << "\n";
        }
        ss << "</context_feeds>\n";
    }

    return ss.str();
}

// ═══════════════════════════════════════════════════════════════════════
// Sanitize output — strip protocol XML tags
// ═══════════════════════════════════════════════════════════════════════

std::string Agent::sanitize(const std::string& output) {
    // Linear state-machine tag stripper — 10-20x faster than regex on large
    // outputs
    static const std::vector<std::string> tags = {"action", "result", "thought", "context_feed",
                                                  "response"};
    std::string out;
    out.reserve(output.size());
    size_t i = 0, n = output.size();
    while (i < n) {
        if (output[i] != '<') {
            out += output[i++];
            continue;
        }
        bool matched = false;
        for (auto& tag : tags) {
            size_t tagLen = tag.size();
            // <tag> or </tag>
            bool isClose = (i + 1 < n && output[i + 1] == '/');
            size_t nameStart = isClose ? i + 2 : i + 1;
            if (n - nameStart >= tagLen && output.compare(nameStart, tagLen, tag) == 0) {
                size_t close = output.find('>', nameStart + tagLen);
                if (close == std::string::npos)
                    break;
                if (isClose) {
                    i = close + 1;
                    matched = true;
                    break;
                }
                // Find matching closing tag
                std::string endTag = "</" + tag + ">";
                size_t endPos = output.find(endTag, close);
                if (endPos == std::string::npos)
                    break;
                i = endPos + endTag.size();
                matched = true;
                break;
            }
        }
        if (!matched)
            out += output[i++];
    }
    size_t start = out.find_first_not_of(" \t\n\r");
    size_t end = out.find_last_not_of(" \t\n\r");
    return (start == std::string::npos) ? "" : out.substr(start, end - start + 1);
}

// ── Tool dispatch — see agent_tools.cpp
// ── Session lifecycle — see agent_session.cpp

}  // namespace cortex::mk3
