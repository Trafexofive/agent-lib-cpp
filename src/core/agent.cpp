// =============================================================================
// agent-lib-MK3 — Agent Implementation
// Core loop: prompt → build messages → LLM generate → parse actions → dispatch
// → loop
// =============================================================================

#include "agent.hpp"
#include "../feeds/feed_engine.hpp"
#include "../relics/builtin_relics.hpp"
#include "../tools/dispatch.hpp"
#include "../utils/ansi.hpp"
#include "dispatch.hpp"
#include "manifest_loader.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace cortex::mk3 {

std::atomic<bool> g_running{true};

// ── XML attribute escaping ──────────────────────────────────────────────
static std::string xmlAttr(const std::string &s) {
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

// Built-in relic endpoint map — file scope, no magic-statics lock on hot path.
static const std::map<std::string, std::string> BUILTIN_RELICS = {
    {"session_journal", "record,query,export,prune"},
    {"state_checkpoint", "save,load,list,gc"}};

// ═══════════════════════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════════════════════

Agent::Agent(AgentConfig cfg, LlmProviderPtr provider)
    : config_(std::move(cfg)), provider_(std::move(provider)) {
    provider_->setModel(config_.model);
    provider_->setTemperature(config_.temperature);
    provider_->setMaxTokens(config_.maxTokens);
    provider_->setTopP(config_.topP);
    provider_->setTopK(config_.topK);
    provider_->setPresencePenalty(config_.presencePenalty);
    provider_->setFrequencyPenalty(config_.frequencyPenalty);

    for (auto &[k, v] : config_.environment)
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
        // Use config harness path if set, otherwise default
        std::string hp = config_.harnessPath.empty()
                             ? "config/harness/default.md"
                             : config_.harnessPath;
        std::ifstream hf(hp);
        if (hf.is_open()) {
            std::ostringstream oss;
            std::string line;
            while (std::getline(hf, line))
                oss << "    " << line << "\n";
            harnessText_ = oss.str();
        }
    }

    // Built-ins are registered in the backend registry below, but NOT granted
    // to this agent by default. Capabilities are declarative: a tool appears in
    // tools_ only when the active manifest imports it.

    // Register internal tool implementations and feeds
    tools::registerDefaults();
    feeds::registerFeeds();

    // Restore dynamically-loaded tools from previous sessions
    loadSessionTools();
}


// ═══════════════════════════════════════════════════════════════════════
// Execution Entry Points
// ═══════════════════════════════════════════════════════════════════════

std::string Agent::prompt(const std::string &input,
                          const std::string &sessionId, bool ephemeral) {
    return prompt(input, nullptr, sessionId, ephemeral);
}

std::string Agent::prompt(const std::string &input, StreamCallback onToken,
                          const std::string &sessionId, bool ephemeral) {
    AgentContext ctx;
    ctx.userInput = input;
    ctx.sessionId = sessionId;
    ctx.streaming = (onToken != nullptr);
    ctx.onToken = std::move(onToken);
    ctx.ephemeral = ephemeral;
    ctx.raw = raw_;
    ctx.verbose = verbose_;
    ctx.debug =
        (env_.count("__DEBUG_MODE__") && env_["__DEBUG_MODE__"] == "true");

    if (!ephemeral && !sessionId.empty())
        loadSession(sessionId);

    std::string result = runLoop(ctx);

    dumpSessionArtifacts();

    return result;
}

