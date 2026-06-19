// src/core/builtins/context_peek.cpp — Agent-owned context_peek builtin
#include "../agent.hpp"
#include "context_common.hpp"

#include <fstream>

namespace cortex::mk3 {

Json::Value Agent::contextPeek(const std::string& path, int cycles, bool force) {
    if (path.empty())
        return agent_builtins::contextErr("path is required");
    if (cycles < 0)
        cycles = 1;
    if (cycles == 0)
        cycles = 1;
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

    if (pinned_.count(key)) {
        Json::Value r;
        r["success"] = true;
        r["path"] = path;
        r["mode"] = "pinned";
        r["note"] = "already pinned; peek ignored";
        return r;
    }

    PeekEntry e;
    e.displayPath = path;
    e.content = std::move(content);
    e.bytes = e.content.size();
    e.cyclesRemaining = cycles;
    peeking_[key] = std::move(e);

    Json::Value r;
    r["success"] = true;
    r["path"] = path;
    r["bytes"] = (Json::UInt64)peeking_[key].bytes;
    r["mode"] = "peek";
    r["cycles_remaining"] = cycles;
    r["pinned_count"] = (int)pinned_.size();
    r["peek_count"] = (int)peeking_.size();
    return r;
}

}  // namespace cortex::mk3
