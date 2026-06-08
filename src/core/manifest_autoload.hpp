#pragma once
// =============================================================================
// agent-lib-MK3 — Manifest Autoload
// Scans a manifest root (default: ./manifests) and imports globally-scoped
// tools, feeds, relics, workflows, and agents without coupling the logic to CLI.
// =============================================================================

#include "agent.hpp"
#include "manifest_loader.hpp"
#include "../feeds/feed_engine.hpp"
#include "../providers/factory.hpp"
#include "../workflows/workflow_engine.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace cortex::mk3 {

struct ManifestAutoloadReport {
    std::vector<std::string> tools;
    std::vector<std::string> feeds;
    std::vector<std::string> relics;
    std::vector<std::string> agents;
    std::vector<std::string> workflows;
    std::vector<ToolSchema> toolSchemas;
    std::string workflowXml;
};

class ManifestAutoload {
public:
    static ManifestAutoloadReport loadGlobal(const std::string& rootDir,
                                             Agent& agent,
                                             const std::string& providerName,
                                             const std::string& skipAgentManifest = "") {
        ManifestAutoloadReport report;
        if (rootDir.empty() || !fs::exists(rootDir)) return report;

        std::set<std::string> seenTools;
        std::set<std::string> seenFeeds;
        std::set<std::string> seenRelics;
        std::set<std::string> seenAgents;
        std::set<std::string> seenWorkflows;

        for (auto it = fs::recursive_directory_iterator(rootDir); it != fs::recursive_directory_iterator(); ++it) {
            if (!it->is_regular_file()) continue;
            const fs::path p = it->path();
            const std::string file = p.filename().string();

            if (file == "tool.yml") {
                auto schema = ManifestLoader::loadToolManifest(p.string());
                if (schema.name.empty() || seenTools.count(schema.name)) continue;
                seenTools.insert(schema.name);
                report.tools.push_back(schema.name);
                report.toolSchemas.push_back(schema);

                ToolDef td;
                td.name = schema.name;
                td.description = schema.description;
                if (!schema.runtime.empty() && !schema.entrypoint.empty()) {
                    td.isNative = false;
                    td.scriptRuntime = schema.runtime;
                    td.scriptPath = (p.parent_path() / schema.entrypoint).string();
                }
                agent.addTool(td);
            }
            else if (file == "feed.yml") {
                auto mr = feeds::FeedEngine::instance().loadFeedManifest(p.string());
                if (!mr.success || mr.name.empty() || seenFeeds.count(mr.name)) continue;
                seenFeeds.insert(mr.name);
                report.feeds.push_back(mr.name);
                agent.addFeed(mr.name);
            }
            else if (file == "relic.yml") {
                std::string name = manifestName(p.string(), p.parent_path().filename().string());
                if (name.empty() || seenRelics.count(name)) continue;
                seenRelics.insert(name);
                report.relics.push_back(name);
                agent.addRelic(name);
            }
            else if (file == "agent.yml") {
                if (!skipAgentManifest.empty() && samePath(p, fs::path(skipAgentManifest))) continue;
                auto cfg = ManifestLoader::loadAgentConfig(p.string());
                if (cfg.name.empty() || seenAgents.count(cfg.name)) continue;
                seenAgents.insert(cfg.name);
                cfg.provider = providerName.empty() ? cfg.provider : providerName;
                auto provider = providers::createProvider(cfg.provider, cfg.model);
                if (!provider) continue;
                auto sub = std::make_shared<Agent>(cfg, provider);
                agent.addSubAgent(sub);
                report.agents.push_back(cfg.name);
            }
            else if (isWorkflowCandidate(p, rootDir)) {
                auto wf = workflows::WorkflowEngine::instance().load(p.string());
                if (wf.name.empty() || seenWorkflows.count(wf.name)) continue;
                seenWorkflows.insert(wf.name);
                report.workflows.push_back(wf.name);
                report.workflowXml += workflows::WorkflowEngine::instance().toXml(wf);
            }
        }

        return report;
    }

private:
    static std::string readFile(const std::string& path) {
        std::ifstream f(path);
        if (!f) return "";
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    static std::string manifestName(const std::string& path, const std::string& fallback) {
        auto yaml = readFile(path);
        if (yaml.empty()) return fallback;
        auto root = ManifestYaml::parse(yaml);
        auto n = ManifestYaml::get(root, "name");
        return n.empty() ? fallback : n;
    }

    static bool samePath(const fs::path& a, const fs::path& b) {
        std::error_code ec;
        return fs::weakly_canonical(a, ec) == fs::weakly_canonical(b, ec);
    }

    static bool isWorkflowCandidate(const fs::path& p, const std::string& rootDir) {
        if (p.extension() != ".yml" && p.extension() != ".yaml") return false;
        if (p.filename() == "tool.yml" || p.filename() == "feed.yml" ||
            p.filename() == "relic.yml" || p.filename() == "agent.yml" ||
            p.filename() == "config.yml") return false;
        auto rel = fs::relative(p, rootDir).string();
        return rel.find("workflows") != std::string::npos;
    }
};

} // namespace cortex::mk3
