// ─────────────────────────────────────────────────────────────────────────────
// Manifest Loader — parses agent.yml, loads tools/agents/relics, populates config
// Supports: sandbox mode, file imports, schema injection, config.yml overrides
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <json/json.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../core/agent.hpp"
#include "../core/types.hpp"
#include "../feeds/feed_engine.hpp"
#include "../providers/factory.hpp"
#include "../relics/builtin_relics.hpp"
#include "../relics/docker_dispatcher.hpp"
#include "../workflows/workflow_engine.hpp"
#include "mini_yaml.hpp"

namespace cortex {
namespace mk3 {

namespace fs = std::filesystem;

// ── Tool schema (from tool.yml) ──
struct ToolSchema {
    std::string name;
    std::string description;
    std::string inputSchema;         // JSON string
    std::string outputSchema;        // JSON string
    std::string examples;            // JSON string
    std::string runtime;             // python3, builtin, etc.
    std::string entrypoint;          // script path
    std::string inputType = "json";  // action body mode: json | text
    std::string textParam;           // where text body lands for text mode
};

// ── Manifest loader ──
class ManifestLoader {
   public:
    // ML01: classify an import-list entry.
    //   - Names ending in .yml → path
    //   - Names starting with ./, ../, or / → path
    //   - Everything else (including "builtin/exec") → bare built-in name
    //
    // The legacy `name.find('/')` test broke `builtin/exec` because it routed
    // a documentation-style prefix into the path branch.
    static bool isPathImport(const std::string& raw) {
        if (raw.empty())
            return false;
        if (raw.size() >= 4 && raw.substr(raw.size() - 4) == ".yml")
            return true;
        if (raw[0] == '/')
            return true;
        if (raw.size() >= 2 && raw[0] == '.' && (raw[1] == '/' || raw[1] == '.'))
            return true;
        return false;
    }

    // Strip a leading "builtin/" prefix from a non-path import name so
    // `builtin/exec` becomes the same as `exec`.
    static std::string stripBuiltinPrefix(const std::string& raw) {
        const std::string prefix = "builtin/";
        if (raw.size() > prefix.size() && raw.compare(0, prefix.size(), prefix) == 0)
            return raw.substr(prefix.size());
        return raw;
    }

