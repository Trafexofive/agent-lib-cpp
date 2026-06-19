// src/core/builtins/context_unpin.cpp — Agent-owned context_unpin builtin
#include "../agent.hpp"
#include "context_common.hpp"

namespace cortex::mk3 {

Json::Value Agent::contextUnpin(const std::string& path) {
    if (path.empty())
        return agent_builtins::contextErr("path is required");
    std::string key = agent_builtins::canonicaliseContextKey(path);
    bool removedPin = pinned_.erase(key) > 0;
    bool removedPeek = peeking_.erase(key) > 0;
    Json::Value r;
    r["success"] = removedPin || removedPeek;
    r["path"] = path;
    r["removed"] = removedPin ? "pinned" : (removedPeek ? "peek" : "none");
    r["pinned_count"] = (int)pinned_.size();
    r["peek_count"] = (int)peeking_.size();
    if (!removedPin && !removedPeek)
        r["error"] = "not in context: " + path;
    return r;
}

}  // namespace cortex::mk3
