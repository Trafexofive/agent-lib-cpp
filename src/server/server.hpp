#pragma once
// =============================================================================
// agent-lib-MK3 — HTTP REST Server
// Wraps Agent instances behind a REST API (OpenAI-compatible /chat/completions)
// =============================================================================

#include "../core/agent.hpp"
#include <httplib.h>
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>

namespace cortex::mk3::server {

struct ServerConfig {
    std::string host = "0.0.0.0";
    int port = 8080;
    int threads = 4;
    std::string apiKey;  // empty = no auth
    bool cors = true;
};

class CortexServer {
public:
    explicit CortexServer(ServerConfig cfg);
    ~CortexServer();

    // Start the server (blocking)
    void start();

    // Stop the server
    void stop();

    // Agent management
    std::string createAgent(const std::string& name, AgentConfig agentCfg);
    bool removeAgent(const std::string& id);
    std::vector<std::string> listAgents() const;
    Agent* getAgent(const std::string& id);

private:
    void setupRoutes();

    ServerConfig cfg_;
    httplib::Server svr_;
    std::atomic<bool> running_{false};

    struct AgentInstance {
        std::string id;
        std::string name;
        LlmProviderPtr provider;
        std::unique_ptr<Agent> agent;
        std::mutex mtx;
        std::atomic<bool> busy{false};
    };

    std::map<std::string, std::unique_ptr<AgentInstance>> agents_;
    mutable std::mutex agentsMtx_;
    int agentCounter_ = 0;
};

} // namespace cortex::mk3::server