    // Load an agent manifest from path, populate config
    static AgentConfig loadAgentConfig(const std::string& manifestPath) {
        AgentConfig cfg;
        auto yaml = readFile(manifestPath);
        if (yaml.empty())
            return cfg;

        auto root = ManifestYaml::parse(yaml);
        auto* kind = ManifestYaml::find(root, "kind");
        if (!kind || kind->value != "Agent")
            return cfg;

        cfg.name = ManifestYaml::get(root, "name", "agent");
        cfg.version = ManifestYaml::get(root, "version", "1.0");
        cfg.summary = ManifestYaml::get(root, "summary");

        // Cognitive engine
        auto* engine = ManifestYaml::find(root, "cognitive_engine");
        if (engine) {
            auto* primary = ManifestYaml::find(*engine, "primary");
            if (primary) {
                cfg.provider = ManifestYaml::get(*primary, "provider", cfg.provider);
                cfg.model = ManifestYaml::get(*primary, "model", cfg.model);
                auto* params = ManifestYaml::find(*primary, "parameters");
                if (params) {
                    std::string temp = ManifestYaml::get(*params, "temperature");
                    if (!temp.empty())
                        cfg.temperature = std::stod(temp);
                    std::string mt = ManifestYaml::get(*params, "max_tokens");
                    if (!mt.empty())
                        cfg.maxTokens = std::stoi(mt);
                    std::string tp = ManifestYaml::get(*params, "top_p");
                    if (!tp.empty())
                        cfg.topP = std::stod(tp);
                    std::string tk = ManifestYaml::get(*params, "top_k");
                    if (!tk.empty())
                        cfg.topK = std::stoi(tk);
                    std::string pp = ManifestYaml::get(*params, "presence_penalty");
                    if (!pp.empty())
                        cfg.presencePenalty = std::stod(pp);
                    std::string fp = ManifestYaml::get(*params, "frequency_penalty");
                    if (!fp.empty())
                        cfg.frequencyPenalty = std::stod(fp);
                }
            }
            auto* fallback = ManifestYaml::find(*engine, "fallback");
            if (fallback) {
                cfg.fallbackProvider = ManifestYaml::get(*fallback, "provider");
                cfg.fallbackModel = ManifestYaml::get(*fallback, "model");
            }
        }

        // Persona
        auto* persona = ManifestYaml::find(root, "persona");
        if (persona) {
            std::string agentPath = ManifestYaml::get(*persona, "agent");
            if (!agentPath.empty()) {
                fs::path base = fs::path(manifestPath).parent_path();
                cfg.systemPromptPath = (base / agentPath).string();
            }
        }

        // Runtime config
        auto* runtime = ManifestYaml::find(root, "runtime");
        if (runtime) {
            std::string ic = ManifestYaml::get(*runtime, "max_iterations");
            if (!ic.empty())
                cfg.iterationCap = std::stoi(ic);
            std::string hc = ManifestYaml::get(*runtime, "history_cap");
            if (!hc.empty())
                cfg.historyCap = std::stoi(hc);
            auto* subagents = ManifestYaml::find(*runtime, "subagents");
            if (subagents) {
                std::string persistence = ManifestYaml::get(*subagents, "persistence");
                if (!persistence.empty())
                    cfg.subAgentPersistence = persistence;
            }
        }

        // Context management: implemented knobs are wired directly to AgentConfig;
        // unimplemented policy/budget knobs are intentionally commented in manifests.
        auto* context = ManifestYaml::find(root, "context");
        if (context) {
            std::string ic = ManifestYaml::get(*context, "max_iterations");
            if (!ic.empty())
                cfg.iterationCap = std::stoi(ic);
            std::string hc = ManifestYaml::get(*context, "history_cap");
            if (!hc.empty())
                cfg.historyCap = std::stoi(hc);
            std::string ats = ManifestYaml::get(*context, "action_timeout_sec");
            if (!ats.empty())
                cfg.actionTimeoutSec = std::stoi(ats);
        }

        // Sandbox
        auto* sandbox = ManifestYaml::find(root, "sandbox");
        if (sandbox) {
            cfg.sandboxMode = ManifestYaml::get(*sandbox, "mode", "process");
            cfg.sandboxRuntime = ManifestYaml::get(*sandbox, "runtime", "");
            cfg.sandboxImage = ManifestYaml::get(*sandbox, "image", "");
        }

        // Manifest directory for resolving relative paths
        cfg.manifestDir = fs::path(manifestPath).parent_path().string();

        return cfg;
    }

    // Load tools from import list, resolve local paths, return loaded schemas
    static void loadFeeds(const std::string& manifestPath, Agent& agent) {
        auto yaml = readFile(manifestPath);
        if (yaml.empty())
            return;
        auto root = ManifestYaml::parse(yaml);
        auto* importNode = ManifestYaml::find(root, "import");
        if (!importNode)
            return;
        auto feedNames = ManifestYaml::getList(*importNode, "feeds");
        for (auto& name : feedNames) {
            if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
                name = name.substr(1, name.size() - 2);
            if (isPathImport(name)) {
                fs::path feedPath = fs::path(manifestPath).parent_path() / name;
                if (!fs::exists(feedPath)) {
                    std::cerr << "[manifest] feed path not found: " << feedPath.string()
                              << " (imported from " << manifestPath << ")\n";
                    continue;
                }
                auto mr = feeds::FeedEngine::instance().loadFeedManifest(feedPath.string());
                if (mr.success && !mr.name.empty())
                    agent.addFeed(mr.name);
            } else {
                agent.addFeed(stripBuiltinPrefix(name));
            }
        }
    }

