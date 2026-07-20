// src/core/agent_env.cpp — Agent environment map
#include "agent.hpp"

namespace cortex::mk3 {

void Agent::setEnv(const std::string& key, const std::string& val) {
    env_[key] = val;
}

std::string Agent::getEnv(const std::string& key, const std::string& def) const {
    auto it = env_.find(key);
    return (it != env_.end()) ? it->second : def;
}

}  // namespace cortex::mk3