// ═══════════════════════════════════════════════════════════════════════
// Core Loop
static std::string buildResultTag(const std::string& id, const Json::Value& result, bool compact = false) {
    std::ostringstream os;
    bool ok = result.isMember("success") && result["success"].asBool();
    int exit = result.isMember("exit_code") ? result["exit_code"].asInt() : (ok ? 0 : -1);
    double ms = result.isMember("_elapsed_ms") ? result["_elapsed_ms"].asDouble() : 0;

    os << "<result id=\"" << id << "\" ok=\"" << (ok ? "true" : "false") << "\"";
    if (exit != 0) os << " exit=\"" << exit << "\"";
    if (ms > 0) os << " ms=\"" << std::fixed << std::setprecision(1) << ms << "\"";

    // Extract primary output body
    std::string body;
    for (const char* key : {"content", "output", "stdout", "result", "results", "data"}) {
        if (result.isMember(key) && result[key].isString()) {
            body = result[key].asString();
            break;
        }
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

std::string Agent::runLoop(AgentContext &ctx) {
    std::string fullResponse;
    std::string rawOutput;
    rawLlOutput_.clear();
    responseOutput_.clear();
    thoughtOutput_.clear();
    iterationPrompts_.clear();
    iterationOutputs_.clear();
    protocolActions_.clear();
    protocolResults_.clear();

    // Push user input to history once at start (NOT per-iteration)
    history_.push_back("User: " + ctx.userInput);


    // Parser lives across iterations — usedActionIds_ and finalResponseSeen_
    // persist so duplicate-ID and post-final enforcement works cross-turn.
    protocol::Parser parser;
    for (ctx.iteration = 1; ctx.iteration <= config_.iterationCap;
         ctx.iteration++) {
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
                pd << "[" << ChatMessage::roleName(msgs[i].role) << "] "
                   << msgs[i].content << "\n";
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
            std::cerr << "[MK3:DEBUG] iter " << ctx.iteration << " — "
                      << msgs.size() << " msgs";
        }

        // Verbose: dump prompt
        if (ctx.verbose) {
            std::cerr << "\n─── PROMPT iter " << ctx.iteration << " ───\n";
            for (size_t i = 0; i < msgs.size(); i++) {
                const char *role = ChatMessage::roleName(msgs[i].role);
                std::string content = msgs[i].content;
                std::cerr << "[" << role << "] " << content << "\n";
            }
            std::cerr << "─── END PROMPT ───\n";
        }


        dispatch::ActionDispatcher d;
        // Wire agent delegation to sub-agent prompt
        d.agentDelegate =
            [this](const std::string &agentName,
                   const std::string &instruction) -> Json::Value {
            auto it = subAgents_.find(agentName);
            if (it == subAgents_.end()) {
                Json::Value err;
                err["success"] = false;
                err["error"] = "Unknown sub-agent: " + agentName;
                return err;
            }
            std::string result = it->second->prompt(instruction);
            Json::Value r;
            r["success"] = true;
            r["output"] = result;
            return r;
        };

        parser.setExecutor([this, &d,
                            &ctx](const protocol::ParsedAction &action)
                               -> Json::Value {
            // Dedup by (name + params + content). Mutating actions must not use
            // or preserve cache: a successful write invalidates prior
            // reads/tests.
            bool mutatesState = (action.type == protocol::ActionType::TOOL &&
                                 action.name == "fs_write");
            std::string key = dispatch::dedupKey(action);
            if (!mutatesState) {
                auto it = executedActions_.find(key);
                if (it != executedActions_.end()) {
                    Json::Value cached;
                    Json::CharReaderBuilder r;
                    std::string errs;
                    std::istringstream ss(it->second);
                    if (Json::parseFromStream(r, ss, &cached, &errs))
                        return cached;
                }
            }

            Json::Value result;
            auto t0 = std::chrono::steady_clock::now();
            // Route TOOL actions through agent (supports script tools +
            // sandbox) Other action types (agent/relic/feed) go through the
            // dispatcher
            if (action.type == protocol::ActionType::TOOL) {
                result = this->dispatchTool(action);
            } else {
                result = d.dispatch(action);
            }
            auto t1 = std::chrono::steady_clock::now();
            double elapsedMs =
                std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0)
                    .count() /
                1000.0;
            result["_elapsed_ms"] = elapsedMs; // metadata for renderer
            if (mutatesState && result.get("success", false).asBool()) {
                executedActions_.clear();
            } else if (!mutatesState) {
                executedActions_[key] =
                    Json::writeString(Json::StreamWriterBuilder(), result);
            }

            // Inject <result> tag into raw output — plain text body, not JSON
            {
                rawLlOutput_ += "\n" + buildResultTag(action.id, result) + "\n";
            }

            // Store protocol result
            if (!ctx.debug && !ctx.raw) {
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
                    summary = action.name + " — " +
                              result.get("error", "?").asString();
                }
                protocolResults_.push_back(
                    {action.id, ok, summary, action.name,
                     result.get("exit_code", 0).asInt(),
                     result.get("_elapsed_ms", 0.0).asDouble(),
                     (size_t)summary.size()});
                // Notify callback so TUI can stream tool results immediately
                if (ctx.onToken && ctx.streaming)
                    ctx.onToken("", false);
            }

            return result;
        });

        // Tracking state
        std::string llmOutput;
        bool taskComplete = false;

        parser.onEvent([&](const protocol::TokenEvent &ev) {
            switch (ev.type) {
            case protocol::TokenEvent::TEXT:
                // Bare text outside XML tags → thought stream (dimmed, not in
                // history)
                thoughtOutput_ += ev.content;
                break;

            case protocol::TokenEvent::RESPONSE:
                llmOutput += ev.content;
                responseOutput_ += ev.content;
                if (ctx.onToken)
                    ctx.onToken(ev.content, false);
                if (ev.metadata.count("is_final") &&
                    ev.metadata.at("is_final") == "true") {
                    taskComplete = true;
                }
                break;

            case protocol::TokenEvent::THOUGHT:
                thoughtOutput_ += ev.content;
                break;

            case protocol::TokenEvent::ACTION_START:
                if (ev.action) {
                    // Store protocol action
                    if (!ctx.debug && !ctx.raw) {
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
                        protocolActions_.push_back(
                            {typeStr, ev.action->name, ev.action->id, body,
                             ev.action->mode == protocol::ExecutionMode::SYNC});
                    }
                    std::ostringstream ax;
                    ax << "<action type=\""
                       << (ev.action->type == protocol::ActionType::TOOL
                               ? "tool"
                           : ev.action->type == protocol::ActionType::AGENT
                               ? "agent"
                           : ev.action->type == protocol::ActionType::RELIC
                               ? "relic"
                               : "feed")
                       << "\" name=\"" << ev.action->name << "\" id=\""
                       << ev.action->id << "\" mode=\""
                       << (ev.action->mode == protocol::ExecutionMode::SYNC
                               ? "sync"
                               : "async")
                       << "\">";
                    if (!ev.action->params.isNull()) {
                        Json::StreamWriterBuilder w;
                        w["indentation"] = "";
                        ax << Json::writeString(w, ev.action->params);
                    } else if (!ev.action->content.empty()) {
                        ax << ev.action->content;
                    }
                    ax << "</action>";
                    llmOutput += ax.str() + "\n";
                }
                break;

            case protocol::TokenEvent::ACTION_RESULT:
                break;

            case protocol::TokenEvent::ERROR:
                history_.push_back(
                    "[ERROR] action=" + (ev.action ? ev.action->name : "?") +
                    " id=" +
                    (ev.metadata.count("id") ? ev.metadata.at("id") : "?") +
                    " reason=" +
                    (ev.metadata.count("reason") ? ev.metadata.at("reason")
                                                 : "?") +
                    ": " + ev.content);
                break;

            case protocol::TokenEvent::CONTEXT_FEED:
                break;

            default:
                break;
            }
        });

        // Call LLM
        try {
            provider_->generateStream(
                msgs, [&](const std::string &token, bool isFinal) {
                    if (taskComplete)
                        return;
                    // Route thinking tokens (\x01 prefix) to thought stream —
                    // live dimmed
                    if (!token.empty() && token[0] == '\x01') {
                        thoughtOutput_ += token.substr(1);
                        if (ctx.onToken)
                            ctx.onToken("", false); // trigger render
                        return;
                    }
                    rawLlOutput_ += token; // capture every byte
                    if (ctx.raw)
                        rawOutput += token;
                    parser.feed(token, isFinal);
                });
        } catch (const std::exception &e) {
            return std::string("Error: ") + e.what();
        }

        if (!parser.waitForActions(
                std::chrono::seconds(config_.actionTimeoutSec))) {
            history_.push_back(
                "System: [TIMEOUT] actions did not complete within " +
                std::to_string(config_.actionTimeoutSec) + "s");
            break;
        }
        parser.flush();

        // Collect context feeds
        auto feeds = parser.contextFeeds();
        for (auto &feed : feeds)
            contextFeeds_.push_back(feed);

        // Determine completion
        auto results = parser.allResults();

        if (ctx.debug || ctx.verbose) {
            std::cerr << " | actions=" << results.size()
                      << " complete=" << taskComplete
                      << " resp=" << responseOutput_.size() << "b"
                      << " text=" << llmOutput.size() << "b";
            if (ctx.verbose && !llmOutput.empty()) {
                std::cerr << " \"" << llmOutput << "\"";
            }
            std::cerr << "\n";
        }

        if (results.empty() && !taskComplete) {
            // No parsed actions and no response tags — LLM produced bare text.
            // Inject one-time reminder into history so it persists in context.
            history_.push_back("Agent: " + rawLlOutput_);
            if (!bareTextReminded_ && !rawLlOutput_.empty()) {
                std::string trimmed = rawLlOutput_;
                size_t first = trimmed.find_first_not_of(" \t\n\r");
                if (first != std::string::npos && trimmed[first] != '<') {
                    bareTextReminded_ = true;
                    std::string preview = trimmed.size() > 60 ? trimmed.substr(0, 60) + "..." : trimmed;
                    history_.push_back(
                        "System: \342\232\240 BARE TEXT STRIPPED: \"" + preview + "\" — "
                        "Wrap ALL responses in <response final=\"true\">...</response>. "
                        "This reminder is sent once — it stays in your context.");
                }
            }
            taskComplete = true;
            if (responseOutput_.empty())
                responseOutput_ = sanitize(llmOutput);
        }

        // Capture iteration output for debugging
        {
            std::ostringstream os;
            os << "### LLM RAW OUTPUT\n\n" << rawLlOutput_ << "\n\n";
            if (!results.empty()) {
                os << "### TOOL RESULTS\n\n";
                for (auto &[id, r] : results) {
                    os << buildResultTag(id, r) << "\n";
                }
            }
            iterationOutputs_.push_back(os.str());
        }

        if (taskComplete) {
            fullResponse = responseOutput_;
            break;
        }

        // Prepare next iteration — push agent output, then system results
        history_.push_back("Agent: " + llmOutput);
        if (!results.empty()) {
            for (auto &[id, result] : results) {
                std::ostringstream sysMsg;
                bool ok =
                    result.isMember("success") && result["success"].asBool();
                sysMsg << buildResultTag(id, result, true);
                history_.push_back("System: " + sysMsg.str());
            }
        }
        parser.clearResults(); // prevent result leakage to next iteration
        tickContextCycles();   // decrement peek cycles; auto-evict at 0
    }

    if (!ctx.ephemeral && !ctx.sessionId.empty()) {
        saveSession(ctx.sessionId);
    }

    if (ctx.raw && !rawOutput.empty()) {
        return rawOutput;
    }
    return sanitize(fullResponse);
}

