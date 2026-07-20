// src/core/agent_subagents.cpp — sub-agent registry on Agent
#include "agent.hpp"

namespace cortex::mk3 {

void Agent::addSubAgent(std::shared_ptr<Agent> agent) {
    subAgents_[agent->name()] = std::move(agent);
}

void Agent::removeSubAgent(const std::string& name) {
    subAgents_.erase(name);
}

bool Agent::hasSubAgent(const std::string& name) const {
    return subAgents_.count(name);
}

Agent* Agent::getSubAgent(const std::string& name) const {
    auto it = subAgents_.find(name);
    return (it != subAgents_.end()) ? it->second.get() : nullptr;
}

}  // namespace cortex::mk3
