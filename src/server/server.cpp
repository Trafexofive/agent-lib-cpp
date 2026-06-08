// =============================================================================
// agent-lib-MK3 — HTTP REST Server Implementation
// =============================================================================

#include "server.hpp"
#include "../providers/generic_openai.hpp"
#include <json/json.h>
#include <thread>
#include <iostream>

namespace cortex::mk3::server {

CortexServer::CortexServer(ServerConfig cfg) : cfg_(std::move(cfg)) {
    setupRoutes();
}

CortexServer::~CortexServer() { stop(); }

// ═══════════════════════════════════════════════════════════════════════
// Route Setup
// ═══════════════════════════════════════════════════════════════════════

void CortexServer::setupRoutes() {
    // CORS
    if (cfg_.cors) {
        svr_.Options(".*", [](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
            res.status = 204;
        });
    }

    // Auth middleware (simple bearer token)
    auto checkAuth = [this](const httplib::Request& req, httplib::Response& res) -> bool {
        if (cfg_.apiKey.empty()) return true;
        auto auth = req.get_header_value("Authorization");
        if (auth == "Bearer " + cfg_.apiKey) return true;
        res.status = 401;
        res.set_content("{\"error\":\"unauthorized\"}", "application/json");
        return false;
    };

    // ── Health ──
    svr_.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("{\"status\":\"ok\",\"version\":\"mk3-1.0\"}", "application/json");
    });

    // ── Create agent ──
    svr_.Post("/api/v1/agents", [this, checkAuth](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;

        Json::Value body;
        Json::CharReaderBuilder reader;
        std::string errs;
        std::istringstream ss(req.body);
        if (!Json::parseFromStream(reader, ss, &body, &errs)) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid json\"}", "application/json");
            return;
        }

        AgentConfig acfg;
        acfg.name = body.get("name", "agent").asString();
        acfg.provider = body.get("provider", "deepseek").asString();
        acfg.model = body.get("model", "deepseek-chat").asString();
        if (body.isMember("temperature")) acfg.temperature = body["temperature"].asDouble();
        if (body.isMember("max_tokens")) acfg.maxTokens = body["maxTokens"].asInt();
        if (body.isMember("system_prompt")) acfg.systemPromptPath = body["system_prompt"].asString();

        std::string id = createAgent(acfg.name, acfg);

        Json::Value resp;
        resp["id"] = id;
        resp["name"] = acfg.name;
        resp["provider"] = acfg.provider;
        resp["model"] = acfg.model;

        res.set_content(Json::writeString(Json::StreamWriterBuilder(), resp), "application/json");
    });

    // ── List agents ──
    svr_.Get("/api/v1/agents", [this, checkAuth](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        Json::Value arr(Json::arrayValue);
        for (auto& id : listAgents()) arr.append(id);
        res.set_content(Json::writeString(Json::StreamWriterBuilder(), arr), "application/json");
    });

    // ── Prompt (non-streaming) ──
    svr_.Post("/api/v1/agents/([^/]+)/prompt", [this, checkAuth](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        std::string agentId = req.matches[1];

        Json::Value body;
        Json::CharReaderBuilder reader;
        std::string errs;
        std::istringstream ss(req.body);
        if (!Json::parseFromStream(reader, ss, &body, &errs)) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid json\"}", "application/json");
            return;
        }

        std::string prompt = body.get("prompt", "").asString();
        std::string sessionId = body.get("session_id", "").asString();

        auto* agent = getAgent(agentId);
        if (!agent) {
            res.status = 404;
            res.set_content("{\"error\":\"agent not found\"}", "application/json");
            return;
        }

        // Lock agent for this request
        auto* inst = agents_[agentId].get();
        if (inst->busy.exchange(true)) {
            res.status = 429;
            res.set_content("{\"error\":\"agent busy\"}", "application/json");
            return;
        }

        std::string result = agent->prompt(prompt, sessionId);
        inst->busy = false;

        Json::Value resp;
        resp["response"] = result;
        res.set_content(Json::writeString(Json::StreamWriterBuilder(), resp), "application/json");
    });

    // ── Prompt (streaming SSE) ──
    svr_.Post("/api/v1/agents/([^/]+)/stream", [this, checkAuth](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        std::string agentId = req.matches[1];

        Json::Value body;
        Json::CharReaderBuilder reader;
        std::string errs;
        std::istringstream ss(req.body);
        if (!Json::parseFromStream(reader, ss, &body, &errs)) {
            res.status = 400;
            res.set_content("data: {\"error\":\"invalid json\"}\n\n", "text/event-stream");
            return;
        }

        std::string prompt = body.get("prompt", "").asString();
        std::string sessionId = body.get("session_id", "").asString();

        auto* agent = getAgent(agentId);
        if (!agent) {
            res.status = 404;
            res.set_content("data: {\"error\":\"agent not found\"}\n\n", "text/event-stream");
            return;
        }

        auto* inst = agents_[agentId].get();
        if (inst->busy.exchange(true)) {
            res.status = 429;
            res.set_content("data: {\"error\":\"agent busy\"}\n\n", "text/event-stream");
            return;
        }

        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");

        std::string fullResponse;
        agent->prompt(prompt, [&](const std::string& token, bool isFinal) {
            if (isFinal) return;
            fullResponse += token;
            std::string ev = "data: {\"type\":\"token\",\"content\":\"" +
                Json::valueToQuotedString(token.c_str()) + "\"}\n\n";
            res.body += ev;
        }, sessionId);

        std::string done = "data: {\"type\":\"done\",\"response\":\"" +
            Json::valueToQuotedString(fullResponse.c_str()) + "\"}\n\n";
        res.body += done;

        inst->busy = false;
    });

    // ── OpenAI-compatible /v1/chat/completions ──
    svr_.Post("/v1/chat/completions", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");

        Json::Value body;
        Json::CharReaderBuilder reader;
        std::string errs;
        std::istringstream ss(req.body);
        if (!Json::parseFromStream(reader, ss, &body, &errs)) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid json\"}", "application/json");
            return;
        }

        // Get or create a default agent for this endpoint
        std::string agentId = "openai-default";
        {
            std::lock_guard<std::mutex> lock(agentsMtx_);
            if (agents_.find(agentId) == agents_.end()) {
                AgentConfig acfg;
                acfg.name = "openai-compat";
                acfg.provider = body.get("model", "deepseek-chat").asString().find("deepseek") != std::string::npos
                    ? "deepseek" : "openrouter";
                acfg.model = body.get("model", "deepseek-chat").asString();

                auto provCfg = providers::deepseekConfig();
                provCfg.defaultModel = acfg.model;
                auto prov = std::make_shared<providers::GenericOpenAIClient>(provCfg);

                auto inst = std::make_unique<AgentInstance>();
                inst->id = agentId;
                inst->name = acfg.name;
                inst->provider = prov;
                inst->agent = std::make_unique<Agent>(acfg, prov);
                agents_[agentId] = std::move(inst);
            }
        }

        auto* inst = agents_[agentId].get();
        if (inst->busy.exchange(true)) {
            res.status = 429;
            res.set_content("{\"error\":\"busy\"}", "application/json");
            return;
        }

        // Extract user message
        std::string prompt;
        if (body.isMember("messages")) {
            for (auto& m : body["messages"]) {
                if (m["role"].asString() == "user") {
                    prompt = m["content"].asString();
                }
            }
        }
        if (prompt.empty()) prompt = body.get("prompt", "Hello").asString();

        bool stream = body.get("stream", false).asBool();

        if (stream) {
            res.set_header("Content-Type", "text/event-stream");
            std::string full;
            inst->agent->prompt(prompt, [&](const std::string& token, bool isFinal) {
                if (isFinal) return;
                full += token;
                Json::Value chunk;
                chunk["id"] = "chatcmpl-mk3";
                chunk["object"] = "chat.completion.chunk";
                Json::Value choice; choice["index"] = 0;
                Json::Value delta; delta["content"] = token;
                choice["delta"] = delta;
                chunk["choices"].append(choice);
                res.body += "data: " + Json::writeString(Json::StreamWriterBuilder(), chunk) + "\n\n";
            });
            res.body += "data: [DONE]\n\n";
        } else {
            std::string result = inst->agent->prompt(prompt);
            Json::Value resp;
            resp["id"] = "chatcmpl-mk3";
            resp["object"] = "chat.completion";
            resp["model"] = inst->agent->config().model;
            Json::Value choice;
            Json::Value msg; msg["role"] = "assistant"; msg["content"] = result;
            choice["message"] = msg;
            choice["index"] = 0;
            choice["finish_reason"] = "stop";
            resp["choices"].append(choice);
            Json::Value usage;
            usage["prompt_tokens"] = 0;
            usage["completion_tokens"] = 0;
            usage["total_tokens"] = 0;
            resp["usage"] = usage;
            res.set_content(Json::writeString(Json::StreamWriterBuilder(), resp), "application/json");
        }

        inst->busy = false;
    });

    // ── Delete agent ──
    svr_.Delete("/api/v1/agents/([^/]+)", [this, checkAuth](const httplib::Request& req, httplib::Response& res) {
        if (!checkAuth(req, res)) return;
        std::string agentId = req.matches[1];
        if (removeAgent(agentId)) {
            res.set_content("{\"status\":\"deleted\"}", "application/json");
        } else {
            res.status = 404;
            res.set_content("{\"error\":\"not found\"}", "application/json");
        }
    });

    // ── List providers ──
    svr_.Get("/api/v1/providers", [](const httplib::Request&, httplib::Response& res) {
        Json::Value arr(Json::arrayValue);
        arr.append("deepseek");
        arr.append("openrouter");
        arr.append("groq");
        arr.append("zen");
        arr.append("together");
        arr.append("fireworks");
        res.set_content(Json::writeString(Json::StreamWriterBuilder(), arr), "application/json");
    });
}

