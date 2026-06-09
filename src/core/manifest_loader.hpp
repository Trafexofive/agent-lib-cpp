// ─────────────────────────────────────────────────────────────────────────────
// Manifest Loader — parses agent.yml, loads tools/agents/relics, populates config
// Supports: sandbox mode, file imports, schema injection, config.yml overrides
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "../core/types.hpp"
#include "../core/agent.hpp"
#include "../feeds/feed_engine.hpp"
#include "../workflows/workflow_engine.hpp"
#include "../providers/factory.hpp"
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <json/json.h>
#include <iostream>

namespace cortex {
namespace mk3 {

namespace fs = std::filesystem;

// ── Mini YAML parser (supports our manifest subset) ──
class ManifestYaml {
public:
    struct Node {
        std::string key;
        std::string value;
        std::vector<Node> children;
        bool isList = false;
    };

    static Node parse(const std::string& yaml) {
        std::vector<std::string> lines;
        std::istringstream ss(yaml);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.empty() || line[0] == '#') continue;
            lines.push_back(line);
        }
        Node root;
        root.key = "root";
        parseLines(root, lines, 0, 0);
        return root;
    }

    static std::string get(const Node& parent, const std::string& key, const std::string& def = "") {
        for (auto& c : parent.children) {
            if (c.key == key) return c.value.empty() ? "" : c.value;
        }
        return def;
    }

    static const Node* find(const Node& parent, const std::string& key) {
        for (auto& c : parent.children) {
            if (c.key == key) return &c;
        }
        return nullptr;
    }

    static std::vector<std::string> getList(const Node& parent, const std::string& key) {
        std::vector<std::string> result;
        auto* node = find(parent, key);
        if (!node) return result;
        // Check direct children first
        for (auto& c : node->children) {
            if (!c.value.empty()) result.push_back(c.value);
        }
        // If no children, look for sibling list items after this key
        if (result.empty()) {
            bool found = false;
            for (auto& c : parent.children) {
                if (&c == node) { found = true; continue; }
                if (found && c.key.empty() && !c.value.empty()) {
                    result.push_back(c.value);
                } else if (found && !c.key.empty()) {
                    break; // Next key — stop collecting
                }
            }
        }
        return result;
    }

private:
    static size_t parseLines(Node& parent, const std::vector<std::string>& lines,
                             size_t idx, int indent) {
        while (idx < lines.size()) {
            const auto& line = lines[idx];
            int lineIndent = 0;
            while (lineIndent < (int)line.size() && line[lineIndent] == ' ') lineIndent++;
            if (lineIndent < indent) return idx;

            std::string trimmed = line.substr(lineIndent);
            if (trimmed.rfind("- ", 0) == 0) {
                Node item;
                item.isList = true;
                item.value = trimQuotes(trimmed.substr(2));
                size_t colon = item.value.find(": ");
                if (colon != std::string::npos) {
                    item.key = item.value.substr(0, colon);
                    item.value = item.value.substr(colon + 2);
                }
                idx = parseLines(item, lines, idx + 1, lineIndent + 2);
                parent.children.push_back(item);
            } else if (trimmed.find(": ") != std::string::npos || trimmed.back() == ':') {
                Node kv;
                size_t colon = trimmed.find(": ");
                if (colon != std::string::npos) {
                    kv.key = trimmed.substr(0, colon);
                    kv.value = trimQuotes(trimmed.substr(colon + 2));
                } else {
                    kv.key = trimmed.substr(0, trimmed.size() - 1);
                }
                if (idx + 1 < lines.size()) {
                    int nextIndent = 0;
                    while (nextIndent < (int)lines[idx+1].size() && lines[idx+1][nextIndent] == ' ') nextIndent++;
                    if (nextIndent > lineIndent) {
                        idx = parseLines(kv, lines, idx + 1, nextIndent);
                        parent.children.push_back(kv);
                        continue;
                    }
                }
                parent.children.push_back(kv);
                idx++;
            } else {
                idx++;
            }
        }
        return idx;
    }

    static std::string trimQuotes(const std::string& s) {
        if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                               (s.front() == '\'' && s.back() == '\'')))
            return s.substr(1, s.size() - 2);
        return s;
    }
};

