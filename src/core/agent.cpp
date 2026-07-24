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

// Vet-fix: harness resolver. Searches a deterministic list of roots
// (any cwd, any host) so the agent never falls back to a hardcoded
// developer-machine absolute path. The order favours:
//   1. exact path the manifest loader resolved to (config_.harnessPath)
//   2. $CORTEX_HOME/manifests/harness/<relative>
//   3. ./manifests/harness/<relative>      (cwd-relative; standard repo layout)
//   4. ~/.config/cortex-mk3/manifests/harness/<relative>  (installed layout)
// If none match and the relative hint is empty, fall back to default.md in
// the same roots, in the same order. Returns empty string when nothing
// resolves; caller throws.
static std::string findHarnessPath(const std::string& fromManifest,
                                 std::vector<std::string>& looked) {
    auto tryOpen = [](const std::string& cand, std::vector<std::string>& looked) -> std::string {
        looked.push_back(cand);
        std::ifstream f(cand);
        if (f.good()) return cand;
        return {};
    };
    auto appendIf = [](std::string base, const std::string& tail) -> std::string {
        if (base.empty()) return tail;
        if (base.back() != '/') base += '/';
        return base + tail;
    };
    const std::string hintRel = [&]() -> std::string {
        if (fromManifest.empty()) return "default.md";
        std::string stem = fromManifest;
        size_t slash = stem.find_last_of('/');
        if (slash != std::string::npos) stem = stem.substr(slash + 1);
        size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        if (stem.empty()) return "default.md";
        return stem + ".md";
    }();
    auto tryRoot = [&](const std::string& root) -> std::string {
        std::string cand = appendIf(root, appendIf("manifests/harness", hintRel));
        return tryOpen(cand, looked);
    };
    auto tryRootDefault = [&](const std::string& root) -> std::string {
        std::string cand = appendIf(root, "manifests/harness/default.md");
        return tryOpen(cand, looked);
    };
    // Compute the FHS-install prefix path exactly once and cache; called
    // in two places (specific + default.md fallback). Each call returns
    // either the resolved path or empty; populates `looked` so the
    // error message below tells the operator where we looked. Static
    // helper — no IIFE recursion, no self-capture UB path.
    auto tryFhsInstall = [&](const std::string& suffix) -> std::string {
        std::error_code ec;
        auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (ec || self.empty()) return {};
        std::string p = self.string();
        size_t slash = p.find_last_of('/');
        if (slash == std::string::npos) return {};
        std::string bindir = p.substr(0, slash);
        size_t last = bindir.find_last_of('/');
        if (last == std::string::npos) return {};
        std::string prefix = bindir.substr(0, last);
        std::string cand = prefix + "/share/cortex-mk3/" + suffix;
        looked.push_back(cand);
        std::ifstream f(cand);
        if (f.good()) return cand;
        return {};
    };
    auto suffixFor = [&](const std::string& relOrDefault) -> std::string {
        return std::string("manifests/harness/") + relOrDefault;
    };
    // 1. Exactly what the manifest loader provided first.
    if (!fromManifest.empty() && fromManifest.find("default.md") == std::string::npos) {
        if (auto r = tryOpen(fromManifest, looked); !r.empty()) return r;
    }
    // 2. CORTEX_HOME
    if (const char* home = std::getenv("CORTEX_HOME"); home && *home) {
        if (auto r = tryRoot(home); !r.empty()) return r;
    }
    // 2'. Exe-relative: the binary lives at <install>/cortex-mk3 and
    // shares an install tree with manifests/. When launched from any
    // cwd (e.g. ~) this is the only reliable way to find the harness
    // without a CORTEX_HOME hint.
    try {
        std::error_code ec;
        auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec && !self.empty()) {
            std::string exe = self.string();
            size_t slash = exe.find_last_of('/');
            if (slash != std::string::npos) {
                std::string exeDir = exe.substr(0, slash);
                if (auto r = tryRoot(exeDir); !r.empty()) return r;
            }
        }
    } catch (...) {
    }
    // 2''. FHS-style install root: <prefix>/share/cortex-mk3/manifests/...
    // for binaries installed via pacman / apt / a future install script.
    if (auto r = tryFhsInstall(suffixFor(hintRel)); !r.empty()) return r;
    // 3. cwd-relative (any cwd, not hardcoded developer box)
    tryRoot(std::filesystem::current_path().string());
    // 4. ~/.config/cortex-mk3 (installed layout)
    if (const char* home = std::getenv("HOME"); home && *home) {
        tryRoot(std::string(home) + "/.config/cortex-mk3");
    }
    // Final fallback: default.md in same roots, in the same order.
    if (hintRel != "default.md") {
        if (const char* home = std::getenv("CORTEX_HOME"); home && *home) {
            if (auto r = tryRootDefault(home); !r.empty()) return r;
        }
        // Same FHS install sibling fallback for default.md.
        if (auto r = tryFhsInstall(suffixFor("default.md")); !r.empty()) return r;
        // Same exe-dir fallback as above for default.md.
        try {
            std::error_code ec;
            auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
            if (!ec && !self.empty()) {
                std::string exe = self.string();
                size_t slash = exe.find_last_of('/');
                if (slash != std::string::npos) {
                    std::string exeDir = exe.substr(0, slash);
                    if (auto r = tryRootDefault(exeDir); !r.empty()) return r;
                }
            }
        } catch (...) {
        }
        tryRootDefault(std::filesystem::current_path().string());
        if (const char* home = std::getenv("HOME"); home && *home) {
            tryRootDefault(std::string(home) + "/.config/cortex-mk3");
        }
    }
    return {};
}

