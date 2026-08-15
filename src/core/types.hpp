#pragma once
// =============================================================================
// agent-lib-MK3 — Core Types
// Lean, single-source-of-truth types shared across all modules.
// =============================================================================

#include <json/json.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cortex::mk3 {

// ---------------------------------------------------------------------------
// Chat messages
// ---------------------------------------------------------------------------
enum class ChatRole { SYSTEM, USER, ASSISTANT, TOOL };

struct ChatMessage {
    ChatRole role;
    std::string content;
    std::string name;        // tool name (for TOOL role)
    std::string toolCallId;  // for TOOL role

    static ChatMessage system(const std::string& c) {
        return {ChatRole::SYSTEM, c, {}, {}};
    }
    static ChatMessage user(const std::string& c) {
        return {ChatRole::USER, c, {}, {}};
    }
    static ChatMessage assistant(const std::string& c) {
        return {ChatRole::ASSISTANT, c, {}, {}};
    }
    static ChatMessage tool(const std::string& id, const std::string& name, const std::string& c) {
        return {ChatRole::TOOL, c, name, id};
    }

    static const char* roleName(ChatRole r) {
        switch (r) {
            case ChatRole::SYSTEM:
                return "system";
            case ChatRole::USER:
                return "user";
            case ChatRole::ASSISTANT:
                return "assistant";
            case ChatRole::TOOL:
                return "tool";
        }
        return "unknown";
    }
};

using ChatMessages = std::vector<ChatMessage>;

// ---------------------------------------------------------------------------
// Streaming callback
// ---------------------------------------------------------------------------
using StreamCallback = std::function<void(const std::string& token, bool isFinal)>;

// ---------------------------------------------------------------------------
// Tool parameter
// ---------------------------------------------------------------------------
struct ToolParam {
    std::string name;
    std::string type;  // "string", "number", "boolean", "object", "array"
    std::string description;
    bool required = false;
    Json::Value defaultVal;
    std::vector<std::string> enumVals;
};

// ---------------------------------------------------------------------------
// Tool definition
// ---------------------------------------------------------------------------
struct ToolDef {
    std::string name;
    std::string description;
    std::vector<ToolParam> params;
    bool isNative = true;            // true = C++ callback, false = script
    std::string scriptPath;          // for script tools
    std::string scriptRuntime;       // "python3", "bash", "process", etc.
    std::string buildCommand;        // optional build command for process/script tools
    std::string buildCwd;            // directory to run buildCommand in
    std::string buildOutput;         // output artifact used to skip rebuild when present
    std::string inputType = "json";  // action body mode: json | text
    std::string textParam;           // for text body mode: content/input/instruction
    int timeoutSec = 0;              // script tool wall clock; 0 → agent actionTimeoutSec

    // Generate OpenAI function-calling schema
    Json::Value toOpenAISchema() const;

    // Generate XML description for protocol-style prompting
    std::string toXml() const;
};

// ---------------------------------------------------------------------------
// Tool result
// ---------------------------------------------------------------------------
struct ToolResult {
    bool success = false;
    std::string output;
    std::string error;
    Json::Value data;

    Json::Value toJson() const {
        Json::Value r;
        r["success"] = success;
        if (!output.empty())
            r["output"] = output;
        if (!error.empty())
            r["error"] = error;
        if (!data.isNull())
            r["data"] = data;
        return r;
    }

    static ToolResult ok(const std::string& out) {
        return {true, out, "", Json::Value()};
    }
    static ToolResult ok(Json::Value d) {
        return {true, "", "", std::move(d)};
    }
    static ToolResult fail(const std::string& err) {
        return {false, "", err, Json::Value()};
    }
};

// ---------------------------------------------------------------------------
// Agent configuration
// ---------------------------------------------------------------------------
struct PromptBuildingConfig {
    // Defaults = tool CARDS (name + PE description + compact keys).
    // Full JSON schemas are the expensive path — opt in via agent.yml
    // prompt_building.runtime_capabilities.input_schemas: true
    // or env CORTEX_TOOL_FULL=1 at schema build time.
    struct RuntimeCapabilities {
        bool inputSchemas = false;
        bool returnSchemas = false;
        bool usageExamples = false;
    };

    RuntimeCapabilities runtimeCapabilities;
};

// Live host↔sandbox path mapping (bind mount / glorified symlink).
// host  — path on the operator machine (resolved absolute at load time)
// guest — path the agent/tools see inside the sandbox
// readOnly — when true, fs_write under this bind is blocked even if global RO is off
struct SandboxBind {
    std::string host;
    std::string guest;
    bool readOnly = false;
};

// Per-kind retention for compaction (see docs/manifests/compaction.md).
// keep: all | tail | none
struct CompactionTagPolicy {
    std::string keep = "tail";  // all | tail | none
    int keepLast = 8;
    int truncateChars = 0;      // 0 = no body trim
    bool onErrorKeepFull = true;  // results: keep errors full when true
};

