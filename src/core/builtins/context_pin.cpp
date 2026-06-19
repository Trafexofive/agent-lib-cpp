// src/core/builtins/context_pin.cpp — Agent-owned context_pin builtin
#include "../agent.hpp"
#include "context_common.hpp"

#include <fstream>

namespace cortex::mk3 {

Json::Value Agent::contextPin(const std::string& path, bool force) {
    if (path.empty())
        return agent_builtins::contextErr("path is required");
    std::string key = agent_builtins::canonicaliseContextKey(path);

    std::ifstream f(key);
    if (!f) {
        f.open(path);
        if (!f)
            return agent_builtins::contextErr("file not found: " + path);
        key = agent_builtins::canonicaliseContextKey(path);
    }
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!force && content.size() > kContextSizeLimit) {
        return agent_builtins::contextErr("size " + std::to_string(content.size()) + " exceeds limit " +
                                          std::to_string(kContextSizeLimit) + " (override with force: true)");
    }

    peeking_.erase(key);
    PinnedEntry e;
    e.displayPath = path;
    e.content = std::move(content);
    e.bytes = e.content.size();
    pinned_[key] = std::move(e);

    Json::Value r;
    r["success"] = true;
    r["path"] = path;
    r["bytes"] = (Json::UInt64)pinned_[key].bytes;
    r["mode"] = "pinned";
    r["pinned_count"] = (int)pinned_.size();
    r["peek_count"] = (int)peeking_.size();
    return r;
}

}  // namespace cortex::mk3
