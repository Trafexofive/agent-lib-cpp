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
struct PendingTool {
    std::string id;
    std::string name;
    std::atomic<bool> done{false};
    std::atomic<bool> ok{true};
    std::mutex outMtx;
    std::string output;  // mutex-protected, grows as tool produces output
    int exitCode = 0;
    bool harvested = false;  // set after final result pushed
    size_t bytesRendered = 0;  // how many bytes have been streamed to renderer
    std::chrono::steady_clock::time_point startTime;  // for elapsedMs
};

class Agent {
public:
    Agent(AgentConfig cfg, LlmProviderPtr provider);
    ~Agent();  // joins tool threads to prevent exit crash

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
    void harvestPendingTools();

    // ---- Session ----
    void loadSession(const std::string& id);
    void saveSession(const std::string& id);
    void clearHistory();
    void undoLastInteraction();

    // ---- Tool management ----
    void addTool(ToolDef tool);
    void addFeed(const std::string& name) { feeds_.insert(name); }
    void addRelic(const std::string& name) { relics_.insert(name); }
    void removeTool(const std::string& name);
    Json::Value toggleBuiltin(const Json::Value& params, bool enable);
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
#ifdef MK3_DUMP_SESSION
    void dumpSessionArtifacts() const;
#endif

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
    std::vector<std::shared_ptr<PendingTool>> pendingTools_;  // threaded tool execution
    std::set<std::string> disabledBuiltins_;
    std::vector<std::thread> toolThreads_;  // join on destruction, prevent exit crash
    std::set<std::string> relics_;  // enabled relic names (from manifest import)
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
