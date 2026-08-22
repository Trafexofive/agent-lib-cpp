// =============================================================================
// agent-lib-MK3 — Agent Implementation
// Core loop: prompt → build messages → LLM generate → parse actions → dispatch
// → loop
// =============================================================================

#include "agent.hpp"
#include "agent_xml.hpp"
#include "agent_run_helpers.hpp"
#include "agent_harness.hpp"
#include "turn_emitter.hpp"

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
#include "../protocol/noise.hpp"
#include "../tools/dispatch.hpp"
#include "../utils/ansi.hpp"
#include "dispatch.hpp"
#include "manifest_loader.hpp"

namespace cortex::mk3 {

// ── XML attribute escaping ──────────────────────────────────────────────

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
    provider_->setStreamStallTimeoutSec(config_.streamStallTimeoutSec);

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
                if (i)
                    routes += "\n  ";
                routes += looked[i] + (i + 1 < looked.size() ? " (miss)" : "");
            }
            throw std::runtime_error(
                "harness prompt not found — searched:\n  " + routes +
                "\nUse --manifest-dir <path> or set CORTEX_HOME. Default "
                "fallback is manifests/harness/default.md");
        }
        config_.harnessPath = resolved; // remember what we resolved to
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

    // Operator context (context.user → USER.md). Optional; silent if missing.
    if (!config_.userPath.empty()) {
        std::ifstream uf(config_.userPath);
        if (uf) {
            std::ostringstream ss;
            ss << uf.rdbuf();
            userText_ = ss.str();
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
    // Vet-fix: do NOT loadSession when history_ already holds this turn's
    // seed (submitComposer → seedUserPrompt + saveSession). Reloading from
    // disk cleared in-memory history_ back to User-only, then the final
    // save often never rewrote Agent lines (taskComplete broke without
    // pushing Agent:), so resume showed only the prompt.
    //
    // Rules:
    //   - history empty → cold load from disk + checkpoint
    //   - history live + same or first session id → keep memory (seed)
    //   - history live + different lastSessionId_ → switch sessions
    if (!ephemeral && !sessionId.empty()) {
        if (history_.empty()) {
            loadSession(sessionId);
            loadStateCheckpoint(sessionId);
        } else if (!lastSessionId_.empty() && lastSessionId_ != sessionId) {
            history_.clear();
            contextFeeds_.clear();
            loadSession(sessionId);
            loadStateCheckpoint(sessionId);
        }
        // else: keep seeded / continuing history_
    }
    lastSessionId_ = sessionId;
    fallbackTriedThisTurn_ = false;
    fallbackSwappedThisTurn_ = false;

    TlsRunGuard tls(&runControl_);
    std::string result = runLoop(ctx);
    restorePrimaryIfFallback();

    dumpSessionArtifacts();

    return result;
}

void Agent::requestStop(RunStopKind k) {
    runControl_.requestStop(k);
    std::vector<std::shared_ptr<Agent>> kids;
    kids.reserve(subAgents_.size());
    for (const auto& kv : subAgents_)
        kids.push_back(kv.second);
    for (const auto& kptr : kids) {
        if (kptr)
            kptr->requestStop(k);
    }
}

void Agent::restorePrimaryIfFallback() {
    if (!fallbackSwappedThisTurn_ || !fallbackSavedProvider_)
        return;
    setProvider(fallbackSavedProvider_, fallbackSavedProviderName_,
                fallbackSavedModel_);
    fallbackSavedProvider_.reset();
    fallbackSwappedThisTurn_ = false;
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
    r["protocol_events"] = static_cast<int>(protocol_.size());
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

}  // namespace cortex::mk3