    static void loadRelics(const std::string& manifestPath, Agent& agent) {
        auto yaml = readFile(manifestPath);
        if (yaml.empty())
            return;
        auto root = ManifestYaml::parse(yaml);
        auto* importNode = ManifestYaml::find(root, "import");
        if (!importNode)
            return;
        auto relicNames = ManifestYaml::getList(*importNode, "relics");
        for (auto& name : relicNames) {
            if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
                name = name.substr(1, name.size() - 2);
            if (isPathImport(name)) {
                fs::path relicPath = fs::path(manifestPath).parent_path() / name;
                fs::path relicYml = relicPath / "relic.yml";
                if (!fs::exists(relicYml)) {
                    std::cerr << "[manifest] relic path not found: " << relicYml.string()
                              << " (imported from " << manifestPath << ")\n";
                    continue;
                }
                auto rc = loadRelicConfig(relicYml.string());
                relics::DockerRelicDispatcher::instance().loadRelic(relicPath.string());
                if (!rc.baseUrl.empty())
                    relics::RelicDispatcher::instance().registerHttpRelic(name, rc.baseUrl);
            }
            agent.addRelic(name);
        }
    }

    static void loadEnv(const std::string& manifestPath, AgentConfig& cfg) {
        auto yaml = readFile(manifestPath);
        if (yaml.empty())
            return;
        auto root = ManifestYaml::parse(yaml);
        auto* importNode = ManifestYaml::find(root, "import");
        if (!importNode)
            return;
        auto envList = ManifestYaml::getList(*importNode, "env");
        for (auto& entry : envList) {
            if (entry.size() >= 2 && entry.front() == '"' && entry.back() == '"')
                entry = entry.substr(1, entry.size() - 2);
            size_t eq = entry.find('=');
            if (eq != std::string::npos)
                cfg.environment[entry.substr(0, eq)] = entry.substr(eq + 1);
        }
    }

    static std::vector<ToolSchema> loadTools(const std::string& manifestPath, Agent& agent) {
        auto yaml = readFile(manifestPath);
        if (yaml.empty())
            return {};

        auto root = ManifestYaml::parse(yaml);
        auto* importNode = ManifestYaml::find(root, "import");
        if (!importNode)
            return {};

        auto toolNames = ManifestYaml::getList(*importNode, "tools");
        std::vector<ToolSchema> schemas;

        for (auto& name : toolNames) {
            // Trim quotes if present
            if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
                name = name.substr(1, name.size() - 2);

            if (isPathImport(name)) {
                fs::path toolPath = fs::path(manifestPath).parent_path() / name;
                if (!fs::exists(toolPath)) {
                    std::cerr << "[manifest] tool path not found: " << toolPath.string()
                              << " (imported from " << manifestPath << ")\n";
                    continue;
                }
                auto schema = loadToolSchema(toolPath.string());
                if (!schema.name.empty()) {
                    schemas.push_back(schema);
                    ToolDef td;
                    td.name = schema.name;
                    td.description = schema.description;
                    td.inputType = schema.inputType.empty() ? "json" : schema.inputType;
                    td.textParam = schema.textParam;
                    if (!schema.runtime.empty() && !schema.entrypoint.empty()) {
                        td.isNative = false;
                        td.scriptRuntime = schema.runtime;
                        td.scriptPath = (toolPath.parent_path() / schema.entrypoint).string();
                    }
                    agent.addTool(td);
                }
            } else {
                // Bare name (incl. legacy "builtin/exec" prefix) → built-in tool.
                std::string bareName = stripBuiltinPrefix(name);
                auto schema = loadBuiltinToolSchema(bareName);
                if (!schema.name.empty()) {
                    schemas.push_back(schema);
                    ToolDef td;
                    td.name = schema.name;
                    td.description = schema.description;
                    td.inputType = schema.inputType.empty() ? "json" : schema.inputType;
                    td.textParam = schema.textParam;
                    agent.addTool(td);
                } else {
                    ToolDef td;
                    td.name = bareName;
                    agent.addTool(td);
                }
            }
        }
        return schemas;
    }