// ── Tool schema (from tool.yml) ──
struct ToolSchema {
    std::string name;
    std::string description;
    std::string inputSchema;   // JSON string
    std::string outputSchema;  // JSON string
    std::string examples;      // JSON string
    std::string runtime;       // python3, builtin, etc.
    std::string entrypoint;    // script path
};

// ── Manifest loader ──
class ManifestLoader {
public:
    // Load an agent manifest from path, populate config
    static AgentConfig loadAgentConfig(const std::string& manifestPath) {
        AgentConfig cfg;
        auto yaml = readFile(manifestPath);
        if (yaml.empty()) return cfg;

        auto root = ManifestYaml::parse(yaml);
        auto* kind = ManifestYaml::find(root, "kind");
        if (!kind || kind->value != "Agent") return cfg;

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
                    if (!temp.empty()) cfg.temperature = std::stod(temp);
                    std::string mt = ManifestYaml::get(*params, "max_tokens");
                    if (!mt.empty()) cfg.maxTokens = std::stoi(mt);
                    std::string tp = ManifestYaml::get(*params, "top_p");
                    if (!tp.empty()) cfg.topP = std::stod(tp);
                    std::string tk = ManifestYaml::get(*params, "top_k");
                    if (!tk.empty()) cfg.topK = std::stoi(tk);
                    std::string pp = ManifestYaml::get(*params, "presence_penalty");
                    if (!pp.empty()) cfg.presencePenalty = std::stod(pp);
                    std::string fp = ManifestYaml::get(*params, "frequency_penalty");
                    if (!fp.empty()) cfg.frequencyPenalty = std::stod(fp);
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
            if (!ic.empty()) cfg.iterationCap = std::stoi(ic);
            std::string hc = ManifestYaml::get(*runtime, "history_cap");
            if (!hc.empty()) cfg.historyCap = std::stoi(hc);
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
        if (yaml.empty()) return;
        auto root = ManifestYaml::parse(yaml);
        auto* importNode = ManifestYaml::find(root, "import");
        if (!importNode) return;
        auto feedNames = ManifestYaml::getList(*importNode, "feeds");
        for (auto& name : feedNames) {
            if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
                name = name.substr(1, name.size() - 2);
            // Path-based feed (contains / or .yml) — load from manifest
            if (name.find(".yml") != std::string::npos || name.find('/') != std::string::npos) {
                fs::path feedPath = fs::path(manifestPath).parent_path() / name;
                auto mr = feeds::FeedEngine::instance().loadFeedManifest(feedPath.string());
                if (mr.success && !mr.name.empty()) agent.addFeed(mr.name);
            } else {
                agent.addFeed(name);
            }
        }
    }

    static void loadRelics(const std::string& manifestPath, Agent& agent) {
        auto yaml = readFile(manifestPath);
        if (yaml.empty()) return;
        auto root = ManifestYaml::parse(yaml);
        auto* importNode = ManifestYaml::find(root, "import");
        if (!importNode) return;
        auto relicNames = ManifestYaml::getList(*importNode, "relics");
        for (auto& name : relicNames) {
            if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
                name = name.substr(1, name.size() - 2);
            agent.addRelic(name);
        }
    }

