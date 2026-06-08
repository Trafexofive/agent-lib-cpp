#pragma once
// =============================================================================
// agent-lib-MK3 — Core Types
// Lean, single-source-of-truth types shared across all modules.
// =============================================================================

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <json/json.h>

namespace cortex::mk3 {

// ---------------------------------------------------------------------------
// Chat messages
// ---------------------------------------------------------------------------
enum class ChatRole { SYSTEM, USER, ASSISTANT, TOOL };

struct ChatMessage {
    ChatRole role;
    std::string content;
    std::string name;       // tool name (for TOOL role)
    std::string toolCallId; // for TOOL role

    static ChatMessage system(const std::string& c)    { return {ChatRole::SYSTEM, c, {}, {}}; }
    static ChatMessage user(const std::string& c)      { return {ChatRole::USER, c, {}, {}}; }
    static ChatMessage assistant(const std::string& c) { return {ChatRole::ASSISTANT, c, {}, {}}; }
    static ChatMessage tool(const std::string& id, const std::string& name, const std::string& c) {
        return {ChatRole::TOOL, c, name, id};
    }

    static const char* roleName(ChatRole r) {
        switch (r) {
            case ChatRole::SYSTEM:    return "system";
            case ChatRole::USER:      return "user";
            case ChatRole::ASSISTANT: return "assistant";
            case ChatRole::TOOL:      return "tool";
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
    std::string type;        // "string", "number", "boolean", "object", "array"
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
    bool isNative = true;                // true = C++ callback, false = script
    std::string scriptPath;              // for script tools
    std::string scriptRuntime;           // "python3", "bash", etc.

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
        if (!output.empty()) r["output"] = output;
        if (!error.empty()) r["error"] = error;
        if (!data.isNull()) r["data"] = data;
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
    int maxTokens = 8192;
    int iterationCap = 3;   // agent has 3 turns to explore before forced response
    int actionTimeoutSec = 30;  // max seconds to wait for dispatched actions
    int historyCap = 40;

    // Paths
    std::string systemPromptPath;
    std::string systemPromptText;  // if set, overrides systemPromptPath (inline prompt)
    std::string harnessPath;       // harness/protocol prompt (XML protocol spec)
    std::string manifestDir;

    // Sandbox
    std::string sandboxMode = "process";    // process, docker, chroot
    std::string sandboxRuntime = "";         // docker image name
    std::string sandboxImage = "";           // docker image
    std::vector<std::string> sandboxFiles;   // files to mount/copy

    // Environment
    std::map<std::string, std::string> environment;
};

// ---------------------------------------------------------------------------
// Agent execution context (per-run state)
// ---------------------------------------------------------------------------
struct AgentContext {
    std::string userInput;
    std::string sessionId;
    int iteration = 0;
    bool streaming = false;
    bool ephemeral = false;
    bool debug = false;
    bool verbose = false;
    bool raw = false;
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
};

} // namespace cortex::mk3