    // Load sub-agents from import list
    static void loadSubAgents(const std::string& manifestPath, Agent& agent,
                              const std::string& providerName) {
        auto yaml = readFile(manifestPath);
        if (yaml.empty())
            return;

        auto root = ManifestYaml::parse(yaml);
        auto* importNode = ManifestYaml::find(root, "import");
        if (!importNode)
            return;

        auto agentNames = ManifestYaml::getList(*importNode, "agents");
        for (auto& name : agentNames) {
            // Look for sub-agent: ./agents/<name>/agent.yml first, then ../<name>/agent.yml
            fs::path base = fs::path(manifestPath).parent_path();
            fs::path agentManifest = base / "agents" / name / "agent.yml";
            if (!fs::exists(agentManifest)) {
                agentManifest = base.parent_path() / name / "agent.yml";
            }
            if (!fs::exists(agentManifest)) {
                continue;
            }

            auto subCfg = loadAgentConfig(agentManifest.string());
            // Sub-agents are explicit manifest scopes. Their cognitive_engine
            // belongs to their own agent.yml and must not be overwritten by the
            // parent provider/model.
            (void)providerName;

            auto provider = providers::createProvider(subCfg.provider, subCfg.model);
            if (!provider) {
                continue;
            }

            auto subAgent = std::make_shared<Agent>(subCfg, provider);
            loadTools(agentManifest.string(), *subAgent);
            loadFeeds(agentManifest.string(), *subAgent);
            loadRelics(agentManifest.string(), *subAgent);
            agent.addSubAgent(subAgent);
        }
    }

    // Load workflows from import section
    static std::string loadWorkflows(const std::string& manifestPath) {
        auto yaml = readFile(manifestPath);
        if (yaml.empty())
            return "";

        auto root = ManifestYaml::parse(yaml);
        auto* importNode = ManifestYaml::find(root, "import");
        if (!importNode)
            return "";

        auto wfList = ManifestYaml::getList(*importNode, "workflows");
        if (wfList.empty())
            return "";

        std::ostringstream ss;
        for (auto& wfName : wfList) {
            fs::path wfPath = fs::path(manifestPath).parent_path() / wfName;
            if (!fs::exists(wfPath)) {
                // Try manifests/workflows/
                wfPath = fs::path("manifests/workflows") / (wfName + ".yml");
                if (!fs::exists(wfPath))
                    continue;
            }
            auto& wf = workflows::WorkflowEngine::instance().load(wfPath.string());
            if (wf.isValid()) {
                ss << workflows::WorkflowEngine::instance().toXml(wf.manifest());
            }
        }
        return ss.str();
    }

    // Load files from import section
    static std::vector<std::string> loadFiles(const std::string& manifestPath) {
        auto yaml = readFile(manifestPath);
        if (yaml.empty())
            return {};

        auto root = ManifestYaml::parse(yaml);
        auto* importNode = ManifestYaml::find(root, "import");
        if (!importNode)
            return {};

        auto files = ManifestYaml::getList(*importNode, "files");
        std::vector<std::string> resolved;
        for (auto& f : files) {
            if (fs::path(f).is_absolute()) {
                resolved.push_back(f);
            } else {
                resolved.push_back((fs::path(manifestPath).parent_path() / f).string());
            }
        }
        return resolved;
    }

    // Load config.yml overrides
    static void loadConfigOverrides(const std::string& manifestPath, AgentConfig& cfg) {
        fs::path configPath = fs::path(manifestPath).parent_path() / "config.yml";
        if (!fs::exists(configPath))
            return;

        auto yaml = readFile(configPath.string());
        if (yaml.empty())
            return;

        auto root = ManifestYaml::parse(yaml);
        auto* runtime = ManifestYaml::find(root, "runtime");
        if (runtime) {
            std::string ic = ManifestYaml::get(*runtime, "max_iterations");
            if (!ic.empty())
                cfg.iterationCap = std::stoi(ic);
            std::string hc = ManifestYaml::get(*runtime, "history_cap");
            if (!hc.empty())
                cfg.historyCap = std::stoi(hc);
        }

        auto* agentNode = ManifestYaml::find(root, "agent");
        if (agentNode) {
            std::string temp = ManifestYaml::get(*agentNode, "temperature");
            if (!temp.empty())
                cfg.temperature = std::stod(temp);
            std::string mdl = ManifestYaml::get(*agentNode, "model");
            if (!mdl.empty())
                cfg.model = mdl;
            std::string prov = ManifestYaml::get(*agentNode, "provider");
            if (!prov.empty())
                cfg.provider = prov;
        }
    }