    static void loadEnv(const std::string& manifestPath, AgentConfig& cfg) {
        auto yaml = readFile(manifestPath);
        if (yaml.empty()) return;
        auto root = ManifestYaml::parse(yaml);
        auto* importNode = ManifestYaml::find(root, "import");
        if (!importNode) return;
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
        if (yaml.empty()) return {};

        auto root = ManifestYaml::parse(yaml);
        auto* importNode = ManifestYaml::find(root, "import");
        if (!importNode) return {};

        auto toolNames = ManifestYaml::getList(*importNode, "tools");
        std::vector<ToolSchema> schemas;

        for (auto& name : toolNames) {
            // Trim quotes if present
            if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
                name = name.substr(1, name.size() - 2);

            // Check if it's a path (ends with .yml or contains /)
            if (name.find(".yml") != std::string::npos || name.find('/') != std::string::npos) {
                fs::path toolPath = fs::path(manifestPath).parent_path() / name;
                auto schema = loadToolSchema(toolPath.string());
                if (!schema.name.empty()) {
                    schemas.push_back(schema);
                    // Wire up script execution: set isNative=false, populate runtime + entrypoint
                    ToolDef td;
                    td.name = schema.name;
                    td.description = schema.description;
                    if (!schema.runtime.empty() && !schema.entrypoint.empty()) {
                        td.isNative = false;
                        td.scriptRuntime = schema.runtime;
                        td.scriptPath = (toolPath.parent_path() / schema.entrypoint).string();
                    }
                    agent.addTool(td);
                }
            } else {
                // Built-in tool — load schema AND register with agent
                auto schema = loadBuiltinToolSchema(name);
                if (!schema.name.empty()) {
                    schemas.push_back(schema);
                    agent.addTool({schema.name, schema.description});
                } else {
                    // No schema file found — still add as bare tool (name only)
                    agent.addTool({name, ""});
                }
            }
        }
        return schemas;
    }

    // Load sub-agents from import list
    static void loadSubAgents(const std::string& manifestPath, Agent& agent,
                               const std::string& providerName) {
        auto yaml = readFile(manifestPath);
        if (yaml.empty()) return;

        auto root = ManifestYaml::parse(yaml);
        auto* importNode = ManifestYaml::find(root, "import");
        if (!importNode) return;

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
            subCfg.provider = providerName; // inherit parent's provider


            auto provider = providers::createProvider(subCfg.provider, subCfg.model);
            if (!provider) {
                continue;
            }

            auto subAgent = std::make_shared<Agent>(subCfg, provider);
            agent.addSubAgent(subAgent);
        }
    }

    // Load workflows from import section
    static std::string loadWorkflows(const std::string& manifestPath) {
        auto yaml = readFile(manifestPath);
        if (yaml.empty()) return "";

        auto root = ManifestYaml::parse(yaml);
        auto* importNode = ManifestYaml::find(root, "import");
        if (!importNode) return "";

        auto wfList = ManifestYaml::getList(*importNode, "workflows");
        if (wfList.empty()) return "";

        std::ostringstream ss;
        for (auto& wfName : wfList) {
            fs::path wfPath = fs::path(manifestPath).parent_path() / wfName;
            if (!fs::exists(wfPath)) {
                // Try manifests/workflows/
                wfPath = fs::path("manifests/workflows") / (wfName + ".yml");
                if (!fs::exists(wfPath)) continue;
            }
            auto wf = workflows::WorkflowEngine::instance().load(wfPath.string());
            if (!wf.name.empty()) {
                ss << workflows::WorkflowEngine::instance().toXml(wf);
            }
        }
        return ss.str();
    }

    // Load files from import section
    static std::vector<std::string> loadFiles(const std::string& manifestPath) {
        auto yaml = readFile(manifestPath);
        if (yaml.empty()) return {};

        auto root = ManifestYaml::parse(yaml);
        auto* importNode = ManifestYaml::find(root, "import");
        if (!importNode) return {};

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
        if (!fs::exists(configPath)) return;

        auto yaml = readFile(configPath.string());
        if (yaml.empty()) return;

        auto root = ManifestYaml::parse(yaml);
        auto* runtime = ManifestYaml::find(root, "runtime");
        if (runtime) {
            std::string ic = ManifestYaml::get(*runtime, "max_iterations");
            if (!ic.empty()) cfg.iterationCap = std::stoi(ic);
            std::string hc = ManifestYaml::get(*runtime, "history_cap");
            if (!hc.empty()) cfg.historyCap = std::stoi(hc);
        }

        auto* agentNode = ManifestYaml::find(root, "agent");
        if (agentNode) {
            std::string temp = ManifestYaml::get(*agentNode, "temperature");
            if (!temp.empty()) cfg.temperature = std::stod(temp);
            std::string mdl = ManifestYaml::get(*agentNode, "model");
            if (!mdl.empty()) cfg.model = mdl;
            std::string prov = ManifestYaml::get(*agentNode, "provider");
            if (!prov.empty()) cfg.provider = prov;
        }
    }

    // Build tool schemas XML for prompt injection
    static std::string toolSchemasToXml(const std::vector<ToolSchema>& schemas) {
        if (schemas.empty()) return "";
        std::ostringstream ss;
        for (auto& s : schemas) {
            ss << "    <tool name=\"" << s.name << "\"";
            if (!s.description.empty()) ss << " desc=\"" << s.description << "\"";
            ss << ">\n";
            // Format JSON with indentation for LLM readability
            if (!s.inputSchema.empty())
                ss << "      <params>\n" << prettyJson(s.inputSchema) << "      </params>\n";
            if (!s.outputSchema.empty())
                ss << "      <returns>\n" << prettyJson(s.outputSchema) << "      </returns>\n";
            ss << "    </tool>\n";
        }
        return ss.str();
    }

    // Indent JSON for readability — LLMs read structured text better than single-line blobs
    static std::string prettyJson(const std::string& raw) {
        std::ostringstream out;
        int depth = 0;
        bool inString = false;
        for (size_t i = 0; i < raw.size(); i++) {
            char c = raw[i];
            if (c == '"' && (i == 0 || raw[i-1] != '\\')) inString = !inString;
            if (inString) { out << c; continue; }
            if (c == '{' || c == '[') {
                out << c << '\n';
                depth++;
                out << std::string(depth * 2, ' ') << "        ";
            } else if (c == '}' || c == ']') {
                out << '\n';
                depth--;
                out << std::string(depth * 2, ' ') << "        " << c;
            } else if (c == ',') {
                out << ",\n" << std::string(depth * 2, ' ') << "        ";
            } else if (c == ':') {
                out << ": ";
            } else if (c != ' ' && c != '\n' && c != '\t') {
                out << c;
            }
        }
        out << '\n';
        return out.str();
    }

    // Public wrapper for global manifest autoloaders.
    static ToolSchema loadToolManifest(const std::string& toolYmlPath) {
        return loadToolSchema(toolYmlPath);
    }