// ═══════════════════════════════════════════════════════════════════════
// Agent Management
// ═══════════════════════════════════════════════════════════════════════

std::string CortexServer::createAgent(const std::string& name, AgentConfig agentCfg) {
    std::lock_guard<std::mutex> lock(agentsMtx_);
    std::string id = name + "-" + std::to_string(++agentCounter_);

    // Create provider based on config
    std::shared_ptr<ILlmProvider> prov;
    if (agentCfg.provider == "deepseek") {
        auto pcfg = providers::deepseekConfig();
        pcfg.defaultModel = agentCfg.model;
        prov = std::make_shared<providers::GenericOpenAIClient>(pcfg);
    } else if (agentCfg.provider == "openrouter") {
        auto pcfg = providers::openrouterConfig();
        pcfg.defaultModel = agentCfg.model;
        prov = std::make_shared<providers::GenericOpenAIClient>(pcfg);
    } else if (agentCfg.provider == "groq") {
        auto pcfg = providers::groqConfig();
        pcfg.defaultModel = agentCfg.model;
        prov = std::make_shared<providers::GenericOpenAIClient>(pcfg);
    } else if (agentCfg.provider == "zen") {
        auto pcfg = providers::zenConfig();
        pcfg.defaultModel = agentCfg.model;
        prov = std::make_shared<providers::GenericOpenAIClient>(pcfg);
    } else {
        // Default to DeepSeek
        auto pcfg = providers::deepseekConfig();
        pcfg.defaultModel = agentCfg.model;
        prov = std::make_shared<providers::GenericOpenAIClient>(pcfg);
    }

    auto inst = std::make_unique<AgentInstance>();
    inst->id = id;
    inst->name = name;
    inst->provider = prov;
    inst->agent = std::make_unique<Agent>(agentCfg, prov);

    agents_[id] = std::move(inst);
    return id;
}

bool CortexServer::removeAgent(const std::string& id) {
    std::lock_guard<std::mutex> lock(agentsMtx_);
    return agents_.erase(id) > 0;
}

std::vector<std::string> CortexServer::listAgents() const {
    std::lock_guard<std::mutex> lock(agentsMtx_);
    std::vector<std::string> ids;
    for (auto& [id, _] : agents_) ids.push_back(id);
    return ids;
}

Agent* CortexServer::getAgent(const std::string& id) {
    std::lock_guard<std::mutex> lock(agentsMtx_);
    auto it = agents_.find(id);
    return (it != agents_.end()) ? it->second->agent.get() : nullptr;
}

// ═══════════════════════════════════════════════════════════════════════
// Start / Stop
// ═══════════════════════════════════════════════════════════════════════

void CortexServer::start() {
    running_ = true;
    std::cout << "[MK3] Server listening on " << cfg_.host << ":" << cfg_.port << "\n";
    svr_.listen(cfg_.host.c_str(), cfg_.port);
}

void CortexServer::stop() {
    running_ = false;
    svr_.stop();
}

} // namespace cortex::mk3::server
