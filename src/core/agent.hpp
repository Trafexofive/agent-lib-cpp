#pragma once
// =============================================================================
// agent-lib-MK3 — Agent Runtime
// The core agent loop: prompt → LLM → parse → dispatch → loop
// =============================================================================

#include <json/json.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../core/provider.hpp"
#include "../core/types.hpp"
#include "../protocol/events.hpp"  // ProtocolEvent* PODs (foundation F1)
#include "../protocol/parser.hpp"
#include "../sandbox/policy.hpp"
#include "../session/manager.hpp"
#include "../tools/registry.hpp"
#include "../workflows/workflow.hpp"  // WorkflowResult (handleWorkflowDelegate)

namespace cortex::mk3 {

namespace dispatch { class ActionDispatcher; }

extern std::atomic<bool> g_running;

// Why g_running flipped false — distinguishes operator cancel from wall kills.
enum class RunStopKind : uint8_t {
    None = 0,
    Operator = 1,   // Ctrl-C/X, slash stop, TUI stopAgentLoop
    ExternalSignal = 2,  // SIGTERM / external `timeout` / kill
    StreamAbort = 3,     // provider callback abort without operator stop
};
extern std::atomic<uint8_t> g_stop_kind;  // RunStopKind

inline void requestRunStop(RunStopKind kind) {
    g_stop_kind.store(static_cast<uint8_t>(kind), std::memory_order_release);
    g_running.store(false, std::memory_order_release);
}
inline RunStopKind currentRunStopKind() {
    return static_cast<RunStopKind>(
        g_stop_kind.load(std::memory_order_acquire));
}
inline void clearRunStop() {
    g_stop_kind.store(static_cast<uint8_t>(RunStopKind::None),
                      std::memory_order_release);
    g_running.store(true, std::memory_order_release);
}

// ProtocolAction / ProtocolResult / ProtocolEventKind / ProtocolEvent:
// defined in protocol/events.hpp (included above). Agent remains the runtime.

// Mutable stream state for one runLoop iteration (onEvent + post-stream).
struct ProtocolStreamState {
    std::string llmOutput;
    std::string actionTranscriptOutput;
    bool taskComplete = false;
    bool nonFinalProtocolRetry = false;
    std::string thoughtRawBuf;
    size_t thoughtEventIdx = static_cast<size_t>(-1);
    size_t runEpochStart = 0;
    // Once a thought segment is classified as symbol-dump / pure noise, stop
    // re-scanning and re-publishing on every token (O(n²) stall under nm floods).
    bool thoughtDroppedAsNoise = false;
};

// ── Pending tool execution (threaded popen, streams output live) ──
class Agent {
   public:
    Agent(AgentConfig cfg, LlmProviderPtr provider);
    ~Agent() = default;

    // ---- Execution ----
    // source/sourceName let sub-agents label history as User vs Parent(agent).
    std::string prompt(const std::string& input, const std::string& sessionId = "",
                       bool ephemeral = false,
                       PromptSource source = PromptSource::Human,
                       const std::string& sourceName = "");
    std::string prompt(const std::string& input, StreamCallback onToken,
                       const std::string& sessionId = "", bool ephemeral = false,
                       PromptSource source = PromptSource::Human,
                       const std::string& sourceName = "");

    // Read-only snapshot of an agent (self or via parent inspect action).
    Json::Value inspectContext(int lastN = 20) const;

    // ---- Modes ----
    void setRaw(bool v) {
        raw_ = v;
    }
    void setVerbose(bool v) {
        verbose_ = v;
    }
    void setDevMode(bool v) {
        devMode_ = v;
        if (v) {
            // File dumps only (iterations.md / raw.md under .cortex/dev).
            // NEVER auto-enable verbose_/raw_ — those print full prompts to
            // stderr and destroy the alt-screen TUI (looks like mashed text,
            // prompt leakage, overlapping blocks). Use -V / --raw explicitly
            // when you want terminal dumps (non-TUI).
            env_["__DEBUG_MODE__"] = "true";
            env_["__DEV_MODE__"] = "true";
        }
    }

    // TUI owns the terminal — suppress all agent std::cerr chatter.
    void setSilenceTerminal(bool v) { silenceTerminal_ = v; }
    bool silenceTerminal() const { return silenceTerminal_; }
    bool devMode() const { return devMode_; }
    // Last directory written by dumpSessionArtifacts (empty if none).
    const std::string& lastDevDumpDir() const { return lastDevDumpDir_; }
    // force=true writes dumps regardless of verbose/raw/dev_mode (slash /export-dump).
    void dumpSessionArtifacts(bool force = false) const;