private:
    static std::string readFile(const std::string& path) {
        std::ifstream f(path);
        if (!f) return "";
        return std::string((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    }

    static ToolSchema loadToolSchema(const std::string& toolYmlPath) {
        ToolSchema s;
        auto yaml = readFile(toolYmlPath);
        if (yaml.empty()) return s;

        auto root = ManifestYaml::parse(yaml);
        s.name = ManifestYaml::get(root, "name");
        s.description = ManifestYaml::get(root, "description");

        // Parse input/output schemas as JSON strings
        auto* inputNode = ManifestYaml::find(root, "input_schema");
        if (inputNode) s.inputSchema = nodeToJson(*inputNode);

        auto* outputNode = ManifestYaml::find(root, "output_schema");
        if (outputNode) s.outputSchema = nodeToJson(*outputNode);

        auto* impl = ManifestYaml::find(root, "implementation");
        if (impl) {
            s.runtime = ManifestYaml::get(*impl, "runtime", s.runtime);
            s.entrypoint = ManifestYaml::get(*impl, "entrypoint", s.entrypoint);
        }
        // Fallback: some tool manifests use top-level runtime/entrypoint
        if (s.runtime.empty()) s.runtime = ManifestYaml::get(root, "runtime");
        if (s.entrypoint.empty()) s.entrypoint = ManifestYaml::get(root, "entrypoint");

        // Examples
        auto* examplesNode = ManifestYaml::find(root, "examples");
        if (examplesNode) {
            Json::Value examples(Json::arrayValue);
            for (auto& ex : examplesNode->children) {
                Json::Value entry;
                entry["description"] = ManifestYaml::get(ex, "description");
                auto* params = ManifestYaml::find(ex, "params");
                if (params) entry["params"] = nodeToJsonValue(*params);
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
            if (fs::exists(p)) return loadToolSchema(p);
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
            if (v == "true") return true;
            if (v == "false") return false;
            // Try as number
            try { return std::stoi(v); } catch (...) {}
            try { return std::stod(v); } catch (...) {}
            return v;
        }

        // Check if it's an object or array
        bool isObj = false;
        for (auto& c : node.children) {
            if (!c.key.empty()) { isObj = true; break; }
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

} // namespace mk3
} // namespace cortex
