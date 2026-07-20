// src/core/agent_tool_mgmt.cpp — tool registration surface on Agent
#include "agent.hpp"

namespace cortex::mk3 {

void Agent::addTool(tools::Tool tool) {
    tools_[tool.name()] = std::move(tool);
}

void Agent::removeTool(const std::string& name) {
    tools_.erase(name);
    disabledBuiltins_.insert(name);
}

Json::Value Agent::toggleBuiltin(const Json::Value& params, bool enable) {
    Json::Value r;
    r["success"] = true;
    std::string name = params.get("name", "").asString();
    if (name.empty() && params.isMember("names") && params["names"].isArray() &&
        params["names"].size() > 0)
        name = params["names"][0].asString();
    if (name.empty()) {
        r["success"] = false;
        r["error"] = "name or names required";
        return r;
    }
    if (enable) {
        disabledBuiltins_.erase(name);
        r["enabled"] = name;
    } else {
        tools_.erase(name);
        disabledBuiltins_.insert(name);
        r["disabled"] = name;
    }
    return r;
}

bool Agent::hasTool(const std::string& name) const {
    // Only consider tools explicitly granted to this agent — not the global
    // registry
    return tools_.count(name);
}

const tools::Tool* Agent::findTool(const std::string& name) const {
    auto it = tools_.find(name);
    return (it != tools_.end()) ? &it->second : nullptr;
}

std::vector<std::string> Agent::toolNames() const {
    std::vector<std::string> names;
    names.reserve(tools_.size());
    for (auto& [name, _] : tools_)
        names.push_back(name);
    return names;
}

// ═══════════════════════════════════════════════════════════════════════

}  // namespace cortex::mk3