// Hybrid compaction config — best of minimal / recommended / profile sugar.
// Absent or enabled=false → off (only history_cap applies).
struct CompactionConfig {
    bool configured = false;  // compaction:/compacting: block present
    bool enabled = false;

    // none | light | balanced | aggressive | archive_first | "" (explicit policy only)
    std::string profile;

    // Triggers (OR). Empty triggers + enabled → never auto (manual later).
    int triggerContextTokens = 0;  // 0 = disabled
    double triggerContextPct = 0;  // 0 = disabled; 0.65 = 65% of model window
    int triggerTurns = 0;          // 0 = disabled; user turns since last compact
    int modelContextTokens = 0;    // 0 = unknown; used with context_pct

    int cooldownMinTurns = 2;
    int cooldownMinSeconds = 0;  // reserved; wall-clock optional later

    CompactionTagPolicy defaultPolicy;
    std::map<std::string, CompactionTagPolicy> tags;  // user,parent,thought,action,result,response,agent,system
    std::vector<std::string> neverDrop;  // pin, open_ask, ...

    // drop | summarize_rules | summarize_llm (llm = phase 2, treated as rules for now)
    std::string outputMode = "summarize_rules";
    bool archiveEnabled = false;
    std::string archiveSink = "artifact";  // artifact | file | none
    std::string archiveFormat = "markdown";

    bool subagentsInherit = true;
    bool childBeforeReturn = true;
};

struct AgentConfig {
    std::string name;
    std::string version = "1.0";
    std::string summary;

    // LLM settings
    std::string provider = "deepseek";
    std::string model = "deepseek-chat";
    std::string fallbackProvider;
    std::string fallbackModel;
    double temperature = 0.7;
    double topP = 0.95;
    int topK = 40;
    double presencePenalty = 0.0;
    double frequencyPenalty = 0.0;

    // When true, the system prompt gets a THINKING MODE rule appended
    // that forces the LLM to emit <thought> before any <action>. Off by
    // default so existing agents are unchanged. Designed for models that
    // don't natively emit thinking tokens (e.g. minimax-m3) and for
    // users who want visible reasoning in the TUI.
    bool requireThought = false;
    // thinking_level: minimal|low|medium|high — reasoning-budget hint injected
    // into the system prompt as a <reasoning_policy> directive. Empty = off
    // (default behavior). Complements requireThought (hard rule).
    std::string thinkingLevel;
    // Manifest runtime defaults (CLI flags override when explicitly set).
    // noSession  → do not load/save session for this agent run
    // ephemeral  → prefer exit-on-done / one-shot lifecycle at the app layer
    bool defaultNoSession = false;
    bool defaultEphemeral = false;
    // dev_mode → auto-dump full LLM-facing iterations + raw stream + history
    // per session under ~/.cortex/dev/<session>/ (and CWD copies for lazy open).
    bool devMode = false;

    // Runtime mode / completion policy — how the harness treats bare or
    // non-final model output (small-model due diligence).
    //
    // mode:
    //   normal      → recover with correction; promote salvage only if the
    //                 iteration budget is exhausted with no final tag
    //   autonomous  → same recovery, but auto-promote salvageable bare/
    //                 non-final content after a few failed recoveries
    //
    // completion_policy (optional override of mode defaults):
    //   recover | promote | strict
    //   strict never auto-promotes; always surfaces the stop warning at cap
    std::string runtimeMode = "normal";
    std::string completionPolicy;  // empty → derive from runtimeMode
    int bareRecoveryPromoteAfter = -1;  // -1 → derive (normal: never early; autonomous: 2)
    int maxTokens = 0;
    int iterationCap =
        50;  // agent turns before forced response (override via manifest max_iterations)
    int actionTimeoutSec = 30;  // max seconds to wait for dispatched actions

    // Stream throttling / stall control (runtime.throttling). 0 = do not cut
    // a stalled stream (let curl LOW_SPEED alone govern). >0 = abort a
    // streaming generation that sends zero bytes for this many seconds
    // (true stall — not slow-but-moving models). Never hardcoded in the codec.
    int streamStallTimeoutSec = 0;  // 0 = inherit provider/LOW_SPEED default
    // Dumb tail window on history lines fed into the prompt. Compaction does
    // not replace this — it is the hard ceiling / seatbelt.
    int historyCap = 40;
    // How often to *recompute* the history_cap window (in user turns).
    // 1 = every turn (old behavior). Default 15 = clamp at most every 15 user
    // turns; between recomputes the window start is frozen so the prompt can
    // grow slightly past the cap until the next clamp.
    // 0 = never recompute after first apply (freeze first window forever).
    int maxTurnsPerCycle = 15;