    // ---- Output ────
    const std::string& rawLlOutput() const {
        return rawLlOutput_;
    }
    const std::string& responseOutput() const {
        return responseOutput_;
    }
    // Current agent-loop generation (1..cap) while prompt() is running; 0 idle.
    int liveIteration() const {
        return liveIteration_.load(std::memory_order_relaxed);
    }
    const std::string& thoughtOutput() const {
        return thoughtOutput_;
    }
    const std::string& lastPrompt() const {
        return lastPrompt_;
    }
    const std::vector<std::string>& iterationPrompts() const {
        return iterationPrompts_;
    }
    const std::vector<std::string>& iterationOutputs() const {
        return iterationOutputs_;
    }
    const std::vector<ProtocolAction>& protocolActions() const {
        return protocolActions_;
    }
    const std::vector<ProtocolResult>& protocolResults() const {
        return protocolResults_;
    }
    // Full multi-prompt transcript for this agent instance (User/Parent/Agent/System).
    // Survives across prompt() calls so sub-agent continuity is inspectable.
    const std::vector<std::string>& history() const { return history_; }

    // Vet-fix: pre-seed history_ with a user prompt so the next saveSession
    // call lands at least one record. submitComposer invokes this so a TUI
    // that aborts between the user's keystroke and runAgentTurn's first
    // iteration still backs the typed text to disk. Idempotent: prompt()
    // detects the trailing-equal User: line and skips its own push.
    void seedUserPrompt(const std::string& text) {
        history_.push_back("User: " + text);
    }

    // Operator steering while a turn is live. Buffered and injected at the
    // soonest safe boundary (between loop iterations). Thread-safe.
    void queueSteer(std::string text) {
        if (text.empty()) return;
        std::lock_guard<std::mutex> lock(steerMu_);
        if (!pendingSteer_.empty()) pendingSteer_ += "\n\n";
        pendingSteer_ += std::move(text);
    }
    bool hasPendingSteer() const {
        std::lock_guard<std::mutex> lock(steerMu_);
        return !pendingSteer_.empty();
    }
    std::string takeSteer() {
        std::lock_guard<std::mutex> lock(steerMu_);
        std::string out;
        out.swap(pendingSteer_);
        return out;
    }

    const std::vector<ProtocolEvent>& protocolEvents() const {
        return protocolEvents_;
    }

    // Threaded tool execution: harvest completed tools, push results to protocolResults_

    // ---- Session ----
    void loadSession(const std::string& id);
    void saveSession(const std::string& id);
    void loadStateCheckpoint(const std::string& sessionId);
    void saveStateCheckpoint(const std::string& sessionId) const;
    void clearHistory();
    void undoLastInteraction();

    // ---- Tool management ----
    void addTool(tools::Tool tool);
    void addFeed(const std::string& name) {
        feeds_.insert(name);
    }
    std::vector<std::string> feedNames() const {
        return std::vector<std::string>(feeds_.begin(), feeds_.end());
    }
    void addRelic(const std::string& name) {
        relics_.insert(name);
    }
    std::vector<std::string> relicNames() const {
        return std::vector<std::string>(relics_.begin(), relics_.end());
    }

    void setAskToolHandler(std::function<Json::Value(const Json::Value& params)> handler) {
        askToolHandler_ = std::move(handler);
    }

    // Vet-fix: forward a retry observer through the provider AND through the
    // empty-response loop in prompt(). Wire one callback at the bridge
    // boundary (runAgentTurn) and both retry flavors get the same hook.
    void setRetryHandler(RetryCallback handler) {
        retryHandler_ = std::move(handler);
        if (provider_) provider_->setRetryCallback(retryHandler_);
    }

    // ---- Context management (pin / peek / unpin) ----
    // A pinned file lives in the system prompt indefinitely until unpin.
    // A peek file lives in the system prompt for `cycles` iterations, then evicts.
    // Path keys are canonicalised so `./x.cpp`, `x.cpp`, `src/../x.cpp` are equivalent.
    Json::Value contextPin(const std::string& path, bool force = false);
    Json::Value contextPeek(const std::string& path, int cycles = 1, bool force = false);
    Json::Value contextUnpin(const std::string& path);
    void tickContextCycles();                // called at end of each iteration
    Json::Value contextSnapshot() const;     // for debugging / introspection
    std::string renderSystemPrompt() const;  // testing hook — no LLM call
    Json::Value stateCheckpointJson() const;
    void loadStateCheckpointJson(const Json::Value& root);
    static constexpr size_t kContextSizeLimit = 65536;  // 64 KB per entry; override via force=true

    void removeTool(const std::string& name);
    Json::Value toggleBuiltin(const Json::Value& params, bool enable);
    int reloadManifests(bool backup);
    void saveSessionTools();
    void loadSessionTools();
    bool hasTool(const std::string& name) const;
    std::vector<std::string> toolNames() const;
    const tools::Tool* findTool(const std::string& name) const;

