#pragma once
// Live-agent hot-swap: external ref from main(), replaceable by an owned Agent
// built from a selected manifest (hub launch). buildAgentFromManifest wires a
// manifest end-to-end (provider, feeds, relics, tools, subagents, prompt env).

#include <exception>
#include <memory>
#include <string>
#include <utility>

#include "src/core/agent.hpp"
#include "src/core/manifest_loader.hpp"
#include "src/providers/factory.hpp"
#include "src/ui/bridge/agent_bridge.hpp"

namespace cortex::mk3::ui {

struct LiveAgentSlot {
    Agent *external = nullptr;
    std::unique_ptr<Agent> owned;
    Agent &get() {
        return owned ? *owned : *external;
    }
    Agent *ptr() {
        return owned ? owned.get() : external;
    }
};

inline std::unique_ptr<Agent>
buildAgentFromManifest(const std::string &manifestPath, AgentBridge &bridge,
                       std::string &err) {
    try {
        auto acfg = ManifestLoader::loadAgentConfig(manifestPath);
        ManifestLoader::loadEnv(manifestPath, acfg);
        if (acfg.name.empty()) {
            err = "manifest has no name: " + manifestPath;
            return nullptr;
        }
        auto provider = providers::createProvider(acfg.provider, acfg.model);
        if (!provider) {
            err = "provider unavailable: " + acfg.provider + "/" + acfg.model;
            return nullptr;
        }
        auto agent = std::make_unique<Agent>(acfg, provider);
        // Honor runtime.dev_mode + CORTEX_DEV_MODE for hub-launched agents too.
        if (acfg.devMode ||
            (std::getenv("CORTEX_DEV_MODE") &&
             std::string(std::getenv("CORTEX_DEV_MODE")) != "0" &&
             std::string(std::getenv("CORTEX_DEV_MODE")) != "false")) {
            agent->setDevMode(true);
        }
        ManifestLoader::loadFeeds(manifestPath, *agent);
        ManifestLoader::loadRelics(manifestPath, *agent);
        auto schemas = ManifestLoader::loadTools(manifestPath, *agent);
        ManifestLoader::loadSubAgents(manifestPath, *agent, acfg.provider);
        std::string workflowXml = ManifestLoader::loadWorkflows(manifestPath);
        // Same schema inject path as main.cpp CLI — hub launch used to skip
        // this and fall back to desc-only stubs (or nothing useful).
        const auto &rc = acfg.promptBuilding.runtimeCapabilities;
        std::string schemaXml = ManifestLoader::toolSchemasToXml(
            schemas, 8, rc.inputSchemas, rc.returnSchemas, rc.usageExamples);
        if (!schemaXml.empty())
            agent->setEnv("__TOOL_SCHEMAS__", schemaXml);
        if (!workflowXml.empty())
            agent->setEnv("__WORKFLOW_XML__", workflowXml);
        {
            std::string skillsXml = ManifestLoader::loadSkillsXml(manifestPath);
            if (!skillsXml.empty())
                agent->setEnv("__SKILLS_XML__", skillsXml);
            std::string modsXml =
                ManifestLoader::loadPromptModulesXml(manifestPath);
            if (!modsXml.empty())
                agent->setEnv("__PROMPT_MODULES_XML__", modsXml);
        }
        agent->setAskToolHandler([&bridge](const Json::Value &params) {
            return bridge.requestAsk(params);
        });
        return agent;
    } catch (const std::exception &e) {
        err = e.what();
        return nullptr;
    }
}
} // namespace cortex::mk3::ui
