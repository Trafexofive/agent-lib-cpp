// src/core/builtins/context_manage.cpp — Agent-owned context lifecycle helpers
//
// Companion to context_pin / context_peek / context_unpin.
// Pinned entries persist across iterations until explicitly unpinned.
// Peek entries decrement once per iteration end (in runLoop) and evict at 0.
// Path keys are canonicalised so two requests for the same file collapse,
// and `./x.cpp`, `x.cpp`, `src/../x.cpp` all resolve to the same entry.
// Size limit is per-entry (default 64 KB); the LLM can override with
// `force: true` when it explicitly needs a large file in context.
#include "../agent.hpp"

namespace cortex::mk3 {

void Agent::tickContextCycles() {
    for (auto it = peeking_.begin(); it != peeking_.end();) {
        if (--it->second.cyclesRemaining <= 0)
            it = peeking_.erase(it);
        else
            ++it;
    }
}

std::string Agent::renderSystemPrompt() const {
    AgentContext ctx;
    std::string prompt = buildSystemPrompt(ctx);
    std::string dynamicTail = buildDynamicContextPrompt();
    if (!dynamicTail.empty()) {
        if (!prompt.empty() && prompt.back() != '\n')
            prompt += '\n';
        prompt += dynamicTail;
    }
    return prompt;
}

Json::Value Agent::contextSnapshot() const {
    Json::Value r(Json::objectValue);
    Json::Value pinned(Json::arrayValue);
    for (auto& [key, e] : pinned_) {
        Json::Value entry;
        entry["path"] = e.displayPath;
        entry["key"] = key;
        entry["bytes"] = (Json::UInt64)e.bytes;
        pinned.append(entry);
    }
    Json::Value peek(Json::arrayValue);
    for (auto& [key, e] : peeking_) {
        Json::Value entry;
        entry["path"] = e.displayPath;
        entry["key"] = key;
        entry["bytes"] = (Json::UInt64)e.bytes;
        entry["cycles_remaining"] = e.cyclesRemaining;
        peek.append(entry);
    }
    r["pinned"] = pinned;
    r["peeking"] = peek;
    return r;
}

}  // namespace cortex::mk3
