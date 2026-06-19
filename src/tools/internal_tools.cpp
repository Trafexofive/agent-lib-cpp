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
    reg.registerFn("exec", builtins::exec);
    reg.registerFn("list", builtins::list);
    reg.registerFn("grep", builtins::grep);
    reg.registerFn("fs_read", builtins::fs_read);
    reg.registerFn("fs_write", builtins::fs_write);
    reg.registerFn("simple_fs_write", builtins::fs_write);
    reg.registerFn("json", builtins::json);
    reg.registerFn("web_fetch", builtins::web_fetch);
    reg.registerFn("sleep", builtins::sleep);
    // context_pin / peek / unpin are handled directly in Agent::dispatchTool because
    // they mutate Agent state (pinned_/peeking_ maps). Registering a stateless fallback
    // here would shadow that path.
    reg.registerFn("ask_tool", builtins::ask_tool);
}

}  // namespace cortex::mk3::tools