// ═══════════════════════════════════════════════════════════════════════
// Prompt Building
// ═══════════════════════════════════════════════════════════════════════

ChatMessages Agent::buildChatPrompt(const AgentContext &ctx) const {
    ChatMessages msgs;
    msgs.push_back(ChatMessage::system(buildSystemPrompt(ctx)));
    return msgs;
}

std::string Agent::buildSystemPrompt(const AgentContext &ctx) const {
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
    ss << "  <info name=\"" << xmlAttr(config_.name) << "\" version=\""
       << xmlAttr(config_.version) << "\"/>\n";
    ss << "</harness>\n\n";

    // ═══ <system> — persona, tools, relics, context ═══
    ss << "<system>\n";

    // Skip persona block when empty — the harness already provides
    // the agent's identity and behavioral contract (v6 IDENTITY section).
    if (!systemPrompt_.empty()) {
        ss << "  <persona>\n";
        ss << systemPrompt_ << "\n";
        ss << "  </persona>\n";
    }

    ss << "  <tools>\n    <description>Available functions — call with <action type=\"tool\">. Each tool has a name and parameters.</description>\n";
    auto schemaIt = env_.find("__TOOL_SCHEMAS__");
    bool hasSchemas = (schemaIt != env_.end() && !schemaIt->second.empty());
    if (hasSchemas) {
        ss << schemaIt->second << "\n";
    }
    for (const auto &[name, tool] : tools_) {
        // Only emit tools NOT already covered by schemas
        if (hasSchemas && schemaIt->second.find("name=\"" + name + "\"") != std::string::npos)
            continue;
        ss << "     <tool name=\"" << xmlAttr(name) << "\"";
        if (!tool.description.empty())
            ss << " desc=\"" << xmlAttr(tool.description) << "\"";
        ss << "/>\n";
    }
    ss << " </tools>\n";

    if (!relics_.empty()) {
        ss << "  <relics>\n    <description>Persistent stores — read/write session state, checkpoints, journals.</description>\n";
        for (auto &name : relics_) {
            auto it = BUILTIN_RELICS.find(name);
            if (it != BUILTIN_RELICS.end())
                ss << "    <relic name=\"" << xmlAttr(name) << "\" endpoints=\""
                   << it->second << "\"/>\n";
        }
        ss << "  </relics>\n";
    }

    if (!subAgents_.empty()) {
        ss << "  <subagents>\n    <description>Delegatable agents — call with <action type=\"agent\">.</description>\n";
        for (const auto &[name, agent] : subAgents_) {
            ss << "    <agent name=\"" << xmlAttr(name) << "\"";
            if (!agent->config().summary.empty())
                ss << " summary=\"" << xmlAttr(agent->config().summary) << "\"";
            ss << "/>\n";
        }
        ss << "  </subagents>\n";
    }

    auto wfIt = env_.find("__WORKFLOW_XML__");
    if (wfIt != env_.end() && !wfIt->second.empty()) {
        ss << wfIt->second << "\n";
    }

    ss << "  <cwd>" << std::filesystem::current_path().string() << "</cwd>\n";
    ss << "</system>\n\n";

    // ═══ DYNAMIC: pinned context — files the LLM asked to keep in view ═══
    if (!pinned_.empty()) {
        ss << "<pinned_context>\n"
              "  <description>Files the agent pinned via context_pin. "
              "Persist until context_unpin.</description>\n";
        for (auto &[key, e] : pinned_) {
            ss << "  <file path=\"" << xmlAttr(e.displayPath)
               << "\" bytes=\"" << e.bytes << "\">\n";
            ss << e.content;
            if (!e.content.empty() && e.content.back() != '\n') ss << '\n';
            ss << "  </file>\n";
        }
        ss << "</pinned_context>\n\n";
    }

    // ═══ DYNAMIC: ephemeral context — peek entries with remaining cycles ═══
    if (!peeking_.empty()) {
        ss << "<ephemeral_context>\n"
              "  <description>Files peeked via context_peek. Auto-evicted "
              "after their cycle count expires.</description>\n";
        for (auto &[key, e] : peeking_) {
            ss << "  <file path=\"" << xmlAttr(e.displayPath)
               << "\" bytes=\"" << e.bytes
               << "\" cycles_remaining=\"" << e.cyclesRemaining << "\">\n";
            ss << e.content;
            if (!e.content.empty() && e.content.back() != '\n') ss << '\n';
            ss << "  </file>\n";
        }
        ss << "</ephemeral_context>\n\n";
    }

    // ═══ DYNAMIC: feeds — only poll feeds explicitly imported ═══
    if (!feeds_.empty()) {
        auto feedResults = feeds::FeedEngine::instance().pollAll();
        bool any = false;
        for (auto &fr : feedResults) {
            if (!feeds_.count(fr.name))
                continue;
            if (!any) {
                ss << "<feeds>\n  <description>System context refreshed each turn — clock, stats, working dir, git.</description>\n";
                any = true;
            }
            ss << "  <" << fr.name << ">\n";
            ss << "  " << fr.summary << "\n";
            ss << "  </" << fr.name << ">\n";
        }
        if (any)
            ss << "</feeds>\n\n";
    }

    // ═══ DYNAMIC: context_feeds — LLM-requested from prior iterations ═══
    if (!contextFeeds_.empty()) {
        ss << "<context_feeds>\n  <description>LLM-requested context from prior turns.</description>\n";
        for (auto &feed : contextFeeds_) {
            ss << "  " << feed << "\n";
        }
        ss << "</context_feeds>\n\n";
    }

    // ═══ DYNAMIC: history — conversation turns ═══
    ss << "<history>\n  <description>Prior conversation between User and Agent.</description>\n";

    // Apply history cap — only include the most recent N entries
    size_t histStart = 0;
    if (config_.historyCap > 0 && history_.size() > (size_t)config_.historyCap) {
        histStart = history_.size() - config_.historyCap;
    }

    bool open = false;
    for (size_t hi = histStart; hi < history_.size(); hi++) {
        const auto &h = history_[hi];
        if (h.rfind("User: ", 0) == 0) {
            if (open)
                ss << "  </turn>\n";
            ss << "  <turn role=\"user\">\n";
            std::istringstream uiss(h.substr(6));
            std::string ul;
            while (std::getline(uiss, ul))
                ss << "    " << ul << "\n";
            open = true;
        } else if (h.rfind("Agent: ", 0) == 0) {
            if (open)
                ss << "  </turn>\n";
            ss << "  <turn role=\"agent\">\n";
            std::istringstream aiss(h.substr(7));
            std::string al;
            while (std::getline(aiss, al))
                ss << "    " << al << "\n";
            open = true;
        } else if (h.rfind("System: ", 0) == 0) {
            if (open)
                ss << "  </turn>\n";
            ss << "  <turn role=\"system\">\n";
            std::istringstream siss(h.substr(8));
            std::string sl;
            while (std::getline(siss, sl))
                ss << "    " << sl << "\n";
            open = true;
        }
    }
    if (open)
        ss << "  </turn>\n";
    ss << "</history>\n";

    return ss.str();
}