    // Resilience — retry behavior for transient upstream failures (empty
    // stream, finish_reason=length, content_filter, transient HTTP errors).
    // Retries use exponential backoff between attempts and stop on the first
    // response that carries any non-thinking content.
    // Free / flaky providers (opencode, openrouter free, etc.) empty-stream often.
    // Default is generous: first try + 8 retries ≈ 9 attempts before giving up.
    int emptyResponseMaxRetries = 8;             // additional attempts after the first
    int emptyResponseInitialBackoffMs = 1500;    // first backoff
    int emptyResponseMaxBackoffMs = 45000;       // cap for exponential growth
    double emptyResponseBackoffMultiplier = 1.8; // delay multiplier per attempt
    bool retryOnFinishReasonLength = true;       // length-truncated responses
    bool retryOnFinishReasonContentFilter = true;// filtered/empty content
    std::vector<std::string> retryOnFinishReasons; // extra reasons to retry (e.g. "refusal")

    // Paths
    std::string systemPromptPath;
    std::string systemPromptText;  // if set, overrides systemPromptPath (inline prompt)
    std::string harnessPath;       // harness/protocol prompt (XML protocol spec)
    std::string personaPath;       // persona prompt (identity/values)
    std::string userPath;          // operator context (USER.md) — context.user
    std::string manifestPath;      // agent.yml file path (for live reload)
    std::string manifestDir;

    // Prompt rendering
    PromptBuildingConfig promptBuilding;

    // Sandbox — capability boundary + live path binds (see sandbox::SandboxPolicy).
    // When sandboxConfigured is true the manifest declared a sandbox: block and
    // the runtime builds a policy from the fields below (CLI --sandbox can still
    // force a preset on top).
    bool sandboxConfigured = false;
    std::string sandboxMode = "process";  // process | docker | chroot
    std::string sandboxRuntime = "";      // legacy alias for image
    std::string sandboxImage = "";        // docker image (mode: docker)
    std::string sandboxNetwork = "out";   // none | out | full (OS-level; docker/chroot)
    bool sandboxReadonly = false;         // global fs_write block
    std::vector<std::string> sandboxFiles;  // shorthand host paths → /workspace/<name>
    std::vector<SandboxBind> sandboxBinds;  // explicit host→guest[:ro] mounts
    std::vector<std::string> sandboxAllowedCommands;  // exec whitelist (* = all)
    std::vector<std::string> sandboxAllowedPaths;     // extra fs roots (empty = workspace+binds)
    std::vector<std::string> sandboxAllowedHosts;     // web_fetch host whitelist (empty = blocked)
    bool sandboxCommandsSet = false;  // distinguish omitted vs explicit []
    bool sandboxPathsSet = false;
    bool sandboxHostsSet = false;

    // Context economy — optional smart compact (see CompactionConfig).
    CompactionConfig compaction;

    // Sub-agent runtime behavior
    // memory  = keep sub-agent history in-process only
    // session = derive stable child session ids from parent session id
    std::string subAgentPersistence = "memory";

    // Environment
    std::map<std::string, std::string> environment;
};

// ---------------------------------------------------------------------------
// Agent execution context (per-run state)
// ---------------------------------------------------------------------------
// Who initiated this prompt turn. Sub-agents use this so history can
// distinguish a human operator from a parent agent delegate call.
enum class PromptSource { Human, ParentAgent, Internal };

struct AgentContext {
    std::string userInput;
    std::string sessionId;
    int iteration = 0;
    bool streaming = false;
    bool ephemeral = false;
    bool debug = false;
    bool verbose = false;
    bool raw = false;
    PromptSource source = PromptSource::Human;
    std::string sourceName;  // parent agent name when source == ParentAgent
    StreamCallback onToken;
    std::map<std::string, std::string> variables;
    std::map<std::string, Json::Value> actionResults;
};

// ---------------------------------------------------------------------------
// Session types
// ---------------------------------------------------------------------------
struct SessionRecord {
    enum Role { USER, AGENT, TOOL_CALL, TOOL_RESULT, SYSTEM };
    Role role;
    std::string content;
    std::string timestamp;
    std::string metadata;
};

struct Session {
    std::string id;
    std::string agentName;
    std::string model;
    std::string provider;
    std::string created;
    std::string updated;
    std::vector<SessionRecord> records;
    std::map<std::string, std::string> metadata;
    // LLM-injected context feeds accumulated across iterations; restored on resume
    std::vector<std::string> contextFeeds;
    // Pre-rendered TUI lines captured during the live run. On -c, we just
    // replay these into historyLines so the user sees exactly what they
    // saw before exiting — no parser, no protocol reconstruction needed.
    std::vector<std::string> renderedHistory;
    // Structured chat timeline (JSON array of {kind,title,body,ok,...}).
    // Written by the TUI from rootRows so resume paints the same blocks
    // the operator saw live — not a thin User/Agent record projection.
    std::string uiTimelineJson;
};

}  // namespace cortex::mk3