    // ---- Sub-agents ----
    void addSubAgent(std::shared_ptr<Agent> agent);
    void removeSubAgent(const std::string& name);
    bool hasSubAgent(const std::string& name) const;
    Agent* getSubAgent(const std::string& name) const;
    std::vector<std::string> subAgentNames() const {
        std::vector<std::string> names;
        for (const auto& [name, _] : subAgents_)
            names.push_back(name);
        return names;
    }
    LlmProviderPtr provider() const { return provider_; }

    // Compaction UI / child fold-up (see compaction.hpp).
    const std::string& lastCompactNote() const { return lastCompactNote_; }
    int64_t lastCompactWallMs() const { return lastCompactWallMs_; }
    // Non-destructive: true for ~8s after a compact fires (footer badge).
    bool compactBadgeActive(int64_t nowMs) const {
        if (lastCompactWallMs_ <= 0 || nowMs <= 0) return false;
        return (nowMs - lastCompactWallMs_) < 8000;
    }
    std::string takeCompactUiPending() const {
        std::string s = lastCompactUiPending_;
        lastCompactUiPending_.clear();
        return s;
    }
    // Public so parent can fold child history after delegate return.
    void compactHistoryInPlaceIfConfigured();

    // Hot-swap cognitive engine (slash /model). Keeps history/tools/session.
    void setProvider(LlmProviderPtr p, std::string providerName, std::string modelName) {
        if (!p) return;
        provider_ = std::move(p);
        if (!providerName.empty()) config_.provider = std::move(providerName);
        if (!modelName.empty()) {
            config_.model = modelName;
            provider_->setModel(config_.model);
        } else if (!config_.model.empty()) {
            provider_->setModel(config_.model);
        }
        provider_->setTemperature(config_.temperature);
        provider_->setMaxTokens(config_.maxTokens > 0 ? config_.maxTokens
                                                      : provider_->getMaxTokens());
        if (retryHandler_) provider_->setRetryCallback(retryHandler_);
        if (silenceTerminal_) provider_->setQuietLogs(true);
    }
    void setModelName(const std::string& modelName) {
        if (modelName.empty() || !provider_) return;
        config_.model = modelName;
        provider_->setModel(modelName);
    }

    // ---- Sandbox ----
    void setSandboxPolicy(const sandbox::SandboxPolicy& policy) {
        sandboxPolicy_ = policy;
    }
    const sandbox::SandboxPolicy& sandboxPolicy() const {
        return sandboxPolicy_;
    }

    // ---- Environment ----
    void setEnv(const std::string& key, const std::string& val);
    std::string getEnv(const std::string& key, const std::string& def = "") const;

    // ---- Accessors ----
    const AgentConfig& config() const {
        return config_;
    }
    void setActionTimeoutSec(int sec) {
        if (sec > 0)
            config_.actionTimeoutSec = sec;
    }
    void setIterationCap(int cap) {
        config_.iterationCap = cap;
    }
    const std::string& name() const {
        return config_.name;
    }
    session::SessionManager& sessionMgr() {
        return sessionMgr_;
    }

    // Set the prompt handler for workflow human-in-the-loop steps.
    using HumanPromptHandler = std::function<std::string(const std::string&, const std::string&, int)>;
    void setHumanPromptHandler(HumanPromptHandler h) { humanPromptHandler_ = std::move(h); }
    HumanPromptHandler getHumanPromptHandler() const { return humanPromptHandler_; }

    // Set the checkpoint handler for workflow checkpoint steps.
    using CheckpointHandler = std::function<void(const std::string&, const Json::Value&)>;
    void setCheckpointHandler(CheckpointHandler h) { checkpointHandler_ = std::move(h); }
    CheckpointHandler getCheckpointHandler() const { return checkpointHandler_; }

   private:
    // Core loop
    std::string runLoop(AgentContext& ctx);
    // Sub-agent action path (was inline lambda in runLoop).
    Json::Value handleAgentDelegate(AgentContext& ctx,
                                    const protocol::ParsedAction& action,
                                    const std::string& instruction);
    workflows::WorkflowResult handleWorkflowDelegate(
        AgentContext& ctx, const std::string& workflowName, const Json::Value& params);
    Json::Value handleActionExecute(AgentContext& ctx, dispatch::ActionDispatcher& d,
                                    std::string& iterationRuntimeOutput,
                                    bool finalizationTurn,
                                    const protocol::ParsedAction& action);
    void publishCleanThought(ProtocolStreamState& st, const std::string& rawAppend);
    void handleProtocolEvent(AgentContext& ctx, ProtocolStreamState& st,
                             const protocol::TokenEvent& ev);