std::string Agent::buildUserPrompt(const AgentContext &ctx) const {
    return ctx.userInput;
}

// ═══════════════════════════════════════════════════════════════════════
// Sanitize output — strip protocol XML tags
// ═══════════════════════════════════════════════════════════════════════

std::string Agent::sanitize(const std::string &output) {
    // Linear state-machine tag stripper — 10-20x faster than regex on large
    // outputs
    static const std::vector<std::string> tags = {"action", "result", "thought",
                                                  "context_feed", "response"};
    std::string out;
    out.reserve(output.size());
    size_t i = 0, n = output.size();
    while (i < n) {
        if (output[i] != '<') {
            out += output[i++];
            continue;
        }
        bool matched = false;
        for (auto &tag : tags) {
            size_t tagLen = tag.size();
            // <tag> or </tag>
            bool isClose = (i + 1 < n && output[i + 1] == '/');
            size_t nameStart = isClose ? i + 2 : i + 1;
            if (n - nameStart >= tagLen &&
                output.compare(nameStart, tagLen, tag) == 0) {
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
    return (start == std::string::npos) ? ""
                                        : out.substr(start, end - start + 1);
}

// ── Tool dispatch — see agent_tools.cpp
// ── Session lifecycle — see agent_session.cpp

} // namespace cortex::mk3