    // Build tool schemas XML for prompt injection
    static std::string toolSchemasToXml(const std::vector<ToolSchema>& schemas,
                                        int baseIndent = 8) {
        if (schemas.empty())
            return "";
        std::ostringstream ss;
        std::string toolPad(baseIndent, ' ');
        std::string fieldPad(baseIndent + 4, ' ');
        for (auto& s : schemas) {
            ss << toolPad << "<tool name=\"" << s.name << "\">\n";
            if (!s.description.empty())
                ss << fieldPad << "<description>" << s.description << "</description>\n";
            if (s.inputType != "json" || !s.textParam.empty()) {
                ss << fieldPad << "<input mode=\"" << s.inputType << "\"";
                if (!s.textParam.empty())
                    ss << " text_param=\"" << s.textParam << "\"";
                ss << ">";
                if (!s.textParam.empty())
                    ss << "Text action bodies are assigned to params." << s.textParam << ".";
                ss << "</input>\n";
            }
            if (!s.inputSchema.empty())
                ss << fieldPad << "<params>\n"
                   << indentBlock(prettyJson(s.inputSchema), baseIndent + 8) << fieldPad
                   << "</params>\n";
            if (!s.outputSchema.empty())
                ss << fieldPad << "<returns>\n"
                   << indentBlock(prettyJson(s.outputSchema), baseIndent + 8) << fieldPad
                   << "</returns>\n";
            if (!s.examples.empty())
                ss << fieldPad << "<examples>\n"
                   << indentBlock(prettyJson(s.examples), baseIndent + 8) << fieldPad
                   << "</examples>\n";
            ss << toolPad << "</tool>\n";
        }
        return ss.str();
    }

    static std::string indentBlock(const std::string& text, int spaces) {
        std::ostringstream out;
        std::istringstream in(text);
        std::string line;
        std::string pad(spaces, ' ');
        while (std::getline(in, line)) {
            if (!line.empty())
                out << pad << line;
            out << '\n';
        }
        return out.str();
    }

    static std::string jsonScalarToString(const Json::Value& v) {
        Json::StreamWriterBuilder w;
        w["indentation"] = "";
        return Json::writeString(w, v);
    }

    static std::string prettyJsonValue(const Json::Value& v, int depth = 0) {
        const std::string pad(depth * 4, ' ');
        const std::string childPad((depth + 1) * 4, ' ');
        if (v.isObject()) {
            auto keys = v.getMemberNames();
            if (keys.empty())
                return "{}";
            std::ostringstream out;
            out << "{\n";
            for (size_t i = 0; i < keys.size(); ++i) {
                out << childPad << jsonScalarToString(Json::Value(keys[i])) << ": "
                    << prettyJsonValue(v[keys[i]], depth + 1);
                if (i + 1 < keys.size())
                    out << ",";
                out << "\n";
            }
            out << pad << "}";
            return out.str();
        }
        if (v.isArray()) {
            if (v.empty())
                return "[]";
            std::ostringstream out;
            out << "[\n";
            for (Json::ArrayIndex i = 0; i < v.size(); ++i) {
                out << childPad << prettyJsonValue(v[i], depth + 1);
                if (i + 1 < v.size())
                    out << ",";
                out << "\n";
            }
            out << pad << "]";
            return out.str();
        }
        return jsonScalarToString(v);
    }

    // Indent JSON for readability — 4 spaces, standard JSON shape.
    static std::string prettyJson(const std::string& raw) {
        Json::Value parsed;
        Json::CharReaderBuilder reader;
        std::string errs;
        std::istringstream ss(raw);
        if (Json::parseFromStream(reader, ss, &parsed, &errs)) {
            return prettyJsonValue(parsed) + "\n";
        }
        return raw + "\n";
    }

    // Public wrapper for global manifest autoloaders.
    static ToolSchema loadToolManifest(const std::string& toolYmlPath) {
        return loadToolSchema(toolYmlPath);
    }

    struct RelicConfig {
        std::string baseUrl;
    };
    static RelicConfig loadRelicConfig(const std::string& path) {
        RelicConfig rc;
        auto yaml = readFile(path);
        if (yaml.empty())
            return rc;
        auto root = ManifestYaml::parse(yaml);
        auto* iface = ManifestYaml::find(root, "interface");
        if (iface)
            rc.baseUrl = ManifestYaml::get(*iface, "base_url");
        return rc;
    }