    // Prompt building
    ChatMessages buildChatPrompt(const AgentContext& ctx) const;
    std::string buildSystemPrompt(const AgentContext& ctx) const;
    std::string buildUserPrompt(const AgentContext& ctx) const;
    std::string buildDynamicContextPrompt() const;
    // Tool dispatch
    Json::Value dispatchTool(const protocol::ParsedAction& action);
    Json::Value dispatchAskTool(const Json::Value& params);
    Json::Value executeScriptTool(const tools::Tool& tool, const Json::Value& params);
    // Session-scoped dev dump dir (~/.cortex/dev/<id>/). Creates parents.
    std::string devDumpDirectory() const;

    // Output sanitization
    static std::string sanitize(const std::string& output);

    // Members
    AgentConfig config_;
    LlmProviderPtr provider_;
    session::SessionManager sessionMgr_;
    std::vector<std::string> history_;
    mutable std::mutex steerMu_;
    std::string pendingSteer_;
    std::string systemPrompt_;
    std::string personaText_;                             // persona content (identity/values)
    std::string userText_;                                // operator context (USER.md)
    std::vector<std::string> contextFeeds_;               // accumulated from <context_feed> tags
    std::map<std::string, std::string> executedActions_;  // dedup: key → cached result JSON string
    std::map<std::string, tools::Tool> tools_;
    std::set<std::string> feeds_;  // enabled feed names (from manifest import)
    std::set<std::string> disabledBuiltins_;
    std::set<std::string> relics_;  // enabled relic names (from manifest import)

    // ── Context entries (live in <pinned_context>/<ephemeral_context>) ──
    struct PinnedEntry {
        std::string displayPath;  // original path as requested by the LLM
        std::string content;
        size_t bytes = 0;
    };
    struct PeekEntry {
        std::string displayPath;
        std::string content;
        size_t bytes = 0;
        int cyclesRemaining = 0;
    };
    std::map<std::string, PinnedEntry> pinned_;
    std::map<std::string, PeekEntry> peeking_;
    bool raw_ = false;
    bool verbose_ = false;
    bool devMode_ = false;
    bool silenceTerminal_ = false;
    mutable std::string lastSessionId_;
    mutable std::string lastDevDumpDir_;
    bool bareTextReminded_ = false;  // one-time bare-text warning, persists across turns
    std::string rawLlOutput_;        // raw LLM stream (all tokens)
    std::string responseOutput_;     // sanitized response text
    std::atomic<int> liveIteration_{0};  // 1..cap while runLoop; 0 idle
    std::string thoughtOutput_;      // thought content (hidden in FULL)
    std::string lastPrompt_;         // last built prompt for /prompts
    std::vector<std::string>
        iterationPrompts_;  // full system prompt per iteration (for /prompts toggle)
    std::vector<std::string> iterationOutputs_;  // LLM response + results per iteration
    std::vector<std::string> subAgentTraces_;    // delegated agent traces for parent dumps
    std::vector<ProtocolAction> protocolActions_;
    std::vector<ProtocolResult> protocolResults_;
    std::vector<ProtocolEvent> protocolEvents_;
    std::map<std::string, std::shared_ptr<Agent>> subAgents_;
    // Workflow integration: handlers for human-in-loop and checkpoint steps
    HumanPromptHandler humanPromptHandler_;
    CheckpointHandler checkpointHandler_;
    std::map<std::string, Json::Value>
        actionResults_;  // persistent results table for ${id.field} expansion
    std::map<std::string, std::string> env_;
    sandbox::SandboxPolicy sandboxPolicy_;
    std::function<Json::Value(const Json::Value& params)> askToolHandler_;
    RetryCallback retryHandler_;

    // ── Cached harness text (loaded once in constructor) ──
    mutable std::string harnessText_;

    // ── History window + compaction state (prompt-build only; session keeps full history_) ──
    // history_cap reclamps at most every maxTurnsPerCycle user turns (default 15).
    mutable size_t historyWindowStart_ = 0;
    mutable int historyCapAppliedAtUserTurn_ = -1000000;
    // -1 = never compacted (must NOT trip triggerTurns on first prompt).
    mutable int lastCompactAtUserTurn_ = -1;
    mutable int64_t lastCompactWallMs_ = 0;
    mutable std::string lastCompactNote_;
    mutable std::string lastCompactArchive_;  // optional cold body from last compact
    // One-shot UI badge after a compact fires (prompt still keeps lastCompactNote_).
    mutable std::string lastCompactUiPending_;
    // One cognitive_engine.fallback attempt per top-level prompt().
    bool fallbackTriedThisTurn_ = false;
};

}  // namespace cortex::mk3
