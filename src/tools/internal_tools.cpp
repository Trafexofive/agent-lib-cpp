// src/tools/internal_tools.cpp — native builtin registration and aliases
#include <map>
#include <string>

#include "builtins/builtins.hpp"
#include "builtins/common.hpp"
#include "registry.hpp"

namespace cortex::mk3::tools {

static const std::map<std::string, std::string> ALIASES = {
    {"execute", "exec"},    {"cmd", "exec"},          {"run", "exec"},
    {"search", "grep"},     {"find", "grep"},         {"read", "fs_read"},
    {"cat", "fs_read"},     {"write", "fs_write"},    {"save", "fs_write"},
    {"fetch", "web_fetch"}, {"curl", "web_fetch"},    {"http", "web_fetch"},
    {"pin", "context_pin"}, {"peek", "context_peek"}, {"unpin", "context_unpin"},
};

std::string dispatch(const std::string& toolName, const Json::Value& params) {
    auto& reg = ToolRegistry::instance();
    auto fn = reg.get(toolName);
    if (fn)
        return fn(params);
    auto it = ALIASES.find(toolName);
    if (it != ALIASES.end()) {
        fn = reg.get(it->second);
        if (fn)
            return fn(params);
    }
    return builtins::jsonErr("unknown tool: " + toolName);
}

void registerDefaults() {
    static bool done = false;
    if (done)
        return;
    done = true;
    auto& reg = ToolRegistry::instance();
    reg.registerStreamingFn("exec", builtins::execStreaming);
    reg.registerStreamingFn("list", builtins::listStreaming);
    reg.registerStreamingFn("grep", builtins::grepStreaming);
    reg.registerStreamingFn("fs_read", builtins::fsReadStreaming);
    reg.registerStreamingFn("fs_write", builtins::fsWriteStreaming);
    reg.registerStreamingFn("simple_fs_write", builtins::fsWriteStreaming);
    reg.registerStreamingFn("json", builtins::jsonStreaming);
    reg.registerStreamingFn("web_fetch", builtins::webFetchStreaming);
    reg.registerStreamingFn("sleep", builtins::sleepStreaming);
    reg.registerStreamingFn("artifact", builtins::artifactStreaming);
    // context_pin / peek / unpin are handled directly in Agent::dispatchTool because
    // they mutate Agent state (pinned_/peeking_ maps). Registering a stateless fallback
    // here would shadow that path.
    reg.registerStreamingFn("ask_tool", builtins::askToolStreaming);
}

}  // namespace cortex::mk3::tools