static std::string indentText(const std::string &text, int spaces) {
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
    provider_->setMaxTokens(config_.maxTokens > 0 ? config_.maxTokens
                                                  : provider_->getMaxTokens());
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
    //
    // Vet-fix: harness resolution must NOT hardcode /home/mlamkadm.
    // The previous fallback was a developer-machine absolute path that
    // crashed on any other host and silently mis-resolved on the dev's
    // host when the build tree moved. Operator wants harness to load
    // consistently from $CORTEX_HOME -> ./manifests -> ~/.config lookups
    // regardless of where the agent was compiled or run.
    {
        std::vector<std::string> looked;
        std::string resolved = findHarnessPath(config_.harnessPath, looked);
        if (resolved.empty()) {
            std::string routes;
            for (std::size_t i = 0; i < looked.size(); ++i) {
                if (i) routes += "\n  ";
                routes += looked[i] + (i + 1 < looked.size() ? " (miss)" : "");
            }
            throw std::runtime_error(
                "harness prompt not found — searched:\n  " + routes +
                "\nUse --manifest-dir <path> or set CORTEX_HOME. Default fallback is manifests/harness/default.md");
        }
        config_.harnessPath = resolved;  // remember what we resolved to
        std::ifstream hf(resolved);
        if (hf.is_open()) {
            std::ostringstream oss;
            std::string line;
            while (std::getline(hf, line))
                oss << "    " << line << "\n";
            harnessText_ = oss.str();
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

std::string Agent::prompt(const std::string &input,
                          const std::string &sessionId, bool ephemeral,
                          PromptSource source, const std::string &sourceName) {
    return prompt(input, nullptr, sessionId, ephemeral, source, sourceName);
}

std::string Agent::prompt(const std::string &input, StreamCallback onToken,
                          const std::string &sessionId, bool ephemeral,
                          PromptSource source, const std::string &sourceName) {
    AgentContext ctx;
    ctx.userInput = input;
    ctx.sessionId = sessionId;
    ctx.streaming = (onToken != nullptr);
    ctx.onToken = std::move(onToken);
    ctx.ephemeral = ephemeral;
    ctx.source = source;
    ctx.sourceName = sourceName;
    ctx.raw = raw_;
    ctx.verbose = verbose_;
    ctx.debug =
        (env_.count("__DEBUG_MODE__") && env_["__DEBUG_MODE__"] == "true") ||
        devMode_;
    lastSessionId_ = sessionId;

    if (!ephemeral && !sessionId.empty()) {
        loadSession(sessionId);
        loadStateCheckpoint(sessionId);
    }

    std::string result = runLoop(ctx);

    dumpSessionArtifacts();

    return result;
}

Json::Value Agent::inspectContext(int lastN) const {
    Json::Value r(Json::objectValue);
    r["success"] = true;
    r["name"] = config_.name;
    r["provider"] = config_.provider;
    r["model"] = config_.model;
    r["response_output"] = responseOutput_;
    r["thought_bytes"] = static_cast<Json::UInt64>(thoughtOutput_.size());
    r["raw_bytes"] = static_cast<Json::UInt64>(rawLlOutput_.size());
    r["protocol_events"] = static_cast<int>(protocolEvents_.size());
    r["sub_agents"] = Json::Value(Json::arrayValue);
    for (const auto &kv : subAgents_)
        r["sub_agents"].append(kv.first);

    Json::Value hist(Json::arrayValue);
    int start = 0;
    if (lastN > 0 && static_cast<int>(history_.size()) > lastN)
        start = static_cast<int>(history_.size()) - lastN;
    for (int i = start; i < static_cast<int>(history_.size()); ++i) {
        const std::string &h = history_[static_cast<size_t>(i)];
        Json::Value entry(Json::objectValue);
        if (h.rfind("User: ", 0) == 0) {
            entry["role"] = "user";
            entry["content"] = h.substr(6);
        } else if (h.rfind("Parent(", 0) == 0) {
            entry["role"] = "parent";
            auto close = h.find(')');
            if (close != std::string::npos && close + 2 <= h.size()) {
                entry["from"] = h.substr(7, close - 7);
                entry["content"] = h.substr(close + 2); // skip ") "
                if (!entry["content"].asString().empty() &&
                    entry["content"].asString()[0] == ' ')
                    entry["content"] = entry["content"].asString().substr(1);
            } else {
                entry["content"] = h;
            }
        } else if (h.rfind("Agent: ", 0) == 0) {
            entry["role"] = "agent";
            entry["content"] = h.substr(7);
        } else if (h.rfind("System: ", 0) == 0) {
            entry["role"] = "system";
            entry["content"] = h.substr(8);
        } else {
            entry["role"] = "other";
            entry["content"] = h;
        }
        hist.append(entry);
    }
    r["history"] = hist;
    r["history_total"] = static_cast<int>(history_.size());
    r["context"] = contextSnapshot();
    return r;
}

// ═══════════════════════════════════════════════════════════════════════
// Core Loop
static std::string stripModelOwnedRuntimeTags(const std::string &s) {
    static const std::regex responseRe(
        R"(<response\b[^>]*>[\s\S]*?</response>)");
    static const std::regex resultRe(R"(<result\b[^>]*>[\s\S]*?</result>)");
    return std::regex_replace(std::regex_replace(s, responseRe, ""), resultRe,
                              "");
}

static std::string
formatDelegatedTrace(const std::string &agentName,
                     const std::string &instruction,
                     const std::vector<std::string> &prompts,
                     const std::vector<std::string> &outputs) {
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

static bool jsonBool(const Json::Value &params, const std::string &key,
                     bool def = false) {
    if (!params.isObject() || !params.isMember(key))
        return def;
    const Json::Value &v = params[key];
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

static Json::Value makeSubAgentResult(const std::string &output,
                                      const std::string &trace,
                                      bool dumpContext) {
    Json::Value r;
    r["success"] = true;
    r["output"] = output;
    if (dumpContext)
        r["trace"] = trace;
    return r;
}

static std::vector<std::string> splitPath(const std::string &path) {
    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string item;
    while (std::getline(ss, item, '.')) {
        if (!item.empty())
            parts.push_back(item);
    }
    return parts;
}

static const Json::Value *
lookupResultPath(const std::map<std::string, Json::Value> &results,
                 const std::string &id, const std::string &path) {
    auto it = results.find(id);
    if (it == results.end())
        return nullptr;
    const Json::Value *cur = &it->second;
    for (const auto &part : splitPath(path)) {
        if (cur->isObject() && cur->isMember(part)) {
            cur = &((*cur)[part]);
        } else if (cur->isArray()) {
            char *end = nullptr;
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

static std::string jsonValueToInlineString(const Json::Value &v) {
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
    for (char &c : s) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok)
            c = '_';
    }
    return s;
}

static bool subAgentSessionPersistenceEnabled(const std::string &value) {
    return value == "session" || value == "sessions" || value == "persistent" ||
           value == "disk" || value == "true";
}

static std::string deriveSubAgentSessionId(const AgentContext &ctx,
                                           const AgentConfig &cfg,
                                           const std::string &agentName) {
    if (ctx.sessionId.empty())
        return "";
    if (!subAgentSessionPersistenceEnabled(cfg.subAgentPersistence))
        return "";
    return safeSessionPart(ctx.sessionId) + "__subagent__" +
           safeSessionPart(agentName);
}

static Json::Value expansionResultView(const Json::Value &result) {
    Json::Value view = result;
    if (result.isMember("output") && result["output"].isString()) {
        Json::Value parsed;
        Json::CharReaderBuilder r;
        std::string errs;
        std::istringstream ss(result["output"].asString());
        if (Json::parseFromStream(r, ss, &parsed, &errs)) {
            view["json"] = parsed;
            if (parsed.isObject()) {
                for (const auto &key : parsed.getMemberNames()) {
                    if (!view.isMember(key))
                        view[key] = parsed[key];
                }
            }
        }
    }
    return view;
}

static Json::Value
expandValueRefs(const Json::Value &value,
                const std::map<std::string, Json::Value> &results) {
    static const std::regex refRe(
        R"(\$\{([A-Za-z_][A-Za-z0-9_-]*)(?:\.([^}]+))?\})");

    if (value.isObject()) {
        Json::Value out(Json::objectValue);
        for (const auto &key : value.getMemberNames()) {
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
        const Json::Value *resolved = lookupResultPath(results, id, path);
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
        const Json::Value *resolved = lookupResultPath(results, id, path);
        out += resolved ? jsonValueToInlineString(*resolved) : m[0].str();
        start = m.suffix().first;
    }
    out.append(start, s.cend());
    return out;
}

static std::string buildResultTag(const std::string &id,
                                  const Json::Value &result,
                                  bool compact = false) {
    std::ostringstream os;
    bool ok = result.isMember("success") && result["success"].asBool();
    int exit = result.isMember("exit_code") ? result["exit_code"].asInt()
                                            : (ok ? 0 : -1);
    double ms =
        result.isMember("_elapsed_ms") ? result["_elapsed_ms"].asDouble() : 0;

    os << "<result id=\"" << id << "\" ok=\"" << (ok ? "true" : "false")
       << "\"";
    if (exit != 0)
        os << " exit=\"" << exit << "\"";
    if (ms > 0)
        os << " ms=\"" << std::fixed << std::setprecision(1) << ms << "\"";

    // Extract primary output body
    std::string body;
    for (const char *key : {"content", "output", "stdout", "result", "results",
                            "data", "value"}) {
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
    // Structured context_* results: serialize compact JSON so the LLM still
    // sees path/bytes/cycles in <result> tags (not an empty body).
    if (body.empty() && result.isObject()) {
        Json::Value slim;
        for (const char *k :
             {"success", "path", "mode", "bytes", "cycles_remaining",
              "pinned_count", "peek_count", "note", "error", "keys"}) {
            if (result.isMember(k))
                slim[k] = result[k];
        }
        if (!slim.empty()) {
            Json::StreamWriterBuilder w;
            w["indentation"] = "";
            body = Json::writeString(w, slim);
        }
    }

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
    // Continuation = this Agent already has transcript from a prior prompt().
    // Sub-agents are reused in-process; wiping protocolEvents_ here made every
    // drilldown show only the latest call, and broke the illusion (and the
    // contract) of a continuous non-ephemeral child conversation.
    const bool continuation = !history_.empty();
    rawLlOutput_.clear();
    responseOutput_.clear();
    thoughtOutput_.clear();
    iterationPrompts_.clear();
    iterationOutputs_.clear();
    subAgentTraces_.clear();
    if (!continuation) {
        protocolActions_.clear();
        protocolResults_.clear();
        protocolEvents_.clear();
    }
    // else: keep protocolEvents_/actions/results so nested TUI + inspect see
    // the full multi-prompt child timeline.

    // Push initiator input once at start (NOT per-iteration). Parent-agent
    // delegates are labeled so the child can distinguish them from the human.
    // Vet-fix: skip when the submitter pre-seeded history_ with the same
    // text — submitComposer can write a saveSession immediately on first
    // non-empty turn (so a TUI that crashes before prompt() runs still has
    // something on disk), but that gives the prompt path a duplicate User:
    // entry. Detect the trailing-equal-or-prefix and skip the second push.
    if (ctx.source == PromptSource::ParentAgent) {
        std::string from = ctx.sourceName.empty() ? "parent" : ctx.sourceName;
        history_.push_back("Parent(" + from + "): " + ctx.userInput);
    } else if (ctx.source == PromptSource::Internal) {
        history_.push_back("System: " + ctx.userInput);
    } else {
        // Vet-fix: submitComposer pre-seeds history_ with the same user
        // text so a TUI exit before this method runs still lands the
        // typed prompt on disk. Skip a trailing-equal User: line to
        // avoid duplicating records on the next prompt.
        const std::string needle = "User: " + ctx.userInput;
        bool alreadyLast = !history_.empty() && history_.back() == needle;
        if (!alreadyLast) {
            history_.push_back(needle);
        }
    }

    // Parser lives across iterations — usedActionIds_ and finalResponseSeen_
    // persist so duplicate-ID and post-final enforcement works cross-turn.
    protocol::Parser parser;

    // Completion policy: how bare / non-final model output is handled.
    // Derived from runtime.mode + optional runtime.completion_policy.
    enum class CompPolicy { Recover, Promote, Strict };
    auto resolveCompPolicy = [&]() -> CompPolicy {
        if (config_.completionPolicy == "strict")
            return CompPolicy::Strict;
        if (config_.completionPolicy == "promote")
            return CompPolicy::Promote;
        if (config_.completionPolicy == "recover")
            return CompPolicy::Recover;
        if (config_.runtimeMode == "autonomous")
            return CompPolicy::Promote;
        return CompPolicy::Recover; // normal default
    };
    const CompPolicy compPolicy = resolveCompPolicy();
    const int promoteAfter =
        config_.bareRecoveryPromoteAfter >= 0
            ? config_.bareRecoveryPromoteAfter
            : (compPolicy == CompPolicy::Promote ? 2 : 1000000);
    int bareRecoveryCount = 0;
    std::string lastSalvage; // best non-final content this turn (for promote)

    auto trimCopy = [](std::string s) {
        size_t a = s.find_first_not_of(" \t\n\r");
        if (a == std::string::npos)
            return std::string();
        size_t b = s.find_last_not_of(" \t\n\r");
        return s.substr(a, b - a + 1);
    };
    auto pickSalvage = [&](const std::string &raw,
                           const std::string &responseBody) {
        // Prefer structured response body (non-final <response>) over raw
        // stream.
        std::string r = trimCopy(responseBody);
        if (!r.empty())
            return r;
        return trimCopy(raw);
    };
    auto buildRecoveryCorrection = [&](const std::string &salvage,
                                       bool nonFinalResponse) -> std::string {
        std::ostringstream os;
        os << "[PROTOCOL RECOVERY] Previous model output had no valid "
              "<response final=\"true\"> and no executable <action>.\n";
        if (nonFinalResponse)
            os << "A <response> body was seen without final=\"true\". "
                  "Re-emit it wrapped correctly.\n";
        else
            os << "Bare / non-protocol text is invisible to the operator as a "
                  "final answer.\n";
        os << "\nSalvaged content — put this inside <response final=\"true\"> "
              "(edit if needed) OR continue with an <action>:\n"
              "----- BEGIN SALVAGE -----\n";
        // Cap injection so a huge bare dump cannot blow the next prompt.
        const size_t kMax = 12000;
        if (salvage.size() > kMax) {
            os << salvage.substr(0, kMax) << "\n…[truncated "
               << (salvage.size() - kMax) << " bytes]";
        } else {
            os << salvage;
        }
        os << "\n----- END SALVAGE -----\n\n"
              "Emit EXACTLY one of:\n"
              "  <response final=\"true\">…</response>\n"
              "  <action type=\"tool\" name=\"…\" id=\"…\">…</action>\n";
        return os.str();
    };

    const int workCap = std::max(1, config_.iterationCap);
    bool finalizationTurn = false;
    bool finalizationDone = false;
    std::string limitReason; // set when we enter finalization due to a cap

    auto emitStatus = [&](const std::string &text) {
        history_.push_back("System: " + text);
        protocolEvents_.push_back({ProtocolEventKind::STATUS, text, {}, {}});
        // Heartbeat so TUI drains the STATUS event mid-turn.
        if (ctx.onToken)
            ctx.onToken("", false);
    };

    // Work turns 1..workCap, then at most one FINALIZATION turn (tools
    // disabled) so the model always gets an honest last chance to emit
    // final=true.
    for (ctx.iteration = 1;; ctx.iteration++) {
        if (!g_running) {
            fullResponse = "[cancelled]";
            emitStatus("[LIMIT] cancelled by operator (Ctrl-C / stop).");
            break;
        }

        if (!finalizationTurn && ctx.iteration > workCap) {
            // Exhausted work budget without a final response → dedicated
            // finalization turn (does not consume another "work" slot).
            finalizationTurn = true;
            ctx.iteration = workCap + 1;
            limitReason = "max_iterations=" + std::to_string(workCap);
            emitStatus("[LIMIT] " + limitReason +
                       " reached without <response final=\"true\">. "
                       "Entering FINALIZATION turn — tools disabled; emit "
                       "final reply now.");
            emitStatus("[FINALIZE] This is your last turn. Output ONLY:\n"
                       "  <response final=\"true\">…your answer…</response>\n"
                       "Do not call tools. Do not emit bare text.");
        }

        if (finalizationTurn && finalizationDone)
            break;

        ChatMessages msgs = buildChatPrompt(ctx);
        // Save full prompt for /prompts toggle
        lastPrompt_ = msgs.size() > 0 ? msgs[0].content : "";
        {
            std::ostringstream pd;
            for (size_t i = 0; i < msgs.size(); i++) {
                const char *role = ChatMessage::roleName(msgs[i].role);
                if (msgs[i].role == ChatRole::SYSTEM) {
                    if (i == 0) {
                        pd << msgs[i].content;
                        if (!msgs[i].content.empty() &&
                            msgs[i].content.back() != '\n')
                            pd << '\n';
                    } else {
                        pd << "<dynamic_context role=\"system\">\n"
                           << msgs[i].content;
                        if (!msgs[i].content.empty() &&
                            msgs[i].content.back() != '\n')
                            pd << '\n';
                        pd << "</dynamic_context>\n";
                    }
                } else if (msgs[i].role == ChatRole::USER) {
                    pd << "<user current=\"true\" iteration=\"" << ctx.iteration
                       << "\"";
                    if (!ctx.sessionId.empty())
                        pd << " session=\"" << xmlAttr(ctx.sessionId) << "\"";
                    pd << ">\n" << msgs[i].content;
                    if (!msgs[i].content.empty() &&
                        msgs[i].content.back() != '\n')
                        pd << '\n';
                    pd << "</user>\n";
                } else {
                    pd << "<message role=\"" << role << "\">\n"
                       << msgs[i].content;
                    if (!msgs[i].content.empty() &&
                        msgs[i].content.back() != '\n')
                        pd << '\n';
                    pd << "</message>\n";
                }
            }
            iterationPrompts_.push_back(pd.str());
        }

        // Soft warning on last WORK turn (tools still allowed).
        if (!finalizationTurn && ctx.iteration == workCap) {
            msgs.push_back(ChatMessage::user(
                "[LIMIT WARNING] This is work iteration " +
                std::to_string(workCap) + "/" + std::to_string(workCap) +
                ". After this turn the runtime will force a FINALIZATION turn "
                "with tools disabled. Prefer <response final=\"true\"> now if "
                "you have enough evidence; otherwise finish critical tools "
                "quickly."));
        }
        // Hard finalization prompt — no tools, must close.
        if (finalizationTurn) {
            std::ostringstream fin;
            fin << "[FINALIZATION TURN] " << limitReason
                << " exhausted. Tools are DISABLED this turn.\n"
                << "Emit exactly:\n"
                << "<response final=\"true\">\n"
                << "…concise answer from evidence already gathered…\n"
                << "</response>\n";
            if (!lastSalvage.empty()) {
                fin << "\nIf useful, you may reuse this salvaged draft:\n"
                    << "----- BEGIN SALVAGE -----\n";
                const size_t kMax = 8000;
                if (lastSalvage.size() > kMax)
                    fin << lastSalvage.substr(0, kMax) << "\n…[truncated]";
                else
                    fin << lastSalvage;
                fin << "\n----- END SALVAGE -----\n";
            }
            msgs.push_back(ChatMessage::user(fin.str()));
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
            [this, &ctx](const protocol::ParsedAction &action,
                         const std::string &instruction) -> Json::Value {
            const std::string &agentName = action.name;
            auto it = subAgents_.find(agentName);
            if (it == subAgents_.end()) {
                Json::Value err;
                err["success"] = false;
                err["error"] = "Unknown sub-agent: " + agentName;
                return err;
            }

            // op: prompt (default) | inspect | context | history
            // XML attrs land in params (parser extra-attr path), e.g.
            //   <action type="agent" name="reader" op="inspect" last_n="10"/>
            //   <action type="agent" name="reader" inspect="true"/>
            std::string op = "prompt";
            if (action.params.isMember("op") &&
                action.params["op"].isString() &&
                !action.params["op"].asString().empty())
                op = action.params["op"].asString();
            else if (action.params.isMember("inspect")) {
                const auto &iv = action.params["inspect"];
                if ((iv.isBool() && iv.asBool()) ||
                    (iv.isString() &&
                     (iv.asString() == "true" || iv.asString() == "1")) ||
                    (iv.isInt() && iv.asInt() != 0))
                    op = "inspect";
            }

            if (op == "inspect" || op == "context" || op == "history") {
                int lastN = action.params.get("last_n", 20).asInt();
                if (lastN <= 0)
                    lastN = 20;
                Json::Value snap = it->second->inspectContext(lastN);
                snap["op"] = op;
                snap["agent"] = agentName;
                // Compact summary for the RESULT card body.
                std::ostringstream sum;
                sum << agentName << " context: history="
                    << snap.get("history_total", 0).asInt()
                    << " events=" << snap.get("protocol_events", 0).asInt();
                if (!snap.get("response_output", "").asString().empty()) {
                    std::string ro = snap["response_output"].asString();
                    if (ro.size() > 200)
                        ro = ro.substr(0, 200) + "…";
                    sum << "\nlast: " << ro;
                }
                snap["output"] = sum.str();
                snap["success"] = true;
                return snap;
            }

            bool forceEphemeral = jsonBool(action.params, "ephemeral", false);
            bool dumpContext = jsonBool(action.params, "dump_context", false);
            std::string childSessionId =
                forceEphemeral
                    ? ""
                    : deriveSubAgentSessionId(ctx, config_, agentName);
            // Ephemeral child calls must not leak prior in-memory history into
            // this mission (non-ephemeral reuses the same Agent object).
            if (forceEphemeral) {
                it->second->clearHistory();
            }
            // Stream the child's progress to the parent's UI so it stays alive
            // (byte counter + spinner) during the synchronous sub-agent call.
            // Without this, the child runs for seconds with zero UI updates —
            // the 'freeze after the first thought block ends' symptom. The
            // child's generateStream calls ctx.onToken("") as a heartbeat but
            // the actual bytes land in the CHILD's rawLlOutput_, so we forward
            // the child's raw delta through the parent's onToken. The parent's
            // onToken publishes non-empty tokens directly (see runAgentTurn).
            Agent *childPtr = it->second.get();
            std::shared_ptr<size_t> childSeen = std::make_shared<size_t>(0);
            StreamCallback childProgress = [childPtr, childSeen,
                                            &ctx](const std::string &, bool) {
                if (!ctx.onToken)
                    return;
                const std::string &r = childPtr->rawLlOutput();
                if (r.size() > *childSeen) {
                    ctx.onToken(r.substr(*childSeen), false);
                    *childSeen = r.size();
                }
            };
            // Further prompts reuse the child session id → continuous history.
            // Label source as ParentAgent so the child sees who asked.
            std::string result =
                childSessionId.empty()
                    ? it->second->prompt(
                          instruction, childProgress, "", forceEphemeral,
                          PromptSource::ParentAgent, config_.name)
                    : it->second->prompt(
                          instruction, childProgress, childSessionId, false,
                          PromptSource::ParentAgent, config_.name);
            std::string trace;
            if (dumpContext) {
                trace = formatDelegatedTrace(agentName, instruction,
                                             it->second->iterationPrompts(),
                                             it->second->iterationOutputs());
                subAgentTraces_.push_back(trace);
            }
            return makeSubAgentResult(result, trace, dumpContext);
        };

        // Wire workflow execution — creates a WorkflowRuntime with tool + agent
        // callbacks
        d.workflowDelegate =
            [this,
             &ctx](const std::string &workflowName,
                   const Json::Value &params) -> workflows::WorkflowResult {
            auto wf =
                workflows::WorkflowEngine::instance().getCached(workflowName);
            workflows::WorkflowRuntime rt;

            // Tool callback: dispatch a tool by name with params
            rt.executeTool = [this](const std::string &name,
                                    const Json::Value &p) -> Json::Value {
                protocol::ParsedAction a;
                a.name = name;
                a.type = protocol::ActionType::TOOL;
                a.params = p;
                return dispatchTool(a);
            };

            // Agent callback: delegate to a sub-agent. Honors the
            // ephemeral / dump_context modifiers from the workflow step so
            // workflow agent actions match the behavior of direct
            // <action type="agent" ...> actions.
            rt.executeAgent =
                [this, &ctx](const workflows::WorkflowAgentInvocation &inv)
                -> Json::Value {
                auto it = subAgents_.find(inv.name);
                if (it == subAgents_.end()) {
                    Json::Value err;
                    err["success"] = false;
                    err["error"] = "Unknown sub-agent: " + inv.name;
                    return err;
                }
                std::string childSessionId =
                    inv.ephemeral
                        ? std::string()
                        : deriveSubAgentSessionId(ctx, config_, inv.name);
                std::string result =
                    childSessionId.empty()
                        ? it->second->prompt(inv.instruction, "", inv.ephemeral)
                        : it->second->prompt(inv.instruction, childSessionId,
                                             false);
                if (inv.dumpContext) {
                    std::string trace =
                        formatDelegatedTrace(inv.name, inv.instruction,
                                             it->second->iterationPrompts(),
                                             it->second->iterationOutputs());
                    subAgentTraces_.push_back(trace);
                    Json::Value r;
                    r["success"] = true;
                    r["output"] = result;
                    r["trace"] = trace;
                    return r;
                }
                Json::Value r;
                r["success"] = true;
                r["output"] = result;
                return r;
            };

            // Slice 2: human callback — defaults to the step's default value.
            rt.executeHuman = [](const std::string &id,
                                 const Json::Value &prompt) -> Json::Value {
                (void)id;
                return prompt.isMember("default")
                           ? Json::Value(prompt["default"])
                           : Json::Value("");
            };

            // Slice 2: relic callback — uses Reliquary singleton.
            rt.executeRelic = [](const std::string &name,
                                 const std::string &action,
                                 const Json::Value &params) -> Json::Value {
                auto &rel = relics::Reliquary::instance();
                if (!rel.has(name)) {
                    Json::Value err;
                    err["success"] = false;
                    err["error"] = "unknown relic: " + name;
                    return err;
                }
                auto result = rel.dispatch(name, action, params);
                Json::Value out;
                out["success"] = result.success;
                if (!result.error.empty())
                    out["error"] = result.error;
                if (!result.data.isNull())
                    out["data"] = result.data;
                return out;
            };

            // Slice 2: feed callback — uses FeedEngine singleton.
            // If step.action is set, calls it as a feed tool.
            // Otherwise refreshes the feed and returns the latest value.
            rt.executeFeed = [](const std::string &name,
                                const Json::Value &query) -> Json::Value {
                auto &eng = feeds::FeedEngine::instance();
                std::string action =
                    query.isMember("action") ? query["action"].asString() : "";
                if (!action.empty())
                    return eng.callFeedTool(name, action, query);
                auto *feed = const_cast<feeds::Feed *>(eng.getFeed(name));
                if (!feed) {
                    Json::Value err;
                    err["success"] = false;
                    err["error"] = "unknown feed: " + name;
                    return err;
                }
                auto result = feed->refresh();
                Json::Value out;
                out["success"] = result.ok;
                out["name"] = result.name;
                out["summary"] = result.summary;
                out["json"] = result.json;
                return out;
            };

            // Slice 2: emit — no-op by default
            rt.executeEmit = [](const std::string &event,
                                const Json::Value &payload) {
                (void)event;
                (void)payload;
            };

            // Slice 4: checkpoint — uses the agent's CheckpointHandler if set.
            rt.executeCheckpoint = [this](const std::string &id,
                                          const Json::Value &state) {
                if (checkpointHandler_)
                    checkpointHandler_(id, state);
            };

            // Slice 6: parallel_race — fire all in parallel, first to succeed
            // wins. Implementation: launch all steps on background threads,
            // wait for the first success, then return that result. Losers
            // continue running in the background but their results are
            // discarded.
            rt.executeParallelRace =
                [this, &rt](const std::vector<workflows::WorkflowStep> &steps,
                            const std::map<std::string, Json::Value> &symbols)
                -> Json::Value {
                std::mutex mtx;
                std::condition_variable cv;
                Json::Value winner(Json::objectValue);
                std::atomic<bool> hasWinner{false};
                std::vector<std::thread> workers;
                workers.reserve(steps.size());
                for (const auto &s : steps) {
                    workers.emplace_back([&, s]() {
                        Json::Value p;
                        for (const auto &k : s.params.getMemberNames())
                            p[k] = s.params[k];
                        Json::Value r;
                        if (s.type == "tool" && rt.executeTool)
                            r = rt.executeTool(s.tool, p);
                        else if (s.type == "agent" && rt.executeAgent) {
                            workflows::WorkflowAgentInvocation inv;
                            inv.name = s.agent;
                            inv.instruction = p.toStyledString();
                            r = rt.executeAgent(inv);
                        } else {
                            r["success"] = true;
                            r["note"] = "no runtime for type=" + s.type;
                        }
                        // Was the winner taken while we were running?
                        if (!hasWinner.load() && r.isObject() &&
                            r.get("success", false).asBool()) {
                            std::lock_guard<std::mutex> lock(mtx);
                            if (!hasWinner.load()) {
                                winner = r;
                                winner["_winner_id"] = s.id;
                                hasWinner.store(true);
                                cv.notify_all();
                            }
                        }
                    });
                }
                // Wait briefly for the first success (1s timeout for now).
                {
                    std::unique_lock<std::mutex> lock(mtx);
                    cv.wait_for(lock, std::chrono::milliseconds(1000),
                                [&] { return hasWinner.load(); });
                }
                Json::Value out(Json::objectValue);
                if (hasWinner.load()) {
                    out["winner"] = winner;
                    out["race_strategy"] = "first-success";
                } else {
                    out["winner"] = Json::Value(Json::objectValue);
                    out["race_strategy"] = "timeout-no-winner";
                }
                // Detach workers — they finish in the background; results
                // discarded
                for (auto &w : workers)
                    w.detach();
                return out;
            };

            // Recursive workflow call — builds its own runtime, not a copy of
            // rt
            rt.executeWorkflow =
                [this,
                 &ctx](const std::string &name,
                       const Json::Value &p) -> workflows::WorkflowResult {
                auto subWf =
                    workflows::WorkflowEngine::instance().getCached(name);
                workflows::WorkflowRuntime subRt;
                subRt.executeTool =
                    [this](const std::string &tn,
                           const Json::Value &tp) -> Json::Value {
                    protocol::ParsedAction a;
                    a.name = tn;
                    a.type = protocol::ActionType::TOOL;
                    a.params = tp;
                    return dispatchTool(a);
                };
                subRt.executeAgent =
                    [this, &ctx](const workflows::WorkflowAgentInvocation &inv)
                    -> Json::Value {
                    auto it = subAgents_.find(inv.name);
                    if (it == subAgents_.end()) {
                        Json::Value err;
                        err["success"] = false;
                        err["error"] = "Unknown sub-agent: " + inv.name;
                        return err;
                    }
                    std::string childSessionId =
                        inv.ephemeral
                            ? std::string()
                            : deriveSubAgentSessionId(ctx, config_, inv.name);
                    std::string result =
                        childSessionId.empty()
                            ? it->second->prompt(inv.instruction, "",
                                                 inv.ephemeral)
                            : it->second->prompt(inv.instruction,
                                                 childSessionId, false);
                    if (inv.dumpContext) {
                        std::string trace = formatDelegatedTrace(
                            inv.name, inv.instruction,
                            it->second->iterationPrompts(),
                            it->second->iterationOutputs());
                        subAgentTraces_.push_back(trace);
                        Json::Value r;
                        r["success"] = true;
                        r["output"] = result;
                        r["trace"] = trace;
                        return r;
                    }
                    Json::Value r;
                    r["success"] = true;
                    r["output"] = result;
                    return r;
                };
                return workflows::WorkflowEngine::instance().execute(subWf,
                                                                     subRt, p);
            };

            return workflows::WorkflowEngine::instance().execute(wf, rt,
                                                                 params);
        };

        std::string iterationRawOutput;
        std::string iterationRuntimeOutput;

        parser.setExecutor(
            [this, &d, &ctx, &iterationRuntimeOutput, finalizationTurn](
                const protocol::ParsedAction &action) -> Json::Value {
                // Finalization turn: refuse all side-effecting actions so the
                // model must close with <response final="true">.
                if (finalizationTurn) {
                    Json::Value denied;
                    denied["success"] = false;
                    denied["error"] =
                        "finalization turn: actions disabled — emit "
                        "<response final=\"true\"> only";
                    denied["output"] = denied["error"];
                    return denied;
                }

                protocol::ParsedAction expandedAction = action;
                expandedAction.params =
                    expandValueRefs(action.params, actionResults_);
                if (!action.content.empty()) {
                    Json::Value contentVal(action.content);
                    Json::Value expandedContent =
                        expandValueRefs(contentVal, actionResults_);
                    if (expandedContent.isString())
                        expandedAction.content = expandedContent.asString();
                }

                // Dedup by (name + resolved params + resolved content).
                // Mutating actions must not use or preserve cache: a successful
                // write invalidates prior reads/tests.
                bool mutatesState =
                    (expandedAction.type == protocol::ActionType::TOOL &&
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
                            actionResults_[expandedAction.id] =
                                expansionResultView(cached);
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
                    std::chrono::duration_cast<std::chrono::microseconds>(t1 -
                                                                          t0)
                        .count() /
                    1000.0;
                result["_elapsed_ms"] = elapsedMs; // metadata for renderer
                actionResults_[expandedAction.id] = expansionResultView(result);
                if (mutatesState && result.get("success", false).asBool()) {
                    executedActions_.clear();
                } else if (!mutatesState) {
                    executedActions_[key] =
                        Json::writeString(Json::StreamWriterBuilder(), result);
                }

                // Inject runtime result into cumulative trace and this
                // iteration's trace.
                {
                    std::string resultTag = buildResultTag(action.id, result);
                    rawLlOutput_ += "\n" + resultTag + "\n";
                    iterationRuntimeOutput += resultTag + "\n";
                }

                // Store protocol result for TUI timeline. Debug mode must still
                // show action/result cards; only raw mode suppresses structured
                // UI.
                if (!ctx.raw) {
                    bool ok = result.get("success", false).asBool();
                    std::string summary;
                    if (ok) {
                        std::string out = result.get("stdout", "").asString();
                        if (!out.empty())
                            summary = out;
                        // Check multiple common result field names
                        else if (result.isMember("result") &&
                                 result["result"].isString())
                            summary = result["result"].asString();
                        else if (result.isMember("results") &&
                                 result["results"].isString())
                            summary = result["results"].asString();
                        else if (result.isMember("output") &&
                                 result["output"].isString())
                            summary = result["output"].asString();
                        else if (result.isMember("data") &&
                                 result["data"].isString())
                            summary = result["data"].asString();
                        // context_peek / pin / unpin return structured JSON
                        // without an "output" string — synthesize a scannable
                        // summary so RESULT cards are not empty tool-name
                        // stubs.
                        if (summary.empty() &&
                            (expandedAction.name == "context_peek" ||
                             expandedAction.name == "context_pin" ||
                             expandedAction.name == "context_unpin" ||
                             expandedAction.name == "context_manage")) {
                            std::ostringstream ss;
                            if (result.isMember("path"))
                                ss << result["path"].asString();
                            if (result.isMember("mode"))
                                ss << (ss.str().empty() ? "" : " · ")
                                   << result["mode"].asString();
                            if (result.isMember("bytes"))
                                ss << (ss.str().empty() ? "" : " · ")
                                   << result["bytes"].asUInt64() << "B";
                            if (result.isMember("cycles_remaining"))
                                ss << (ss.str().empty() ? "" : " · ")
                                   << "cycles="
                                   << result["cycles_remaining"].asInt();
                            if (result.isMember("note") &&
                                result["note"].isString())
                                ss << (ss.str().empty() ? "" : "\n")
                                   << result["note"].asString();
                            if (result.isMember("error") &&
                                result["error"].isString())
                                ss << (ss.str().empty() ? "" : "\n")
                                   << "error: " << result["error"].asString();
                            summary = ss.str();
                        }
                        if (summary.empty())
                            summary = action.name;
                    } else {
                        summary = action.name + " — " +
                                  result.get("error", "?").asString();
                    }
                    ProtocolResult protocolResult{
                        action.id,
                        ok,
                        summary,
                        action.name,
                        result.get("exit_code", 0).asInt(),
                        result.get("_elapsed_ms", 0.0).asDouble(),
                        (size_t)summary.size()};
                    protocolResults_.push_back(protocolResult);
                    protocolEvents_.push_back(
                        {ProtocolEventKind::RESULT, "", {}, protocolResult});
                    // Notify callback so TUI can stream tool results
                    // immediately
                    if (ctx.onToken && ctx.streaming)
                        ctx.onToken("", false);
                }

                return result;
            });

        // Tracking state
        std::string llmOutput;
        std::string actionTranscriptOutput; // model actions only, no premature
                                            // responses
        bool taskComplete = false;
        bool nonFinalProtocolRetry = false;

        parser.onEvent([&](const protocol::TokenEvent &ev) {
            switch (ev.type) {
            case protocol::TokenEvent::TEXT:
                // Bare text outside XML tags → ordered thought/protocol stream.
                thoughtOutput_ += ev.content;
                if (!ev.content.empty()) {
                    if (!protocolEvents_.empty() &&
                        protocolEvents_.back().kind ==
                            ProtocolEventKind::THOUGHT) {
                        protocolEvents_.back().text += ev.content;
                    } else {
                        protocolEvents_.push_back(
                            {ProtocolEventKind::THOUGHT, ev.content, {}, {}});
                    }
                }
                break;

            case protocol::TokenEvent::RESPONSE:
                llmOutput += ev.content;
                responseOutput_ += ev.content;
                if (!ev.content.empty()) {
                    if (!protocolEvents_.empty() &&
                        protocolEvents_.back().kind ==
                            ProtocolEventKind::RESPONSE) {
                        protocolEvents_.back().text += ev.content;
                    } else {
                        protocolEvents_.push_back(
                            {ProtocolEventKind::RESPONSE, ev.content, {}, {}});
                    }
                }
                if (ctx.onToken)
                    ctx.onToken(ev.content, false);
                if (ev.metadata.count("is_final") &&
                    ev.metadata.at("is_final") == "true") {
                    taskComplete = true;
                }
                break;

            case protocol::TokenEvent::THOUGHT:
                thoughtOutput_ += ev.content;
                if (!ev.content.empty()) {
                    if (!protocolEvents_.empty() &&
                        protocolEvents_.back().kind ==
                            ProtocolEventKind::THOUGHT) {
                        protocolEvents_.back().text += ev.content;
                    } else {
                        protocolEvents_.push_back(
                            {ProtocolEventKind::THOUGHT, ev.content, {}, {}});
                    }
                }
                break;

            case protocol::TokenEvent::ACTION_START:
                if (ev.action) {
                    // Store protocol action for TUI/timeline regardless of
                    // raw/debug; debug mode must not hide the action/result UI.
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
                    std::string modeStr;
                    switch (ev.action->mode) {
                    case protocol::ExecutionMode::ASYNC:
                        modeStr = "async";
                        break;
                    case protocol::ExecutionMode::FIRE_AND_FORGET:
                        modeStr = "fire_and_forget";
                        break;
                    default:
                        modeStr = "sync";
                        break;
                    }
                    std::map<std::string, std::string> modifiers;
                    if (ev.action->params.isObject()) {
                        static const std::unordered_set<std::string> reserved =
                            {"type", "name",       "id",
                             "mode", "depends_on", "timeout"};
                        for (const auto &key :
                             ev.action->params.getMemberNames()) {
                            if (reserved.count(key))
                                continue;
                            Json::StreamWriterBuilder aw;
                            aw["indentation"] = "";
                            modifiers[key] =
                                Json::writeString(aw, ev.action->params[key]);
                        }
                    }
                    ProtocolAction protocolAction{
                        typeStr,           ev.action->name, ev.action->id, body,
                        modeStr == "sync", modeStr,         modifiers};
                    // Provisional open-tag then full close share one id —
                    // update the existing ACTION event/card in place
                    // (stream-as-fast-as-parse).
                    bool merged = false;
                    for (auto it = protocolEvents_.rbegin();
                         it != protocolEvents_.rend(); ++it) {
                        if (it->kind == ProtocolEventKind::ACTION &&
                            it->action.id == protocolAction.id) {
                            it->action = protocolAction;
                            merged = true;
                            break;
                        }
                    }
                    if (!merged) {
                        protocolActions_.push_back(protocolAction);
                        protocolEvents_.push_back({ProtocolEventKind::ACTION,
                                                   "",
                                                   protocolAction,
                                                   {}});
                    } else {
                        // Keep protocolActions_ tail in sync when present.
                        for (auto it = protocolActions_.rbegin();
                             it != protocolActions_.rend(); ++it) {
                            if (it->id == protocolAction.id) {
                                *it = protocolAction;
                                break;
                            }
                        }
                    }
                    // Notify the TUI immediately on ACTION_START, before
                    // sync dispatch blocks on tools/sub-agents. The action
                    // card must render first; results arrive later.
                    if (ctx.onToken)
                        ctx.onToken("", false);
                    std::ostringstream ax;
                    ax << "<action type=\""
                       << (ev.action->type == protocol::ActionType::TOOL
                               ? "tool"
                           : ev.action->type == protocol::ActionType::AGENT
                               ? "agent"
                           : ev.action->type == protocol::ActionType::RELIC
                               ? "relic"
                           : ev.action->type == protocol::ActionType::WORKFLOW
                               ? "workflow"
                               : "feed")
                       << "\" name=\"" << ev.action->name << "\" id=\""
                       << ev.action->id << "\" mode=\""
                       << (ev.action->mode == protocol::ExecutionMode::SYNC
                               ? "sync"
                               : "async")
                       << "\"";
                    if (!ev.action->content.empty() &&
                        ev.action->params.isObject()) {
                        for (const auto &key :
                             ev.action->params.getMemberNames()) {
                            const auto &v = ev.action->params[key];
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

        // Call LLM with exponential-backoff retry on transient empty/filtered
        // responses. Network exceptions are surfaced immediately (existing
        // behavior); only successful-but-empty streams are retried.
        //
        // RETRY ISOLATION (vet-fix): every retry rebuilds the visible stream
        // from scratch. Two independent observers must agree the iteration is
        // fresh: the protocol/parser state on this side AND the UI's previous
        // event baseline on the other. Without an out-of-band signal, the UI
        // would replay the failed attempt's partial protocol rows on top of
        // the retry attempt's real ones, causing visual flicker and stale
        // "running" badges. We emit a ProtocolEventKind::RETRY marker so the
        // bridge can reset its previousEvent baseline coherently.
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

                // Out-of-band retry marker — bridge consumes this and
                // resets its known-protocol baseline so the retry stream
                // starts without double-rendering attempt-N streams.
                {
                    ProtocolEvent retryMarker;
                    retryMarker.kind = ProtocolEventKind::RETRY;
                    retryMarker.text = std::string("retry ") +
                                       std::to_string(attempt) + " / " +
                                       std::to_string(maxAttempts - 1);
                    protocolEvents_.push_back(std::move(retryMarker));
                }

                int delay =
                    std::min(backoffMs, config_.emptyResponseMaxBackoffMs);
                // Vet-fix: also fire the structured retry signal so a TUI hook
                // can publish a Notification card. The bridge collapses by
                // source+id; the same callback path makes network/HTTP and
                // empty-response retries visually equivalent.
                if (retryHandler_) {
                    RetrySignal rs;
                    rs.kind = RetrySignal::Kind::Network; // empty-response =
                                                          // upstream silence
                    rs.attempt = attempt;
                    rs.maxAttempts = std::max(0, maxAttempts - 1);
                    rs.curlError = "empty-response";
                    rs.backoffMs = delay;
                    retryHandler_(rs);
                }
                if (ctx.debug) {
                    std::cerr
                        << "[MK3:RETRY] empty-response attempt=" << attempt
                        << " delay_ms=" << delay << " finish_reason=\""
                        << streamStats.finishReason << "\" any_content="
                        << (streamStats.anyContent ? "true" : "false") << "\n";
                }
                auto deadline = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(delay);
                while (g_running &&
                       std::chrono::steady_clock::now() < deadline) {
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
                    static_cast<int>(backoffMs *
                                     config_.emptyResponseBackoffMultiplier),
                    config_.emptyResponseMaxBackoffMs);
            }

            try {
                provider_->generateStream(
                    msgs, [&](const std::string &token, bool isFinal) {
                        if (taskComplete)
                            return;
                        // Route thinking tokens (\x01 prefix) to thought stream
                        // — live dimmed
                        if (!token.empty() && token[0] == '\x01') {
                            std::string thoughtChunk = token.substr(1);
                            thoughtOutput_ += thoughtChunk;
                            if (!thoughtChunk.empty()) {
                                if (!protocolEvents_.empty() &&
                                    protocolEvents_.back().kind ==
                                        ProtocolEventKind::THOUGHT) {
                                    protocolEvents_.back().text += thoughtChunk;
                                } else {
                                    protocolEvents_.push_back(
                                        {ProtocolEventKind::THOUGHT,
                                         thoughtChunk,
                                         {},
                                         {}});
                                }
                            }
                            if (ctx.onToken)
                                ctx.onToken("", false); // trigger render
                            return;
                        }
                        rawLlOutput_ += token; // cumulative model/runtime trace
                        iterationRawOutput +=
                            token; // exact model bytes this iteration
                        if (ctx.raw)
                            rawOutput += token;
                        parser.feed(token, isFinal);
                        if (ctx.onToken)
                            ctx.onToken("", isFinal);
                    });
            } catch (const std::exception &e) {
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
                    for (const auto &r : config_.retryOnFinishReasons) {
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
            // No parsed actions and no final response. This is NOT completion.
            // Either the upstream returned no content, or the model emitted
            // bare/non-protocol text (or a non-final <response>). Recover
            // per runtime.mode / completion_policy — never silently finish.
            if (!streamStats.anyContent) {
                std::string detail;
                if (!streamStats.finishReason.empty())
                    detail +=
                        " (finish_reason=" + streamStats.finishReason + ")";
                if (!streamStats.lastError.empty())
                    detail += " — " + streamStats.lastError.substr(0, 200);
                if (streamStats.httpStatus > 0)
                    detail += " [http " +
                              std::to_string(streamStats.httpStatus) + "]";
                std::string visibleError =
                    "⚠ Model returned an empty response" + detail +
                    ". The agent loop is aborting this turn rather than "
                    "silently finishing. "
                    "Retry with a different model if this persists.";
                history_.push_back("System: [EMPTY RESPONSE] " + detail);
                history_.push_back("Agent: " + visibleError);
                responseOutput_ = visibleError;
                taskComplete = true; // runtime failure, not model final
            } else {
                // Salvage whatever the model produced so the next turn can
                // re-emit it inside a proper final response (small-model QoL).
                std::string salvage =
                    pickSalvage(iterationRawOutput, responseOutput_);
                const bool hadNonFinalResponse =
                    !trimCopy(responseOutput_).empty();
                if (!salvage.empty())
                    lastSalvage = salvage;

                history_.push_back("Agent: " + iterationRawOutput);
                ++bareRecoveryCount;
                bareTextReminded_ = true;

                // Autonomous / promote policy: after N failed recoveries,
                // accept salvage as the turn result rather than burning the
                // rest of the iteration budget on a stuck small model.
                const bool earlyPromote =
                    compPolicy == CompPolicy::Promote &&
                    bareRecoveryCount >= promoteAfter && !lastSalvage.empty() &&
                    !finalizationTurn && ctx.iteration < workCap;

                if (earlyPromote) {
                    history_.push_back(
                        "System: [AUTO-PROMOTED] runtime.mode=" +
                        config_.runtimeMode +
                        " promoted salvaged non-final output after " +
                        std::to_string(bareRecoveryCount) +
                        " recovery attempt(s). Original lacked "
                        "<response final=\"true\">.");
                    responseOutput_ = lastSalvage;
                    fullResponse = lastSalvage;
                    taskComplete = true;
                    // Surface as a protocol RESPONSE so nested chat / TUI
                    // paint the promoted answer instead of only a stop banner.
                    protocolEvents_.push_back(
                        {ProtocolEventKind::RESPONSE, lastSalvage, {}, {}});
                } else {
                    history_.push_back(
                        "System: " +
                        buildRecoveryCorrection(salvage, hadNonFinalResponse));
                    nonFinalProtocolRetry = true;
                    taskComplete = false;
                    responseOutput_.clear();
                }
            }
        }

        // Capture exact model output plus runtime-injected results for
        // debugging. Do not add nested markdown headings here; iterations.md
        // already marks sections.
        {
            std::ostringstream os;
            os << iterationRawOutput;
            if (!iterationRawOutput.empty() &&
                iterationRawOutput.back() != '\n')
                os << "\n";
            os << iterationRuntimeOutput;
            iterationOutputs_.push_back(os.str());
        }
        // DEV_MODE / verbose: rewrite dumps after every iteration so a crash
        // mid-turn still leaves the last LLM-facing prompt on disk.
        if (devMode_ || verbose_ || raw_ || ctx.debug)
            dumpSessionArtifacts();

        // Never force a follow-up after finalization — that turn is one-shot.
        bool forceResultFollowup =
            !finalizationTurn && taskComplete && !results.empty() &&
            iterationRawOutput.find("<action") != std::string::npos;
        // If the model emits action(s) and a final response in the same
        // generation, it cannot have seen the real runtime results yet. Keep
        // only the action transcript for the follow-up prompt; discard
        // premature response text and any model-owned result/prose.
        std::string historyOutput =
            forceResultFollowup ? actionTranscriptOutput : llmOutput;
        if (forceResultFollowup) {
            // The model cannot consume a sync action result in the same
            // generation that emitted the action. Force one follow-up turn with
            // the real <result> in context instead of accepting a premature
            // final. Also drop the premature response from history so the next
            // turn sees only the action it actually took plus the runtime
            // result.
            taskComplete = false;
            responseOutput_.clear();
        }

        if (taskComplete) {
            Json::Value expandedResponse =
                expandValueRefs(Json::Value(responseOutput_), actionResults_);
            fullResponse = expandedResponse.isString()
                               ? expandedResponse.asString()
                               : responseOutput_;
            break;
        }

        // Prepare next iteration — push agent output, then system results.
        // Bare/non-final protocol retries already pushed the raw model output
        // plus a strict system correction above; don't add an empty duplicate.
        if (!nonFinalProtocolRetry)
            history_.push_back("Agent: " + historyOutput);
        if (!results.empty()) {
            for (auto &[id, result] : results) {
                std::ostringstream sysMsg;
                sysMsg << buildResultTag(id, result, true);
                history_.push_back("System: " + sysMsg.str());
            }
        }
        parser.clearResults(); // prevent result leakage to next iteration
        tickContextCycles();   // decrement peek cycles; auto-evict at 0

        // Finalization is exactly one shot — never loop forever after cap.
        if (finalizationTurn) {
            finalizationDone = true;
            break;
        }
    }

    if (fullResponse.empty()) {
        // Iteration budget exhausted (or cancelled path cleared response).
        // Per policy: promote salvage when allowed so small-model turns still
        // leave a usable answer + an honest recovery note in history.
        if (compPolicy != CompPolicy::Strict && !lastSalvage.empty()) {
            history_.push_back(
                "System: [AUTO-PROMOTED @ CAP] No <response final=\"true\"> "
                "before iteration cap. Promoted salvaged content under "
                "runtime.mode=" +
                config_.runtimeMode + " / policy=" +
                (config_.completionPolicy.empty() ? std::string("(derived)")
                                                  : config_.completionPolicy) +
                ".");
            fullResponse = lastSalvage;
            responseOutput_ = lastSalvage;
            protocolEvents_.push_back(
                {ProtocolEventKind::RESPONSE, lastSalvage, {}, {}});
        } else {
            fullResponse =
                "⚠ Agent stopped without emitting <response final=\"true\">. "
                "The runtime refused to treat non-final/bare output as "
                "completion" +
                (lastSalvage.empty() ? std::string(".")
                                     : " (salvage was available but "
                                       "completion_policy=strict).");
        }
    }

    // Vet-fix: skip auto-saves when the run produced no captured content
    // AND no live state worth persisting. Empty-then-create + empty-then-
    // checkpoint was creating pairs of zero-row files the Sessions page had
    // to filter out by hand.
    if (!ctx.ephemeral && !ctx.sessionId.empty()) {
        const bool hasContent =
            std::any_of(history_.begin(), history_.end(),
                        [](const std::string &h) { return !h.empty(); });
        if (hasContent || !contextFeeds_.empty()) {
            saveSession(ctx.sessionId);
            saveStateCheckpoint(ctx.sessionId);
        } else if (sessionMgr_.exists(ctx.sessionId)) {
            // Session previously persisted — refresh its checkpoint so
            // operator-controlled resume still works even after our newer
            // content-gating engagement above.
            saveStateCheckpoint(ctx.sessionId);
        }
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
    // First iteration: current user request is a real user message, not
    // synthetic history inside the system prompt. Later iterations replay the
    // inline action/result transcript from history, but still need a minimal
    // provider-level user message; some OpenRouter routes reject system-only
    // continuation calls as "No user query found".
    if (ctx.iteration <= 1) {
        msgs.push_back(ChatMessage::user(buildUserPrompt(ctx)));
    } else {
        msgs.push_back(ChatMessage::user("Continue from the inline transcript "
                                         "above. Use runtime results only; "
                                         "if enough information is available, "
                                         "emit <response final=\"true\">."));
    }
    std::string dynamicTail = buildDynamicContextPrompt();
    if (!dynamicTail.empty()) {
        // Dynamic context is intentionally last for prompt-cache friendliness,
        // but it is runtime/system context, not another user request.
        msgs.push_back(ChatMessage::system(dynamicTail));
    }
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

    // ═══ <system> — persona, system, tools, relics, context ═══
    ss << "<system>\n";

    // Persona block — identity/values (loaded from personaPath)
    if (!personaText_.empty()) {
        ss << "  <persona>\n";
        ss << indentText(personaText_, 4) << "\n";
        ss << "  </persona>\n";
    }

    // System prompt block — capabilities/tools/behavior (loaded from
    // systemPromptPath)
    if (!systemPrompt_.empty()) {
        ss << "  <system_prompt>\n";
        ss << indentText(systemPrompt_, 4) << "\n";
        ss << "  </system_prompt>\n";
    }

    ss << "  <action_available>\n";
    ss << "    <description>Callable runtime surfaces. Use these with <action "
          "type=\"...\"> only "
          "when needed.</description>\n";

    ss << "    <tools>\n        <description>Functions callable with <action "
          "type=\"tool\">. "
          "Prefer declared JSON params; if a tool declares text input, small "
          "scalar attrs plus a "
          "text body are allowed.</description>\n";
    auto schemaIt = env_.find("__TOOL_SCHEMAS__");
    bool hasSchemas = (schemaIt != env_.end() && !schemaIt->second.empty());
    if (hasSchemas) {
        ss << schemaIt->second << "\n";
    }
    for (const auto &[name, tool] : tools_) {
        // Only emit tools NOT already covered by manifest-loaded schemas.
        if (hasSchemas &&
            schemaIt->second.find("name=\"" + name + "\"") != std::string::npos)
            continue;

        // Session-restored script tools keep scriptPath but historically lost
        // schema context. Recover nearest tool.yml so the model sees params.
        bool emittedRecoveredSchema = false;
        if (!tool.scriptPath().empty()) {
            std::filesystem::path scriptPath(tool.scriptPath());
            std::vector<std::filesystem::path> candidates = {
                scriptPath.parent_path() / "tool.yml",
                scriptPath.parent_path().parent_path() / "tool.yml"};
            for (const auto &candidate : candidates) {
                if (!std::filesystem::exists(candidate))
                    continue;
                auto recovered =
                    ManifestLoader::loadToolManifest(candidate.string());
                if (recovered.name.empty() || recovered.name != name)
                    continue;
                const auto &rc = config_.promptBuilding.runtimeCapabilities;
                ss << ManifestLoader::toolSchemasToXml(
                    {recovered}, 8, rc.inputSchemas, rc.returnSchemas,
                    rc.usageExamples);
                emittedRecoveredSchema = true;
                break;
            }
        }
        if (emittedRecoveredSchema)
            continue;

        ss << "        <tool name=\"" << xmlAttr(name) << "\"";
        if (!tool.description().empty() &&
            tool.description() != "See input_schema for parameters")
            ss << " desc=\"" << xmlAttr(tool.description()) << "\"";
        ss << ">\n";
        ss << "\n            <params unavailable=\"true\">schema not loaded; "
              "do not guess required "
              "JSON keys</params>\n";
        ss << "        </tool>\n";
    }
    ss << "    </tools>\n";

    if (!relics_.empty()) {
        ss << "    <relics>\n        <description>Persistent stores callable "
              "with <action "
              "type=\"relic\">.</description>\n";
        for (auto &name : relics_) {
            ss << "        <relic name=\"" << xmlAttr(name) << "\"/>\n";
        }
        ss << "    </relics>\n";
    }

    if (!feedNames().empty()) {
        ss << "    <feeds>\n        <description>Ambient context feeds. "
              "Callable with <action "
              "type=\"feed\"> when fresh params are needed.</description>\n";
        for (const auto &name : feedNames()) {
            ss << "        <feed name=\"" << xmlAttr(name)
               << "\" action=\"feed\"/>\n";
        }
        ss << "    </feeds>\n";
    }

    if (!subAgents_.empty()) {
        ss << "    <sub_agents>\n"
              "        <description>Delegatable agents callable with <action "
              "type=\"agent\" "
              "name=\"AGENT_NAME\" id=\"a1\" mode=\"sync\" "
              "ephemeral=\"true|false\" "
              "dump_context=\"true|false\">plain text instruction</action>. "
              "Inputs and outputs are plain text unless the sub-agent says "
              "otherwise. "
              "Default result contains only the sub-agent final response. Set "
              "dump_context=\"true\" "
              "only when you explicitly need its prompts/runtime "
              "trace.</description>\n";
        for (const auto &[name, agent] : subAgents_) {
            const auto &cfg = agent->config();
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
                for (const auto &toolName : names) {
                    const auto *tool = agent->findTool(toolName);
                    ss << "                <tool name=\"" << xmlAttr(toolName)
                       << "\"";
                    if (tool && !tool->description().empty() &&
                        tool->description() !=
                            "See input_schema for parameters")
                        ss << " desc=\"" << xmlAttr(tool->description())
                           << "\"";
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
        if (config_.historyCap > 0 &&
            history_.size() > (size_t)config_.historyCap) {
            histStart = history_.size() - config_.historyCap;
        }

        size_t userTurn = 0;
        for (size_t hi = histStart; hi < history_.size(); hi++) {
            const auto &h = history_[hi];
            std::string emitted;
            if (h.rfind("Agent: ", 0) == 0) {
                emitted = h.substr(7);
            } else if (h.rfind("System: ", 0) == 0) {
                emitted = h.substr(8);
            } else if (h.rfind("User: ", 0) == 0) {
                userTurn++;
                // On iteration 1 the current request is sent as a real user
                // message, not replayed inside the system prompt transcript.
                if (ctx.iteration <= 1 && hi + 1 == history_.size() &&
                    h.substr(6) == ctx.userInput)
                    continue;
                emitted = "<user turn=\"" + std::to_string(userTurn) +
                          "\" source=\"human\"";
                if (!ctx.sessionId.empty())
                    emitted += " session=\"" + xmlAttr(ctx.sessionId) + "\"";
                emitted += ">" + h.substr(6) + "</user>";
            } else if (h.rfind("Parent(", 0) == 0) {
                // Parent-agent delegate — distinct from human operator turns.
                auto close = h.find(')');
                std::string from = "parent";
                std::string body = h;
                if (close != std::string::npos) {
                    from = h.substr(7, close - 7);
                    body = (close + 1 < h.size() && h[close + 1] == ':')
                               ? h.substr(close + 2)
                               : h.substr(close + 1);
                    if (!body.empty() && body[0] == ' ')
                        body = body.substr(1);
                }
                if (ctx.iteration <= 1 && hi + 1 == history_.size() &&
                    body == ctx.userInput)
                    continue;
                emitted =
                    "<user turn=\"parent\" source=\"parent_agent\" from=\"" +
                    xmlAttr(from) + "\">" + body + "</user>";
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

std::string Agent::buildUserPrompt(const AgentContext &ctx) const {
    // Surface initiator identity in the live user message so the model can
    // tell a human operator apart from a parent-agent delegate without
    // relying solely on history replay.
    if (ctx.source == PromptSource::ParentAgent) {
        std::string from = ctx.sourceName.empty() ? "parent" : ctx.sourceName;
        return std::string("[FROM parent agent \"") + from + "\"]\n" +
               ctx.userInput;
    }
    return ctx.userInput;
}

std::string Agent::buildDynamicContextPrompt() const {
    std::ostringstream ss;

    if (!pinned_.empty()) {
        ss << "<pinned_context>\n"
              "  <description>Files the agent pinned via context_pin. "
              "Persist until context_unpin.</description>\n";
        for (auto &[key, e] : pinned_) {
            ss << "  <file path=\"" << xmlAttr(e.displayPath) << "\" bytes=\""
               << e.bytes << "\">\n";
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
        for (auto &[key, e] : peeking_) {
            ss << "  <file path=\"" << xmlAttr(e.displayPath) << "\" bytes=\""
               << e.bytes << "\" cycles_remaining=\"" << e.cyclesRemaining
               << "\">\n";
            ss << e.content;
            if (!e.content.empty() && e.content.back() != '\n')
                ss << '\n';
            ss << "  </file>\n";
        }
        ss << "</ephemeral_context>\n";
    }

    if (!feeds_.empty()) {
        auto feedResults = feeds::FeedEngine::instance().pollAll();
        auto toolSpecs = feeds::FeedEngine::instance().feedToolSpecs();
        bool any = false;
        for (auto &fr : feedResults) {
            if (!feeds_.count(fr.name))
                continue;
            if (!any) {
                if (ss.tellp() > 0)
                    ss << "\n";
                ss << "<feeds>\n  <description>Dynamic system context "
                      "refreshed each turn. "
                      "Bottom-loaded for prompt-cache stability. "
                      "Each feed may also expose tools — invoke via "
                      "<action type=\"feed\" name=\"<feed>.<tool>\" "
                      ".../>.</description>\n";
                any = true;
            }
            ss << "  <" << fr.name << ">\n";
            ss << "  " << fr.summary << "\n";
            auto specIt = toolSpecs.find(fr.name);
            if (specIt != toolSpecs.end() && !specIt->second.empty()) {
                ss << "    <tools>\n";
                for (const auto &spec : specIt->second) {
                    ss << "      <tool name=\"" << spec.name
                       << "\" description=\"" << spec.description << "\"/>\n";
                }
                ss << "    </tools>\n";
            }
            ss << "  </" << fr.name << ">\n";
        }
        if (any)
            ss << "</feeds>\n";
    }

    if (!contextFeeds_.empty()) {
        if (ss.tellp() > 0)
            ss << "\n";
        ss << "<context_feeds>\n  <description>LLM-requested dynamic context "
              "from prior "
              "turns.</description>\n";
        for (auto &feed : contextFeeds_) {
            ss << "  " << feed << "\n";
        }
        ss << "</context_feeds>\n";
    }

    return ss.str();
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

// ── Tool dispatch — see agent_tool_dispatch.cpp
// ── Session lifecycle — see agent_session.cpp

} // namespace cortex::mk3
