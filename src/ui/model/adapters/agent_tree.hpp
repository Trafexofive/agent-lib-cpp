#pragma once
// Sub-agent tree/path helpers. Header-only and rendering-free.

#include <set>
#include <string>
#include <vector>

#include "src/core/agent.hpp"
#include "src/ui/model/timeline_model.hpp"

namespace cortex::mk3::ui::model {

inline Agent* resolveAgentPath(Agent* root, const AgentPath& path) {
    if (!root) return nullptr;
    Agent* cur = root;
    for (const auto& part : path.parts) {
        cur = cur->getSubAgent(part);
        if (!cur) return nullptr;
    }
    return cur;
}

inline std::set<std::string> childAgentNameSet(const Agent* agent) {
    std::set<std::string> out;
    if (!agent) return out;
    for (const auto& name : agent->subAgentNames()) out.insert(name);
    return out;
}

// Generic path resolver for tests and future non-Agent stores.
template <typename ChildExistsFn>
bool pathExists(const AgentPath& path, ChildExistsFn childExists) {
    AgentPath cur;
    for (const auto& part : path.parts) {
        if (!childExists(cur, part)) return false;
        cur = cur.child(part);
    }
    return true;
}

}  // namespace cortex::mk3::ui::model
