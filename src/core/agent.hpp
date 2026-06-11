#pragma once
// =============================================================================
// agent-lib-MK3 — Agent Runtime
// The core agent loop: prompt → LLM → parse → dispatch → loop
// =============================================================================

#include "../core/types.hpp"
#include "../core/provider.hpp"
#include "../protocol/parser.hpp"
#include "../session/manager.hpp"
#include <set>
#include "../tools/registry.hpp"
#include "../sandbox/policy.hpp"
#include <json/json.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>

namespace cortex::mk3 {

extern std::atomic<bool> g_running;

struct ProtocolAction {
    std::string type, name, id, body;
    bool sync = true;
};

struct ProtocolResult {
    std::string id;
    bool ok = true;
    std::string summary;
    std::string toolName;
    int exitCode = 0;
    double elapsedMs = 0;
    size_t outputBytes = 0;
};

// ── Pending tool execution (threaded popen, streams output live) ──
class Agent {
public:
    Agent(AgentConfig cfg, LlmProviderPtr provider);
    ~Agent() = default;

    // ---- Execution ----
    std::string prompt(const std::string& input, const std::string& sessionId = "",
                       bool ephemeral = false);
    std::string prompt(const std::string& input, StreamCallback onToken,
                       const std::string& sessionId = "", bool ephemeral = false);

    // ---- Modes ----
    void setRaw(bool v) { raw_ = v; }
    void setVerbose(bool v) { verbose_ = v; }

    // ---- Output ────
    const std::string& rawLlOutput() const { return rawLlOutput_; }
    const std::string& responseOutput() const { return responseOutput_; }
    const std::string& thoughtOutput() const { return thoughtOutput_; }
    const std::string& lastPrompt() const { return lastPrompt_; }
    const std::vector<std::string>& iterationPrompts() const { return iterationPrompts_; }
    const std::vector<ProtocolAction>& protocolActions() const { return protocolActions_; }
    const std::vector<ProtocolResult>& protocolResults() const { return protocolResults_; }

    // Threaded tool execution: harvest completed tools, push results to protocolResults_

    // ---- Session ----
    void loadSession(const std::string& id);
    void saveSession(const std::string& id);
    void clearHistory();
    void undoLastInteraction();

    // ---- Tool management ----
    void addTool(ToolDef tool);
    void addFeed(const std::string& name) { feeds_.insert(name); }
    void addRelic(const std::string& name) { relics_.insert(name); }

    // ---- Context management (pin / peek / unpin) ----
    // A pinned file lives in the system prompt indefinitely until unpin.
    // A peek file lives in the system prompt for `cycles` iterations, then evicts.
    // Path keys are canonicalised so `./x.cpp`, `x.cpp`, `src/../x.cpp` are equivalent.
    Json::Value contextPin(const std::string& path, bool force = false);
    Json::Value contextPeek(const std::string& path, int cycles = 1, bool force = false);
    Json::Value contextUnpin(const std::string& path);
    void tickContextCycles();                       // called at end of each iteration
    Json::Value contextSnapshot() const;            // for debugging / introspection
    std::string renderSystemPrompt() const;         // testing hook — no LLM call
    static constexpr size_t kContextSizeLimit = 65536; // 64 KB per entry; override via force=true

    void removeTool(const std::string& name);
    Json::Value toggleBuiltin(const Json::Value& params, bool enable);
    int reloadManifests(bool backup);
    void saveSessionTools();
    void loadSessionTools();
    bool hasTool(const std::string& name) const;
    std::vector<std::string> toolNames() const;

    // ---- Sub-agents ----
    void addSubAgent(std::shared_ptr<Agent> agent);
    void removeSubAgent(const std::string& name);
    bool hasSubAgent(const std::string& name) const;
    Agent* getSubAgent(const std::string& name) const;

    // ---- Sandbox ----
    void setSandboxPolicy(const sandbox::SandboxPolicy& policy) { sandboxPolicy_ = policy; }
    const sandbox::SandboxPolicy& sandboxPolicy() const { return sandboxPolicy_; }

    // ---- Environment ----
    void setEnv(const std::string& key, const std::string& val);
    std::string getEnv(const std::string& key, const std::string& def = "") const;

    // ---- Accessors ----
    const AgentConfig& config() const { return config_; }
    void setIterationCap(int cap) { config_.iterationCap = cap; }
    const std::string& name() const { return config_.name; }
    session::SessionManager& sessionMgr() { return sessionMgr_; }

private:
    // Core loop
    std::string runLoop(AgentContext& ctx);

    // Prompt building
    ChatMessages buildChatPrompt(const AgentContext& ctx) const;
    std::string buildSystemPrompt(const AgentContext& ctx) const;
    std::string buildUserPrompt(const AgentContext& ctx) const;
    // Tool dispatch
    Json::Value dispatchTool(const protocol::ParsedAction& action);
    Json::Value executeScriptTool(const ToolDef& tool, const Json::Value& params);
    void dumpSessionArtifacts() const;

    // Output sanitization
    static std::string sanitize(const std::string& output);

    // Members
    AgentConfig config_;
    LlmProviderPtr provider_;
    session::SessionManager sessionMgr_;
    std::vector<std::string> history_;
    std::string systemPrompt_;
    std::vector<std::string> contextFeeds_;  // accumulated from <context_feed> tags
    std::map<std::string, std::string> executedActions_;  // dedup: key → cached result JSON string
    std::map<std::string, ToolDef> tools_;
    std::set<std::string> feeds_;   // enabled feed names (from manifest import)
    std::set<std::string> disabledBuiltins_;
    std::set<std::string> relics_;  // enabled relic names (from manifest import)

    // ── Context entries (live in <pinned_context>/<ephemeral_context>) ──
    struct PinnedEntry {
        std::string displayPath;   // original path as requested by the LLM
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
    std::map<std::string, PeekEntry>   peeking_;
    bool raw_ = false;
    bool verbose_ = false;
    bool bareTextReminded_ = false;  // one-time bare-text warning, persists across turns
    std::string rawLlOutput_;      // raw LLM stream (all tokens)
    std::string responseOutput_;   // sanitized response text
    std::string thoughtOutput_;    // thought content (hidden in FULL)
    std::string lastPrompt_;       // last built prompt for /prompts
    std::vector<std::string> iterationPrompts_;     // full system prompt per iteration (for /prompts toggle)
    std::vector<std::string> iterationOutputs_;     // LLM response + results per iteration
    std::vector<ProtocolAction> protocolActions_;
    std::vector<ProtocolResult> protocolResults_;
    std::map<std::string, std::shared_ptr<Agent>> subAgents_;
    std::map<std::string, std::string> env_;
    sandbox::SandboxPolicy sandboxPolicy_;

    // ── Cached harness text (loaded once in constructor) ──
    mutable std::string harnessText_;
};

} // namespace cortex::mk3
