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
    struct RuntimeCapabilities {
        bool inputSchemas = true;
        bool returnSchemas = true;
        bool usageExamples = true;
    };

    RuntimeCapabilities runtimeCapabilities;
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
    // Manifest runtime defaults (CLI flags override when explicitly set).
    // noSession  → do not load/save session for this agent run
    // ephemeral  → prefer exit-on-done / one-shot lifecycle at the app layer
    bool defaultNoSession = false;
    bool defaultEphemeral = false;
    // dev_mode → auto-dump full LLM-facing iterations + raw stream + history
    // per session under ~/.cortex/dev/<session>/ (and CWD copies for lazy open).
    bool devMode = false;
    int maxTokens = 0;
    int iterationCap =
        50;  // agent turns before forced response (override via manifest max_iterations)
    int actionTimeoutSec = 30;  // max seconds to wait for dispatched actions
    int historyCap = 40;

    // Resilience — retry behavior for transient upstream failures (empty
    // stream, finish_reason=length, content_filter, transient HTTP errors).
    // Retries use exponential backoff between attempts and stop on the first
    // response that carries any non-thinking content.
    int emptyResponseMaxRetries = 2;             // additional attempts after the first
    int emptyResponseInitialBackoffMs = 1000;    // first backoff
    int emptyResponseMaxBackoffMs = 30000;       // cap for exponential growth
    double emptyResponseBackoffMultiplier = 2.0; // delay multiplier per attempt
    bool retryOnFinishReasonLength = true;       // length-truncated responses
    bool retryOnFinishReasonContentFilter = true;// filtered/empty content
    std::vector<std::string> retryOnFinishReasons; // extra reasons to retry (e.g. "refusal")

    // Paths
    std::string systemPromptPath;
    std::string systemPromptText;  // if set, overrides systemPromptPath (inline prompt)
    std::string harnessPath;       // harness/protocol prompt (XML protocol spec)
    std::string personaPath;       // persona prompt (identity/values)
    std::string manifestDir;

    // Prompt rendering
    PromptBuildingConfig promptBuilding;

    // Sandbox
    std::string sandboxMode = "process";    // process, docker, chroot
    std::string sandboxRuntime = "";        // docker image name
    std::string sandboxImage = "";          // docker image
    std::vector<std::string> sandboxFiles;  // files to mount/copy

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
};

}  // namespace cortex::mk3