   private:
    static std::string readFile(const std::string& path) {
        std::ifstream f(path);
        if (!f)
            return "";
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    static ToolSchema loadToolSchema(const std::string& toolYmlPath) {
        ToolSchema s;
        auto yaml = readFile(toolYmlPath);
        if (yaml.empty())
            return s;

        auto root = ManifestYaml::parse(yaml);
        s.name = ManifestYaml::get(root, "name");
        s.description = ManifestYaml::get(root, "description");

        // Parse input/output schemas as JSON strings
        auto* inputNode = ManifestYaml::find(root, "input_schema");
        if (inputNode)
            s.inputSchema = nodeToJson(*inputNode);

        auto* outputNode = ManifestYaml::find(root, "output_schema");
        if (outputNode)
            s.outputSchema = nodeToJson(*outputNode);

        auto* impl = ManifestYaml::find(root, "implementation");
        if (impl) {
            s.runtime = ManifestYaml::get(*impl, "runtime", s.runtime);
            s.entrypoint = ManifestYaml::get(*impl, "entrypoint", s.entrypoint);
            s.inputType = ManifestYaml::get(*impl, "input_type", s.inputType);
            s.textParam = ManifestYaml::get(*impl, "text_param", s.textParam);
        }
        // Fallback: some tool manifests use top-level runtime/entrypoint
        if (s.runtime.empty())
            s.runtime = ManifestYaml::get(root, "runtime");
        if (s.entrypoint.empty())
            s.entrypoint = ManifestYaml::get(root, "entrypoint");
        s.inputType =
            ManifestYaml::get(root, "input_type", s.inputType.empty() ? "json" : s.inputType);
        s.textParam = ManifestYaml::get(root, "text_param", s.textParam);

        // Examples
        auto* examplesNode = ManifestYaml::find(root, "examples");
        if (examplesNode) {
            Json::Value examples(Json::arrayValue);
            for (auto& ex : examplesNode->children) {
                Json::Value entry;
                entry["description"] = ManifestYaml::get(ex, "description");
                auto* params = ManifestYaml::find(ex, "params");
                if (params)
                    entry["params"] = nodeToJsonValue(*params);
                examples.append(entry);
            }
            Json::StreamWriterBuilder w;
            w["indentation"] = "";
            s.examples = Json::writeString(w, examples);
        }

        return s;
    }

    static ToolSchema loadBuiltinToolSchema(const std::string& name) {
        // Look for manifest in manifests/built-in/tools/<name>/tool.yml
        std::vector<std::string> searchPaths = {
            "manifests/built-in/tools/" + name + "/tool.yml",
            "manifests/tools/" + name + "/tool.yml",
            "config/agents/*/tools/" + name + "/tool.yml",
        };
        for (auto& p : searchPaths) {
            if (fs::exists(p))
                return loadToolSchema(p);
        }
        return {};
    }

    static std::string nodeToJson(const ManifestYaml::Node& node) {
        Json::Value v = nodeToJsonValue(node);
        Json::StreamWriterBuilder w;
        w["indentation"] = "";
        return Json::writeString(w, v);
    }

    static Json::Value nodeToJsonValue(const ManifestYaml::Node& node) {
        if (node.children.empty()) {
            // Leaf value
            std::string v = node.value.empty() ? node.key : node.value;
            // Try as bool
            if (v == "true")
                return true;
            if (v == "false")
                return false;
            // Try as number
            try {
                return std::stoi(v);
            } catch (...) {
            }
            try {
                return std::stod(v);
            } catch (...) {
            }
            return v;
        }

        // Check if it's an object or array
        bool isObj = false;
        for (auto& c : node.children) {
            if (!c.key.empty()) {
                isObj = true;
                break;
            }
        }

        if (isObj) {
            Json::Value obj(Json::objectValue);
            for (auto& c : node.children) {
                if (!c.key.empty()) {
                    obj[c.key] = nodeToJsonValue(c);
                }
            }
            return obj;
        } else {
            Json::Value arr(Json::arrayValue);
            for (auto& c : node.children) {
                arr.append(nodeToJsonValue(c));
            }
            return arr;
        }
    }
};

}  // namespace mk3
}  // namespace cortex
