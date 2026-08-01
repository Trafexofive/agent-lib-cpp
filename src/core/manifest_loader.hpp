// ─────────────────────────────────────────────────────────────────────────────
// Manifest Loader — parses agent.yml, loads tools/agents/relics, populates config
// Supports: sandbox mode, file imports, schema injection
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../core/agent.hpp"
#include "../core/types.hpp"
#include "../feeds/feed_engine.hpp"
#include "../providers/factory.hpp"
#include "../relics/docker_dispatcher.hpp"
#include "../relics/reliquary.hpp"
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
    std::string runtime;             // python3, builtin, process, etc.
    std::string entrypoint;          // script/binary path
    std::string buildCommand;        // optional build command
    std::string buildCwd;            // optional build cwd
    std::string buildOutput;         // optional build artifact
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
            // thinking: true -> require the LLM to emit <thought> before any
            // <action>. Injected into the system prompt at agent-load time.
            std::string think = ManifestYaml::get(*engine, "thinking");
            if (think == "true" || think == "1" || think == "yes")
                cfg.requireThought = true;
        }

        // Context block — prompt paths + runtime config
        // Each prompt path defaults to a global default when omitted.
        // Resolve relative paths against the manifest directory (not process CWD).
        auto* context = ManifestYaml::find(root, "context");
        fs::path base = fs::absolute(fs::path(manifestPath).parent_path());

        // Harness prompt (protocol spec)
        std::string harnessRel = context ? ManifestYaml::get(*context, "harness") : "";
        cfg.harnessPath =
            harnessRel.empty() ? "manifests/harness/default.md" : (base / harnessRel).string();

        // System prompt (capabilities/tools/behavior)
        std::string systemRel = context ? ManifestYaml::get(*context, "system") : "";
        cfg.systemPromptPath =
            systemRel.empty() ? "manifests/system/default.md" : (base / systemRel).string();

        // Persona prompt (identity/values)
        std::string personaRel = context ? ManifestYaml::get(*context, "persona") : "";
        cfg.personaPath =
            personaRel.empty() ? "manifests/persona/default.md" : (base / personaRel).string();

        // Runtime config knobs (max_iterations, history_cap, action_timeout_sec)
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

        // Prompt-building knobs
        auto* promptBuilding = ManifestYaml::find(root, "prompt_building");
        if (promptBuilding) {
            auto* rc = ManifestYaml::find(*promptBuilding, "runtime_capabilities");
            if (!rc)
                rc = ManifestYaml::find(*promptBuilding, "available_actions");
            if (rc) {
                std::string inputSchemas = ManifestYaml::get(*rc, "input_schemas", "enable");
                std::string returnSchemas = ManifestYaml::get(*rc, "return_schemas", "enable");
                std::string examples = ManifestYaml::get(*rc, "usage_examples", "enable");

                // Backward-compatible aliases for the old names.
                if (ManifestYaml::find(*rc, "output_schema"))
                    returnSchemas = ManifestYaml::get(*rc, "output_schema", returnSchemas);
                if (ManifestYaml::find(*rc, "examples"))
                    examples = ManifestYaml::get(*rc, "examples", examples);

                cfg.promptBuilding.runtimeCapabilities.inputSchemas =
                    promptFlagEnabled(inputSchemas);
                cfg.promptBuilding.runtimeCapabilities.returnSchemas =
                    promptFlagEnabled(returnSchemas);
                cfg.promptBuilding.runtimeCapabilities.usageExamples = promptFlagEnabled(examples);
            }
        }

        // Legacy fallback: persona.agent (old convention, no context: block)
        if (!context) {
            auto* persona = ManifestYaml::find(root, "persona");
            if (persona) {
                std::string agentPath = ManifestYaml::get(*persona, "agent");
                if (!agentPath.empty())
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
            // Orthogonal lifecycle defaults (CLI flags OR with these):
            //   no_session → don't load/save session records
            //   ephemeral  → exit when the agent turn finishes (app layer)
            auto truthy = [](const std::string& v) {
                return v == "true" || v == "1" || v == "yes";
            };
            std::string ns = ManifestYaml::get(*runtime, "no_session");
            if (truthy(ns))
                cfg.defaultNoSession = true;
            std::string ep = ManifestYaml::get(*runtime, "ephemeral");
            if (truthy(ep))
                cfg.defaultEphemeral = true;
            // DEV_MODE: lazy live-test dumps (iterations as LLM saw them + raw + history)
            std::string dm = ManifestYaml::get(*runtime, "dev_mode");
            if (dm.empty())
                dm = ManifestYaml::get(*runtime, "DEV_MODE");
            if (truthy(dm))
                cfg.devMode = true;
            // normal | autonomous — bare/non-final completion handling
            std::string mode = ManifestYaml::get(*runtime, "mode");
            if (mode.empty())
                mode = ManifestYaml::get(*runtime, "runtime_mode");
            if (!mode.empty()) {
                // normalize
                for (char& c : mode) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (mode == "normal" || mode == "autonomous" || mode == "auto") {
                    if (mode == "auto") mode = "autonomous";
                    cfg.runtimeMode = mode;
                }
            }
            std::string cp = ManifestYaml::get(*runtime, "completion_policy");
            if (cp.empty())
                cp = ManifestYaml::get(*runtime, "on_bare_output");
            if (!cp.empty()) {
                for (char& c : cp) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (cp == "recover" || cp == "promote" || cp == "strict")
                    cfg.completionPolicy = cp;
            }
            std::string br = ManifestYaml::get(*runtime, "bare_promote_after");
            if (!br.empty())
                cfg.bareRecoveryPromoteAfter = std::stoi(br);
            auto* subagents = ManifestYaml::find(*runtime, "subagents");
            if (subagents) {
                std::string persistence = ManifestYaml::get(*subagents, "persistence");
                if (!persistence.empty())
                    cfg.subAgentPersistence = persistence;
            }
        }

        // Sandbox
        auto* sandbox = ManifestYaml::find(root, "sandbox");
        if (sandbox) {
            cfg.sandboxMode = ManifestYaml::get(*sandbox, "mode", "process");
            cfg.sandboxRuntime = ManifestYaml::get(*sandbox, "runtime", "");
            cfg.sandboxImage = ManifestYaml::get(*sandbox, "image", "");
        }

        // Retry / resilience — exponential backoff for transient upstream
        // failures. Optional block; defaults from AgentConfig apply when
        // omitted. Useful for OpenRouter/free models that intermittently
        // emit empty content.
        auto* retry = ManifestYaml::find(root, "retry");
        if (retry) {
            std::string maxRetries = ManifestYaml::get(*retry, "empty_response_max_retries");
            if (!maxRetries.empty())
                cfg.emptyResponseMaxRetries = std::stoi(maxRetries);
            std::string initial = ManifestYaml::get(*retry, "empty_response_initial_backoff_ms");
            if (!initial.empty())
                cfg.emptyResponseInitialBackoffMs = std::stoi(initial);
            std::string maxBack = ManifestYaml::get(*retry, "empty_response_max_backoff_ms");
            if (!maxBack.empty())
                cfg.emptyResponseMaxBackoffMs = std::stoi(maxBack);
            std::string mult = ManifestYaml::get(*retry, "empty_response_backoff_multiplier");
            if (!mult.empty())
                cfg.emptyResponseBackoffMultiplier = std::stod(mult);
            std::string lenFlag = ManifestYaml::get(*retry, "retry_on_finish_reason_length");
            if (!lenFlag.empty())
                cfg.retryOnFinishReasonLength = promptFlagEnabled(lenFlag);
            std::string filterFlag =
                ManifestYaml::get(*retry, "retry_on_finish_reason_content_filter");
            if (!filterFlag.empty())
                cfg.retryOnFinishReasonContentFilter = promptFlagEnabled(filterFlag);
            auto* reasons = ManifestYaml::find(*retry, "retry_on_finish_reasons");
            if (reasons) {
                cfg.retryOnFinishReasons.clear();
                for (auto& c : reasons->children)
                    if (!c.value.empty())
                        cfg.retryOnFinishReasons.push_back(c.value);
            }
        }

        // Manifest directory for resolving relative paths (absolute)
        cfg.manifestDir = base.string();

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
                // Register the relic into the unified Reliquary (a
                // Relic subclass that wraps Docker dispatch). The legacy
                // DockerRelicDispatcher is left untouched for any direct
                // callers; new dispatch paths should go through Reliquary.
                relics::Reliquary::instance().loadDockerRelicsFrom(relicPath.string());
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
                        td.scriptPath = (toolPath.parent_path() / schema.entrypoint).lexically_normal().string();
                        td.buildCommand = schema.buildCommand;
                        td.buildCwd = schema.buildCwd.empty()
                                          ? toolPath.parent_path().string()
                                          : (toolPath.parent_path() / schema.buildCwd).lexically_normal().string();
                        td.buildOutput = schema.buildOutput.empty()
                                             ? ""
                                             : (toolPath.parent_path() / schema.buildOutput).lexically_normal().string();
                        agent.addTool(tools::Tool(td, td.scriptPath, td.scriptRuntime));
                    } else {
                        agent.addTool(tools::Tool(td));
                    }
                }
            } else {
                // Bare name (incl. legacy "builtin/exec" prefix) → built-in tool.
                std::string bareName = stripBuiltinPrefix(name);
                // Ensure backend registry is populated before grant. Agent ctor
                // already calls this, but loadTools can run in other paths.
                tools::registerDefaults();
                auto schema = loadBuiltinToolSchema(bareName);
                if (!schema.name.empty()) {
                    schemas.push_back(schema);
                    // Prefer the executable Tool from the registry so dispatch
                    // has a real callback. Schema-only grants (no cb) are a
                    // last resort — Agent::dispatchTool also falls back to
                    // tools::dispatch for those.
                    const tools::Tool* reg = tools::ToolRegistry::instance().findTool(bareName);
                    if (reg) {
                        agent.addTool(*reg);
                    } else {
                        ToolDef td;
                        td.name = schema.name;
                        td.description = schema.description;
                        td.inputType = schema.inputType.empty() ? "json" : schema.inputType;
                        td.textParam = schema.textParam;
                        td.isNative = true;
                        agent.addTool(tools::Tool(td));
                    }
                } else {
                    ToolDef td;
                    td.name = bareName;
                    td.isNative = true;
                    agent.addTool(tools::Tool(td));
                }
            }
        }
        return schemas;
    }

    // Grant the standard built-in tool set to an agent and return their
    // schemas. Used by the no-manifest CLI path so bare `run` still has a
    // working tool surface instead of an empty <action_available>.
    static std::vector<ToolSchema> loadBuiltinTools(Agent& agent) {
        static const std::vector<std::string> builtin = {
            "exec", "grep", "list", "fs_read", "fs_write", "json", "web_fetch", "sleep", "artifact",
            "context_pin", "context_peek", "context_unpin", "ask_tool",
        };
        std::vector<ToolSchema> schemas;
        for (const auto& name : builtin) {
            auto schema = loadBuiltinToolSchema(name);
            if (!schema.name.empty()) {
                schemas.push_back(schema);
                const tools::Tool* reg = tools::ToolRegistry::instance().findTool(name);
                if (reg) {
                    agent.addTool(*reg);
                } else {
                    ToolDef td;
                    td.name = schema.name;
                    td.description = schema.description;
                    td.inputType = schema.inputType.empty() ? "json" : schema.inputType;
                    td.textParam = schema.textParam;
                    agent.addTool(tools::Tool(td));
                }
            } else {
                // Schema missing — still grant so dispatch can run the
                // backend-registered implementation; <action_available> will
                // mark params unavailable.
                ToolDef td;
                td.name = name;
                agent.addTool(tools::Tool(td));
            }
        }
        return schemas;
    }

    static fs::path resolveSubAgentManifest(const fs::path& parentManifest,
                                            const std::string& name) {
        fs::path base = parentManifest.parent_path();
        fs::path requested(name);
        std::vector<fs::path> candidates;

        if (requested.is_absolute()) {
            candidates.push_back(requested);
        } else if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
                   requested.extension() == ".yml") {
            candidates.push_back(base / requested);
        } else {
            candidates.push_back(base / "agents" / name / "agent.yml");
            candidates.push_back(base.parent_path() / name / "agent.yml");
            candidates.push_back(fs::path("config/agents") / name / "agent.yml");
            candidates.push_back(fs::path("manifests/agents") / name / "agent.yml");
        }

        for (const auto& candidate : candidates) {
            std::error_code ec;
            fs::path normalized = candidate.lexically_normal();
            if (fs::exists(normalized, ec) && fs::is_regular_file(normalized, ec))
                return normalized;
        }
        return {};
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
            fs::path agentManifest = resolveSubAgentManifest(fs::path(manifestPath), name);
            if (agentManifest.empty())
                continue;

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

        // Compact workflow cards by default (name + summary + step spine).
        // Full step XML drowned parent prompts and duplicated tool guidance.
        // Set CORTEX_WORKFLOW_FULL=1 to restore legacy full toXml dumps.
        const bool fullXml = []() {
            const char* e = std::getenv("CORTEX_WORKFLOW_FULL");
            return e && e[0] && std::string(e) != "0" && std::string(e) != "false";
        }();
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
            if (!wf.isValid())
                continue;
            const auto& m = wf.manifest();
            if (fullXml) {
                ss << workflows::WorkflowEngine::instance().toXml(m);
                continue;
            }
            ss << "<workflow name=\"" << m.name << "\" version=\"" << m.version << "\">\n";
            if (!m.summary.empty())
                ss << "  <summary>" << m.summary << "</summary>\n";
            ss << "  <step_count>" << m.steps.size() << "</step_count>\n";
            if (!m.steps.empty()) {
                ss << "  <spine>";
                for (size_t i = 0; i < m.steps.size(); ++i) {
                    if (i)
                        ss << " → ";
                    const auto& st = m.steps[i];
                    ss << st.id;
                    if (!st.type.empty())
                        ss << "(" << st.type;
                    if (!st.tool.empty())
                        ss << ":" << st.tool;
                    else if (!st.agent.empty())
                        ss << ":" << st.agent;
                    if (!st.type.empty())
                        ss << ")";
                }
                ss << "</spine>\n";
            }
            ss << "</workflow>\n";
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

    // Build tool schemas XML for prompt injection
    static std::string toolSchemasToXml(const std::vector<ToolSchema>& schemas, int baseIndent = 8,
                                        bool includeInputSchemas = true,
                                        bool includeReturnSchemas = true,
                                        bool includeExamples = true) {
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
            if (includeInputSchemas && !s.inputSchema.empty())
                ss << fieldPad << "<params>\n"
                   << indentBlock(prettyJson(s.inputSchema), baseIndent + 8) << fieldPad
                   << "</params>\n";
            if (includeReturnSchemas && !s.outputSchema.empty())
                ss << fieldPad << "<returns>\n"
                   << indentBlock(prettyJson(s.outputSchema), baseIndent + 8) << fieldPad
                   << "</returns>\n";
            if (includeExamples && !s.examples.empty())
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

    // Cheap kind sniff — reads the first "kind:" line of a manifest YAML
    // without parsing the whole document. Returns lowercased kind value
    // ("agent", "tool", "relic", "workflow", "feed", "harness", "prompt",
    // "skill") or "" when the file is missing or has no kind: field.
    static std::string detectKind(const std::string& manifestPath) {
        std::ifstream f(manifestPath);
        if (!f) return "";
        std::string line;
        while (std::getline(f, line)) {
            // Skip leading whitespace / comments.
            size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '	')) i++;
            if (i >= line.size() || line[i] == '#') continue;
            // Look for top-level "kind:" key.
            if (line.compare(i, 5, "kind:") == 0) {
                size_t v = i + 5;
                while (v < line.size() && (line[v] == ' ' || line[v] == '	' || line[v] == '"'))
                    v++;
                size_t end = v;
                while (end < line.size() && line[end] != '"' && line[end] != '#' &&
                       line[end] != '\n' && line[end] != '\n')
                    end++;
                std::string raw = line.substr(v, end - v);
                // Lowercase + trim.
                for (char& c : raw) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                while (!raw.empty() && (raw.back() == ' ' || raw.back() == '	' || raw.back() == '"'))
                    raw.pop_back();
                return raw;
            }
            // Stop on first non-kind, non-blank, non-comment top-level key.
            if (!line.empty() && line[0] != ' ' && line[0] != '	' && line[0] != '#') break;
        }
        return "";
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
    static bool promptFlagEnabled(const std::string& raw) {
        std::string v = raw;
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return !(v == "false" || v == "0" || v == "no" || v == "off" || v == "disable" ||
                 v == "disabled" || v == "none");
    }

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
        // PE: many tools put the skill text in description; some only have summary.
        if (s.description.empty())
            s.description = ManifestYaml::get(root, "summary");

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
            auto* build = ManifestYaml::find(*impl, "build");
            if (build) {
                s.buildCommand = ManifestYaml::get(*build, "command", s.buildCommand);
                s.buildCwd = ManifestYaml::get(*build, "cwd", s.buildCwd);
                s.buildOutput = ManifestYaml::get(*build, "output", s.buildOutput);
            }
        }
        // Fallback: some tool manifests use top-level runtime/entrypoint
        if (s.runtime.empty())
            s.runtime = ManifestYaml::get(root, "runtime");
        if (s.entrypoint.empty())
            s.entrypoint = ManifestYaml::get(root, "entrypoint");
        auto* topBuild = ManifestYaml::find(root, "build");
        if (topBuild) {
            s.buildCommand = ManifestYaml::get(*topBuild, "command", s.buildCommand);
            s.buildCwd = ManifestYaml::get(*topBuild, "cwd", s.buildCwd);
            s.buildOutput = ManifestYaml::get(*topBuild, "output", s.buildOutput);
        }
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
