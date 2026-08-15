// ─────────────────────────────────────────────────────────────────────────────
// Manifest Loader — parses agent.yml, loads tools/agents/relics, populates config
// Supports: sandbox mode, file imports, schema injection
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "../core/agent.hpp"
#include "../core/agent_catalog.hpp"
#include "../core/compaction.hpp"
#include "../core/types.hpp"
#include "../feeds/feed_engine.hpp"
#include "../providers/factory.hpp"
#include "../relics/docker_dispatcher.hpp"
#include "../relics/reliquary.hpp"
#include "../tools/registry.hpp"
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
    int timeoutSec = 0;              // wall clock for script tools; 0 = agent default
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
    // Parse a compaction:/compacting: node into CompactionConfig.
    // Used for both runtime.compaction (preferred) and top-level alias.
    static void parseCompactionBlock(const ManifestYaml::Node& compactNode,
                                     CompactionConfig& out) {
        out.configured = true;
        std::string en = ManifestYaml::get(compactNode, "enabled", "true");
        out.enabled = promptFlagEnabled(en);

        out.profile = ManifestYaml::get(compactNode, "profile");

        auto parseTokenCount = [](const std::string& ct) -> int {
            if (ct.empty()) return 0;
            // allow "60k" / "60000" / "60K"
            std::string n;
            int val = 0;
            for (char c : ct) {
                if (std::isdigit(static_cast<unsigned char>(c)))
                    n.push_back(c);
                else if (c == 'k' || c == 'K') {
                    if (!n.empty()) {
                        val = std::stoi(n) * 1000;
                        n.clear();
                    }
                }
            }
            if (!n.empty()) val = std::stoi(n);
            return val;
        };

        auto* trig = ManifestYaml::find(compactNode, "trigger");
        if (trig) {
            std::string ct = ManifestYaml::get(*trig, "context_tokens");
            if (ct.empty())
                ct = ManifestYaml::get(*trig, "context_window");
            if (!ct.empty())
                out.triggerContextTokens = parseTokenCount(ct);
            std::string cp = ManifestYaml::get(*trig, "context_pct");
            if (!cp.empty())
                out.triggerContextPct = std::stod(cp);
            std::string turns = ManifestYaml::get(*trig, "turns");
            if (!turns.empty())
                out.triggerTurns = std::stoi(turns);
            std::string mctx = ManifestYaml::get(*trig, "model_context_tokens");
            if (mctx.empty())
                mctx = ManifestYaml::get(*trig, "model_window");
            if (!mctx.empty())
                out.modelContextTokens = parseTokenCount(mctx);
        }

        auto* cool = ManifestYaml::find(compactNode, "cooldown");
        if (cool) {
            std::string mt = ManifestYaml::get(*cool, "min_turns");
            if (!mt.empty())
                out.cooldownMinTurns = std::stoi(mt);
            std::string ms = ManifestYaml::get(*cool, "min_seconds");
            if (!ms.empty())
                out.cooldownMinSeconds = std::stoi(ms);
        }

        auto parseTagPol = [](const ManifestYaml::Node& node) {
            CompactionTagPolicy p;
            std::string keep = ManifestYaml::get(node, "keep");
            if (!keep.empty())
                p.keep = keep;
            std::string kl = ManifestYaml::get(node, "keep_last");
            if (kl.empty())
                kl = ManifestYaml::get(node, "n");
            if (!kl.empty())
                p.keepLast = std::stoi(kl);
            std::string tc = ManifestYaml::get(node, "truncate_chars");
            if (tc.empty())
                tc = ManifestYaml::get(node, "truncate_body_chars");
            if (!tc.empty())
                p.truncateChars = std::stoi(tc);
            std::string oe = ManifestYaml::get(node, "on_error");
            if (oe == "truncate")
                p.onErrorKeepFull = false;
            else if (oe == "keep")
                p.onErrorKeepFull = true;
            return p;
        };

        auto* policy = ManifestYaml::find(compactNode, "policy");
        if (policy) {
            auto* def = ManifestYaml::find(*policy, "default");
            if (def)
                out.defaultPolicy = parseTagPol(*def);
            auto* tags = ManifestYaml::find(*policy, "tags");
            if (tags) {
                for (const auto& t : tags->children) {
                    if (t.key.empty())
                        continue;
                    if (!t.children.empty() || !t.value.empty()) {
                        CompactionTagPolicy p = out.defaultPolicy;
                        if (!t.children.empty())
                            p = parseTagPol(t);
                        else if (!t.value.empty())
                            p.keep = t.value;
                        out.tags[t.key] = p;
                    }
                }
            }
            auto nd = ManifestYaml::getList(*policy, "never_drop");
            if (!nd.empty())
                out.neverDrop = nd;
        }

        auto* overrides = ManifestYaml::find(compactNode, "overrides");
        if (!out.profile.empty()) {
            CompactionConfig base = out;
            compaction::applyProfile(out);
            if (base.triggerContextTokens > 0)
                out.triggerContextTokens = base.triggerContextTokens;
            if (base.triggerContextPct > 0)
                out.triggerContextPct = base.triggerContextPct;
            if (base.triggerTurns > 0)
                out.triggerTurns = base.triggerTurns;
            if (base.modelContextTokens > 0)
                out.modelContextTokens = base.modelContextTokens;
            if (base.cooldownMinTurns > 0)
                out.cooldownMinTurns = base.cooldownMinTurns;
            if (base.cooldownMinSeconds > 0)
                out.cooldownMinSeconds = base.cooldownMinSeconds;
            if (!base.tags.empty()) {
                for (const auto& kv : base.tags)
                    out.tags[kv.first] = kv.second;
            }
            if (!base.neverDrop.empty())
                out.neverDrop = base.neverDrop;
            if (!base.defaultPolicy.keep.empty())
                out.defaultPolicy = base.defaultPolicy;
            if (!base.outputMode.empty() && base.outputMode != "summarize_rules")
                out.outputMode = base.outputMode;
            if (base.archiveEnabled)
                out.archiveEnabled = true;
            if (!base.archiveSink.empty())
                out.archiveSink = base.archiveSink;
            if (!base.archiveFormat.empty())
                out.archiveFormat = base.archiveFormat;
        } else if (out.enabled && out.tags.empty()) {
            out.profile = "balanced";
            compaction::applyProfile(out);
        }

        if (overrides) {
            auto* otags = ManifestYaml::find(*overrides, "tags");
            if (otags) {
                for (const auto& t : otags->children) {
                    if (t.key.empty())
                        continue;
                    CompactionTagPolicy p = compaction::tagOrDefault(out, t.key);
                    if (!t.children.empty())
                        p = parseTagPol(t);
                    else if (!t.value.empty())
                        p.keep = t.value;
                    out.tags[t.key] = p;
                }
            }
            auto ond = ManifestYaml::getList(*overrides, "never_drop");
            if (!ond.empty())
                out.neverDrop = ond;
        }

        auto* output = ManifestYaml::find(compactNode, "output");
        if (output) {
            std::string mode = ManifestYaml::get(*output, "mode");
            if (!mode.empty())
                out.outputMode = mode;
            auto* arch = ManifestYaml::find(*output, "archive");
            if (arch) {
                std::string ae = ManifestYaml::get(*arch, "enabled", "true");
                out.archiveEnabled = promptFlagEnabled(ae);
                std::string sink = ManifestYaml::get(*arch, "sink");
                if (!sink.empty())
                    out.archiveSink = sink;
                std::string fmt = ManifestYaml::get(*arch, "format");
                if (!fmt.empty())
                    out.archiveFormat = fmt;
            }
        }
        auto* archTop = ManifestYaml::find(compactNode, "archive");
        if (archTop) {
            std::string ae = ManifestYaml::get(*archTop, "enabled", "true");
            out.archiveEnabled = promptFlagEnabled(ae);
            std::string sink = ManifestYaml::get(*archTop, "sink");
            if (!sink.empty())
                out.archiveSink = sink;
            std::string fmt = ManifestYaml::get(*archTop, "format");
            if (!fmt.empty())
                out.archiveFormat = fmt;
        }

        auto* subs = ManifestYaml::find(compactNode, "subagents");
        if (subs) {
            std::string inh = ManifestYaml::get(*subs, "inherit");
            if (!inh.empty())
                out.subagentsInherit = promptFlagEnabled(inh);
            std::string cbr = ManifestYaml::get(*subs, "child_before_return");
            if (!cbr.empty())
                out.childBeforeReturn = promptFlagEnabled(cbr);
        }
    }

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
            // thinking_level: minimal|low|medium|high — reasoning-budget hint.
            // Accepted under cognitive_engine.thinking_level (sibling of
            // thinking) or cognitive_engine.primary.thinking_level (as the
            // operator often writes it).
            std::string thinkLevel = ManifestYaml::get(*engine, "thinking_level");
            if (thinkLevel.empty() && primary)
                thinkLevel = ManifestYaml::get(*primary, "thinking_level");
            if (!thinkLevel.empty()) {
                for (char& c : thinkLevel)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (thinkLevel == "minimal" || thinkLevel == "low" ||
                    thinkLevel == "medium" || thinkLevel == "high")
                    cfg.thinkingLevel = thinkLevel;
            }
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

        // Operator / user context (USER.md). Optional — empty path = omit block.
        // context.user: ./USER.md  |  user_context: …  |  operator: …
        if (context) {
            std::string userRel = ManifestYaml::get(*context, "user");
            if (userRel.empty())
                userRel = ManifestYaml::get(*context, "user_context");
            if (userRel.empty())
                userRel = ManifestYaml::get(*context, "operator");
            if (!userRel.empty()) {
                fs::path up = fs::path(userRel);
                if (up.is_absolute())
                    cfg.userPath = up.string();
                else
                    cfg.userPath = (base / up).lexically_normal().string();
            }
        }

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

        // Prompt-building knobs.
        // Preferred: runtime.prompt_building. Legacy: top-level prompt_building:.
        auto loadPromptBuilding = [&](const ManifestYaml::Node& pb) {
            auto* rc = ManifestYaml::find(pb, "runtime_capabilities");
            if (!rc)
                rc = ManifestYaml::find(pb, "available_actions");
            if (!rc)
                return;
            // Default OFF = tool cards (cheaper cold prompt). Explicit
            // enable/true still turns full JSON schemas back on.
            std::string inputSchemas = ManifestYaml::get(*rc, "input_schemas", "disable");
            std::string returnSchemas = ManifestYaml::get(*rc, "return_schemas", "disable");
            std::string examples = ManifestYaml::get(*rc, "usage_examples", "disable");

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
        };
        {
            const ManifestYaml::Node* promptBuilding = nullptr;
            auto* rtPb = ManifestYaml::find(root, "runtime");
            if (rtPb)
                promptBuilding = ManifestYaml::find(*rtPb, "prompt_building");
            if (!promptBuilding)
                promptBuilding = ManifestYaml::find(root, "prompt_building");
            if (promptBuilding)
                loadPromptBuilding(*promptBuilding);
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
            std::string ats = ManifestYaml::get(*runtime, "action_timeout_sec");
            if (ats.empty())
                ats = ManifestYaml::get(*runtime, "action_timeout");
            if (!ats.empty())
                cfg.actionTimeoutSec = std::stoi(ats);
            // runtime.throttling — stream stall / generation cutoffs, manifest
            // configurable (never hardcoded in the provider codec).
            if (auto* thr = ManifestYaml::find(*runtime, "throttling")) {
                std::string stall = ManifestYaml::get(*thr, "stall_timeout_sec");
                if (!stall.empty())
                    cfg.streamStallTimeoutSec = std::stoi(stall);
            }
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

        // Sandbox — full capability boundary + live binds
        auto* sandbox = ManifestYaml::find(root, "sandbox");
        if (sandbox) {
            cfg.sandboxConfigured = true;
            cfg.sandboxMode = ManifestYaml::get(*sandbox, "mode", "process");
            cfg.sandboxRuntime = ManifestYaml::get(*sandbox, "runtime", "");
            cfg.sandboxImage = ManifestYaml::get(*sandbox, "image", "");
            if (cfg.sandboxImage.empty() && !cfg.sandboxRuntime.empty())
                cfg.sandboxImage = cfg.sandboxRuntime;

            std::string net = ManifestYaml::get(*sandbox, "network", "out");
            if (!net.empty())
                cfg.sandboxNetwork = net;

            std::string ro = ManifestYaml::get(*sandbox, "readonly", "");
            if (ro.empty())
                ro = ManifestYaml::get(*sandbox, "read_only", "");
            if (!ro.empty())
                cfg.sandboxReadonly = promptFlagEnabled(ro);

            // allowed_commands — presence matters (empty list = block all)
            if (ManifestYaml::find(*sandbox, "allowed_commands")) {
                cfg.sandboxCommandsSet = true;
                cfg.sandboxAllowedCommands =
                    ManifestYaml::getList(*sandbox, "allowed_commands");
            }
            if (ManifestYaml::find(*sandbox, "allowed_paths")) {
                cfg.sandboxPathsSet = true;
                cfg.sandboxAllowedPaths = ManifestYaml::getList(*sandbox, "allowed_paths");
            }
            if (ManifestYaml::find(*sandbox, "allowed_hosts")) {
                cfg.sandboxHostsSet = true;
                cfg.sandboxAllowedHosts = ManifestYaml::getList(*sandbox, "allowed_hosts");
            }

            // files: shorthand host paths → /workspace/<basename>
            auto fileList = ManifestYaml::getList(*sandbox, "files");
            for (auto& f : fileList) {
                if (f.size() >= 2 && f.front() == '"' && f.back() == '"')
                    f = f.substr(1, f.size() - 2);
                if (!f.empty())
                    cfg.sandboxFiles.push_back(f);
            }

            // bind: host:guest[:ro] OR {path/host, to/guest, readonly}
            auto* bindNode = ManifestYaml::find(*sandbox, "bind");
            if (!bindNode)
                bindNode = ManifestYaml::find(*sandbox, "binds");
            if (bindNode) {
                for (const auto& item : bindNode->children) {
                    SandboxBind b;
                    // Structured map children on list item
                    if (!item.children.empty() || !item.key.empty()) {
                        std::string host = item.key == "path" || item.key == "host"
                                               ? item.value
                                               : "";
                        if (host.empty())
                            host = ManifestYaml::get(item, "path");
                        if (host.empty())
                            host = ManifestYaml::get(item, "host");
                        if (host.empty())
                            host = ManifestYaml::get(item, "from");
                        std::string guest = ManifestYaml::get(item, "to");
                        if (guest.empty())
                            guest = ManifestYaml::get(item, "guest");
                        if (guest.empty())
                            guest = ManifestYaml::get(item, "target");
                        std::string bro = ManifestYaml::get(item, "readonly");
                        if (bro.empty())
                            bro = ManifestYaml::get(item, "ro");
                        if (!host.empty() && !guest.empty()) {
                            b.host = host;
                            b.guest = guest;
                            b.readOnly = !bro.empty() ? promptFlagEnabled(bro) : false;
                            cfg.sandboxBinds.push_back(b);
                            continue;
                        }
                    }
                    // Scalar: "./ctx:/home/ctx" or "./ctx:/home/ctx:ro"
                    std::string spec = item.value;
                    if (spec.empty() && !item.key.empty()) {
                        // parser split on ": " — reconstruct host:guest
                        spec = item.key;
                        if (!item.value.empty())
                            spec += ":" + item.value;
                    }
                    if (spec.empty())
                        continue;
                    // Inline parse without pulling launcher dep into every TU:
                    // host:guest[:ro|:rw]
                    bool roFlag = false;
                    std::string body = spec;
                    if (body.size() > 3) {
                        auto tail = body.substr(body.size() - 3);
                        if (tail == ":ro") {
                            roFlag = true;
                            body = body.substr(0, body.size() - 3);
                        } else if (tail == ":rw") {
                            body = body.substr(0, body.size() - 3);
                        }
                    }
                    auto colon = body.rfind(':');
                    if (colon != std::string::npos && colon > 0 && colon + 1 < body.size()) {
                        b.host = body.substr(0, colon);
                        b.guest = body.substr(colon + 1);
                        b.readOnly = roFlag;
                        cfg.sandboxBinds.push_back(b);
                    } else if (!body.empty()) {
                        // bare path — guest filled at resolve time
                        b.host = body;
                        b.readOnly = roFlag || cfg.sandboxReadonly;
                        cfg.sandboxBinds.push_back(b);
                    }
                }
            }

            // Resolve relative host paths against manifest directory
            fs::path base = fs::path(manifestPath).parent_path();
            for (auto& b : cfg.sandboxBinds) {
                if (b.host.empty())
                    continue;
                fs::path h(b.host);
                if (h.is_relative())
                    h = base / h;
                b.host = h.lexically_normal().string();
                if (b.guest.empty())
                    b.guest = (fs::path("/workspace") / fs::path(b.host).filename()).string();
            }
            for (auto& f : cfg.sandboxFiles) {
                fs::path h(f);
                if (h.is_relative())
                    h = base / h;
                f = h.lexically_normal().string();
            }
        }

        // Compaction / context economy.
        // Preferred: runtime.compaction (or runtime.compacting).
        // Legacy alias: top-level compaction:/compacting: (still accepted).
        {
            const ManifestYaml::Node* compactNode = nullptr;
            auto* runtimeNode = ManifestYaml::find(root, "runtime");
            if (runtimeNode) {
                compactNode = ManifestYaml::find(*runtimeNode, "compaction");
                if (!compactNode)
                    compactNode = ManifestYaml::find(*runtimeNode, "compacting");
            }
            if (!compactNode) {
                compactNode = ManifestYaml::find(root, "compaction");
                if (!compactNode)
                    compactNode = ManifestYaml::find(root, "compacting");
            }
            if (compactNode)
                parseCompactionBlock(*compactNode, cfg.compaction);
        }

        // max_turns_per_cycle on runtime/context (legacy: history_cap_every_turns).
        auto readCycleTurns = [](const ManifestYaml::Node& node) -> std::string {
            std::string v = ManifestYaml::get(node, "max_turns_per_cycle");
            if (v.empty())
                v = ManifestYaml::get(node, "history_cap_every_turns");  // legacy alias
            return v;
        };
        auto* runtimeForHist = ManifestYaml::find(root, "runtime");
        if (runtimeForHist) {
            std::string he = readCycleTurns(*runtimeForHist);
            if (!he.empty())
                cfg.maxTurnsPerCycle = std::stoi(he);
        }
        auto* contextForHist = ManifestYaml::find(root, "context");
        if (contextForHist) {
            std::string he = readCycleTurns(*contextForHist);
            if (!he.empty())
                cfg.maxTurnsPerCycle = std::stoi(he);
        }

        // Retry / resilience — exponential backoff for transient upstream
        // failures. Preferred: runtime.retry. Legacy: top-level retry:.
        const ManifestYaml::Node* retry = nullptr;
        {
            auto* rt = ManifestYaml::find(root, "runtime");
            if (rt)
                retry = ManifestYaml::find(*rt, "retry");
            if (!retry)
                retry = ManifestYaml::find(root, "retry");
        }
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
        cfg.manifestPath = fs::absolute(fs::path(manifestPath)).lexically_normal().string();

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
        // CLASS-1: builtin tool.yml lookup must not depend on process CWD.
        // Hint = agent.yml path → catalog walks binary/project/CORTEX_HOME roots.
        const std::string schemaHint = manifestPath;

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
                    td.timeoutSec = schema.timeoutSec;
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
                auto schema = loadBuiltinToolSchema(bareName, schemaHint);
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
    static std::vector<ToolSchema> loadBuiltinTools(Agent& agent,
                                                    const std::string& schemaHint = "") {
        static const std::vector<std::string> builtin = {
            "exec", "grep", "list", "fs_read", "fs_write", "json", "web_fetch", "sleep", "artifact",
            "context_pin", "context_peek", "context_unpin", "ask_tool",
        };
        std::vector<ToolSchema> schemas;
        tools::registerDefaults();
        for (const auto& name : builtin) {
            auto schema = loadBuiltinToolSchema(name, schemaHint);
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

            // Compaction inherit: if parent has subagents.inherit and child did
            // not configure its own block, copy parent policy.
            {
                const auto& pc = agent.config().compaction;
                if (pc.subagentsInherit && pc.enabled && !subCfg.compaction.configured) {
                    subCfg.compaction = pc;
                    subCfg.compaction.configured = true;
                }
            }

            auto provider = providers::createProvider(subCfg.provider, subCfg.model);
            if (!provider) {
                continue;
            }

            auto subAgent = std::make_shared<Agent>(subCfg, provider);
            auto schemas = loadTools(agentManifest.string(), *subAgent);
            loadFeeds(agentManifest.string(), *subAgent);
            loadRelics(agentManifest.string(), *subAgent);
            // Children get the same card/schema/skills inject as parent path.
            const auto& rc = subCfg.promptBuilding.runtimeCapabilities;
            std::string schemaXml =
                toolSchemasToXml(schemas, 8, rc.inputSchemas, rc.returnSchemas,
                                 rc.usageExamples);
            if (!schemaXml.empty())
                subAgent->setEnv("__TOOL_SCHEMAS__", schemaXml);
            std::string skillsXml = loadSkillsXml(agentManifest.string());
            if (!skillsXml.empty())
                subAgent->setEnv("__SKILLS_XML__", skillsXml);
            std::string modsXml = loadPromptModulesXml(agentManifest.string());
            if (!modsXml.empty())
                subAgent->setEnv("__PROMPT_MODULES_XML__", modsXml);
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

    // Resolve a skill name/path to SKILL.md under the agent module.
    static fs::path resolveSkillPath(const fs::path& agentManifest, const std::string& name) {
        fs::path base = agentManifest.parent_path();
        fs::path requested(name);
        std::vector<fs::path> candidates;
        if (requested.is_absolute()) {
            candidates.push_back(requested);
        } else if (name.find('/') != std::string::npos || requested.extension() == ".md") {
            candidates.push_back(base / requested);
            if (requested.filename() != "SKILL.md")
                candidates.push_back(base / requested / "SKILL.md");
        } else {
            candidates.push_back(base / "skills" / name / "SKILL.md");
            candidates.push_back(base / "skills" / (name + ".md"));
            candidates.push_back(base / name / "SKILL.md");
        }
        for (const auto& c : candidates) {
            std::error_code ec;
            fs::path n = c.lexically_normal();
            if (fs::exists(n, ec) && fs::is_regular_file(n, ec))
                return n;
        }
        return {};
    }

    // Strip YAML frontmatter; return {title, body}.
    static void splitSkillMarkdown(const std::string& raw, std::string& title,
                                   std::string& body) {
        title.clear();
        body.clear();
        if (raw.size() >= 3 && raw.compare(0, 3, "---") == 0) {
            size_t end = raw.find("\n---", 3);
            if (end != std::string::npos) {
                std::string fm = raw.substr(3, end - 3);
                auto root = ManifestYaml::parse(fm);
                title = ManifestYaml::get(root, "name");
                if (title.empty())
                    title = ManifestYaml::get(root, "description");
                size_t bodyStart = raw.find('\n', end + 1);
                if (bodyStart != std::string::npos)
                    body = raw.substr(bodyStart + 1);
                else
                    body.clear();
                // drop leading blank lines
                while (!body.empty() && (body[0] == '\n' || body[0] == '\r'))
                    body.erase(body.begin());
                return;
            }
        }
        body = raw;
    }

    // Build <skill> cards for prompt injection from import.skills.
    // Empty import → empty string (no fake skill theater).
    static std::string loadSkillsXml(const std::string& manifestPath) {
        auto yaml = readFile(manifestPath);
        if (yaml.empty())
            return "";
        auto root = ManifestYaml::parse(yaml);
        auto* importNode = ManifestYaml::find(root, "import");
        if (!importNode)
            return "";
        auto skillList = ManifestYaml::getList(*importNode, "skills");
        if (skillList.empty())
            return "";

        std::ostringstream ss;
        fs::path agentMan(manifestPath);
        for (auto& name : skillList) {
            fs::path sp = resolveSkillPath(agentMan, name);
            if (sp.empty()) {
                std::cerr << "[manifest] skill not found: " << name
                          << " (from " << manifestPath << ")\n";
                continue;
            }
            std::string raw = readFile(sp.string());
            if (raw.empty())
                continue;
            std::string title, body;
            splitSkillMarkdown(raw, title, body);
            if (title.empty())
                title = sp.parent_path().filename().string();
            if (title.empty())
                title = name;
            // Cap body so skills stay laws, not essays.
            const size_t kCap = 900;
            if (body.size() > kCap)
                body = body.substr(0, kCap - 1) + "…";
            ss << "<skill name=\"" << title << "\">\n";
            if (!body.empty())
                ss << indentBlock(body, 2);
            ss << "</skill>\n";
        }
        return ss.str();
    }

    // Optional prompt modules from import.files (markdown snippets).
    // Only injects files that exist; missing paths are errors to stderr.
    static std::string loadPromptModulesXml(const std::string& manifestPath) {
        auto paths = loadFiles(manifestPath);
        if (paths.empty())
            return "";
        std::ostringstream ss;
        for (const auto& p : paths) {
            if (!fs::exists(p)) {
                std::cerr << "[manifest] import.files missing: " << p << "\n";
                continue;
            }
            // Skip non-text / huge dumps
            std::error_code ec;
            auto sz = fs::file_size(p, ec);
            if (ec || sz > 24000)
                continue;
            std::string raw = readFile(p);
            if (raw.empty())
                continue;
            const size_t kCap = 1200;
            if (raw.size() > kCap)
                raw = raw.substr(0, kCap - 1) + "…";
            std::string stem = fs::path(p).stem().string();
            ss << "<module name=\"" << stem << "\">\n";
            ss << indentBlock(raw, 2);
            ss << "</module>\n";
        }
        return ss.str();
    }

    // Compact one-line key inventory from a JSON Schema object (card mode).
    // Avoids dumping full property trees while still naming required params.
    static std::string toolSchemaCardLine(const std::string& inputSchemaJson) {
        if (inputSchemaJson.empty())
            return "";
        Json::Value root;
        Json::CharReaderBuilder rb;
        std::string errs;
        std::istringstream iss(inputSchemaJson);
        if (!Json::parseFromStream(rb, iss, &root, &errs) || !root.isObject())
            return "";
        std::vector<std::string> required;
        if (root.isMember("required") && root["required"].isArray()) {
            for (const auto& r : root["required"]) {
                if (r.isString())
                    required.push_back(r.asString());
            }
        }
        std::vector<std::string> keys;
        if (root.isMember("properties") && root["properties"].isObject()) {
            auto names = root["properties"].getMemberNames();
            keys.assign(names.begin(), names.end());
        }
        if (required.empty() && keys.empty())
            return "";
        std::ostringstream line;
        if (!required.empty()) {
            line << "required: ";
            for (size_t i = 0; i < required.size(); ++i) {
                if (i)
                    line << ", ";
                line << required[i];
            }
        }
        if (!keys.empty()) {
            if (!required.empty())
                line << " · ";
            line << "keys: ";
            const size_t kMax = 12;
            for (size_t i = 0; i < keys.size() && i < kMax; ++i) {
                if (i)
                    line << ", ";
                line << keys[i];
            }
            if (keys.size() > kMax)
                line << ", …";
        }
        return line.str();
    }

    // Build tool schemas XML for prompt injection.
    // Default path = CARDS (description + compact key line).
    // Full JSON when includeInputSchemas / CORTEX_TOOL_FULL=1.
    static std::string toolSchemasToXml(const std::vector<ToolSchema>& schemas, int baseIndent = 8,
                                        bool includeInputSchemas = false,
                                        bool includeReturnSchemas = false,
                                        bool includeExamples = false) {
        if (schemas.empty())
            return "";
        // Escape hatch: force full input JSON regardless of agent.yml flags.
        const bool forceFull = []() {
            const char* e = std::getenv("CORTEX_TOOL_FULL");
            return e && e[0] && std::string(e) != "0" && std::string(e) != "false";
        }();
        const bool fullInput = includeInputSchemas || forceFull;
        std::ostringstream ss;
        std::string toolPad(baseIndent, ' ');
        std::string fieldPad(baseIndent + 4, ' ');
        for (auto& s : schemas) {
            ss << toolPad << "<tool name=\"" << s.name << "\">\n";
            if (!s.description.empty()) {
                // Cap wall-of-text PE in the hot prompt; full tool.yml still on disk.
                std::string desc = s.description;
                const size_t kDescCap = 720;
                if (desc.size() > kDescCap)
                    desc = desc.substr(0, kDescCap - 1) + "…";
                ss << fieldPad << "<description>" << desc << "</description>\n";
            }
            if (s.inputType != "json" || !s.textParam.empty()) {
                ss << fieldPad << "<input mode=\"" << s.inputType << "\"";
                if (!s.textParam.empty())
                    ss << " text_param=\"" << s.textParam << "\"";
                ss << ">";
                if (!s.textParam.empty())
                    ss << "Text action bodies are assigned to params." << s.textParam << ".";
                ss << "</input>\n";
            }
            if (fullInput && !s.inputSchema.empty()) {
                ss << fieldPad << "<params>\n"
                   << indentBlock(prettyJson(s.inputSchema), baseIndent + 8) << fieldPad
                   << "</params>\n";
            } else if (!s.inputSchema.empty()) {
                std::string card = toolSchemaCardLine(s.inputSchema);
                if (!card.empty())
                    ss << fieldPad << "<params card=\"true\">" << card << "</params>\n";
            }
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
            std::string to = ManifestYaml::get(*impl, "timeout", "");
            if (to.empty())
                to = ManifestYaml::get(*impl, "timeout_sec", "");
            if (!to.empty()) {
                try {
                    s.timeoutSec = std::stoi(to);
                } catch (...) {
                }
            }
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
        if (s.timeoutSec <= 0) {
            std::string to = ManifestYaml::get(root, "timeout", "");
            if (to.empty())
                to = ManifestYaml::get(root, "timeout_sec", "");
            if (!to.empty()) {
                try {
                    s.timeoutSec = std::stoi(to);
                } catch (...) {
                }
            }
        }
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

    // Resolve builtin tool.yml independent of process CWD.
    // schemaHint: agent.yml path or any path inside a cortex tree (optional).
    static ToolSchema loadBuiltinToolSchema(const std::string& name,
                                            const std::string& schemaHint = "") {
        // 1) Catalog roots: override → CORTEX_HOME → CWD walk-up → binary parents.
        //    This is the Class-1 fix for launching with cwd=inkcell (or any foreign tree).
        const char* rels[] = {
            "built-in/tools/",  // under manifests/
            "tools/",           // alt layout under manifests/
        };
        for (const char* rel : rels) {
            std::string found =
                catalog::findShared(std::string(rel) + name + "/tool.yml", schemaHint);
            if (!found.empty())
                return loadToolSchema(found);
        }

        // 2) Direct candidates relative to hint / binary (belt).
        std::vector<fs::path> bases;
        if (!schemaHint.empty()) {
            fs::path h = schemaHint;
            std::error_code ec;
            if (fs::is_regular_file(h, ec))
                h = h.parent_path();
            fs::path mroot = catalog::resolveManifestsRoot(h);
            if (!mroot.empty())
                bases.push_back(mroot);
            bases.push_back(h);
        }
        {
            char buf[PATH_MAX];
            ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                fs::path cur = fs::path(buf).parent_path();
                for (int i = 0; i < 8; ++i) {
                    bases.push_back(cur);
                    bases.push_back(cur / "manifests");
                    if (!cur.has_parent_path() || cur == cur.root_path())
                        break;
                    cur = cur.parent_path();
                }
            }
        }
        bases.push_back(fs::current_path());
        bases.push_back(fs::current_path() / "manifests");

        std::error_code ec;
        for (const auto& base : bases) {
            for (const char* mid : {"built-in/tools/", "tools/", "manifests/built-in/tools/",
                                    "manifests/tools/"}) {
                fs::path p = base / mid / name / "tool.yml";
                if (fs::is_regular_file(p, ec))
                    return loadToolSchema(p.string());
            }
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
