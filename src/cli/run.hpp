#pragma once
// run subcommand: agent execution, interactive pickers, manifest resolution.

#include <iostream>
#include <string>
#include <vector>

#include "src/cli/options.hpp"
#include "src/utils/ansi.hpp"
#include "src/cli/session.hpp"
#include "src/core/agent.hpp"
#include "src/core/agent_catalog.hpp"
#include "src/core/manifest_loader.hpp"
#include "src/core/sandbox_launcher.hpp"
#include "src/providers/factory.hpp"
#include "src/sandbox/policy.hpp"
#include "src/ui/app/mk3_tui_app.hpp"

namespace cortex::mk3::cli {

static bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ═══════════════════════════════════════════════════════════════════════
// Interactive provider/model picker (fzf-style)
// ═══════════════════════════════════════════════════════════════════════
static bool interactivePicker(std::string& outProvider, std::string& outModel) {
    // ── Gather providers (unchanged) ──
    static const std::vector<std::pair<std::string, std::string>> providerInfo = {
        {"deepseek", "DeepSeek API"},
        {"openrouter", "OpenRouter"},
        {"xai", "xAI / Grok"},
        {"openai-codex", "OpenAI Codex"},
        {"groq", "Groq"},
        {"zen", "OpenCode Zen (free)"},
        {"opencode-go", "OpenCode Go ($10/mo)"},
        {"opencode", "OpenCode (alias→zen)"},
        {"together", "Together AI"},
        {"fireworks", "Fireworks AI"},
        {"sambanova", "SambaNova"},
        {"cerebras", "Cerebras"},
        {"hyperbolic", "Hyperbolic"},
        {"llm7", "LLM7"},
        {"nvidia", "NVIDIA"},
    };
    auto avail = providers::availableProviders();
    std::vector<std::pair<std::string, std::string>> providers;
    for (const auto& [name, desc] : providerInfo) {
        if (std::find(avail.begin(), avail.end(), name) != avail.end())
            providers.push_back({name, desc});
    }
    if (providers.empty()) {
        std::cerr << "No providers available.\n";
        return false;
    }

    // ── Provider selection (skip if already chosen) ──
    if (outProvider.empty()) {
        cli::ListPickerConfig cfg;
        cfg.title = "Select Provider";
        cfg.hint = "j/k or ↑↓ to navigate, Enter to select, Esc/q cancel";
        int idx = cli::run_list_picker((int)providers.size(),
                                       [&](int i, bool sel) {
                                           const auto& [name, desc] = providers[(size_t)i];
                                           std::ostringstream o;
                                           std::string row = name + " — " + desc;
                                           if (sel)
                                               o << "\033[7;36m  " << row << "\033[0m";
                                           else
                                               o << "  " << row;
                                           return o.str();
                                       },
                                       cfg);
        if (idx < 0)
            return false;
        outProvider = providers[(size_t)idx].first;
    }

    // ── Model selection (unchanged business logic) ──
    auto provider = providers::createProvider(outProvider, "");
    if (!provider) {
        std::cerr << "Failed to create provider: " << outProvider << "\n";
        return false;
    }
    auto models = provider->listModels();
    if (outProvider == "openrouter") {
        std::vector<ILlmProvider::ModelInfo> freeOnly;
        for (auto& m : models)
            if (m.isFree)
                freeOnly.push_back(m);
        if (!freeOnly.empty())
            models = std::move(freeOnly);
    }
    if (models.empty()) {
        std::cerr << "No models available for " << outProvider
                  << ". Use --model to specify one.\n";
        return true;  // provider was selected, just no model list
    }

    std::vector<std::pair<std::string, std::string>> modelItems;
    for (auto& m : models) {
        std::string label = m.name.empty() ? m.id : m.name;
        if (m.isFree)
            label += " [free]";
        label += " — " + std::to_string(m.contextWindow / 1000) + "K";
        modelItems.push_back({m.id, label});
    }

    cli::ListPickerConfig mcfg;
    mcfg.title = "Select Model — " + outProvider;
    mcfg.hint = "j/k or ↑↓ to navigate, Enter to select, Esc/q cancel";
    int midx = cli::run_list_picker(
        (int)modelItems.size(),
        [&](int i, bool sel) {
            const auto& item = modelItems[(size_t)i];
            std::ostringstream o;
            if (sel)
                o << "\033[7;36m  " << item.second << "\033[0m";
            else
                o << "  " << item.second;
            return o.str();
        },
        mcfg);
    if (midx < 0)
        return false;
    outModel = modelItems[(size_t)midx].first;
    return true;
}

static bool validateExplicitModel(const std::string& providerName, const std::string& model,
                                  const std::shared_ptr<ILlmProvider>& provider) {
    if (model.empty())
        return true;

    auto models = provider->listModels();
    if (models.empty())
        return true;  // Provider cannot validate/catalog failed; defer to API.

    std::vector<std::string> suggestions;
    for (const auto& m : models) {
        if (m.id == model)
            return true;
        if (endsWith(m.id, "/" + model) || m.id.find(model) != std::string::npos) {
            if (suggestions.size() < 5)
                suggestions.push_back(m.id);
        }
    }

    std::cerr << "Error: model '" << model << "' is not listed for provider '" << providerName
              << "'.\n";
    if (!suggestions.empty()) {
        std::cerr << "Did you mean:\n";
        for (const auto& s : suggestions)
            std::cerr << "  " << s << "\n";
    }
    std::cerr << "Run 'cortex-mk3 list --provider " << providerName
              << "' to see valid model IDs.\n";
    return false;
}

// ═══════════════════════════════════════════════════════════════════════
// Bare -m / --manifest: defer to the inkcell app's manifest browser
// (hub launch) in interactive mode; keep the listing fallback non-TTY.
// ═══════════════════════════════════════════════════════════════════════
static bool resolveCliManifest(CliConfig& cli) {
    if (cli.manifestPickerRequested && cli.manifestPath.empty()) {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
            auto agents = catalog::discoverAgents(cli.manifestDir);
            if (agents.empty()) {
                std::cerr << "No agents found under manifests/agents.\n\nmanifests/ roots:\n";
                for (const auto& [root, source] : catalog::manifestsSearchRoots(cli.manifestDir))
                    std::cerr << "  [" << source << "] " << root << "\n";
                std::cerr << "\nInstall:\n"
                             "  $CORTEX_HOME/manifests/agents/<name>/agent.yml\n"
                             "  ~/.config/cortex/manifests/agents/<name>/agent.yml\n"
                             "List: cortex-mk3 list --agents\n";
            } else {
                std::cerr << "Agents (" << agents.size()
                          << ") under manifests/ — re-run with -m <name> or a TTY:\n\n";
                for (size_t i = 0; i < agents.size(); ++i) {
                    for (const auto& line : catalog::formatOwnershipTree(agents[i], false))
                        std::cerr << line << "\n";
                    if (i + 1 < agents.size())
                        std::cerr << "\n";
                }
            }
            return false;
        }
        if (!cli.prompt.empty() && !cli.replMode) {
            std::cerr << "Error: bare -m opens the manifest browser (interactive TUI). "
                         "Use `-m <name>` with -p.\n";
            return false;
        }
        return true;  // interactive app launches at the manifests surface
    }
    if (cli.manifestPath.empty())
        return true;

    std::string err;
    std::string resolved =
        catalog::resolveAgent(cli.manifestPath, cli.manifestDir, &err);
    if (resolved.empty()) {
        std::cerr << "Error: " << err << "\n";
        return false;
    }
    if (resolved != cli.manifestPath)
        std::cerr << "\033[2m[manifest]\033[0m " << cli.manifestPath << " → " << resolved << "\n";
    cli.manifestPath = resolved;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════
// Command: run (agent execution)
// ═══════════════════════════════════════════════════════════════════════
static int cmdRun(CliConfig& cli) {
    // Stale-binary guard: bare CWD `sessions/` or `state/` dirs are a legacy
    // (pre home-based) artifact, not where this build reads/writes. Warn so an
    // operator isn't confused why sessions "disappear" (the binary is using
    // ~/.cortex / $CORTEX_HOME).
    {
        std::error_code ec;
        for (const char* d : {"sessions", "state"}) {
            if (fs::is_directory(d, ec)) {
                std::cerr << "\033[33mcortex-mk3:\033[0m stale CWD " << d
                          << "/ dir present (legacy location). Sessions/state "
                             "now live under ~/.cortex or $CORTEX_HOME. Remove "
                          << d << "/ if unused.\n";
            }
        }
    }

    // Global agent / manifest selection (any-CWD catalog)
    if (!resolveCliManifest(cli))
        return cli.manifestPickerRequested ? 0 : 1;

    // ── Interactive picker ──
    // Three cases trigger the picker (only in non-REPL, no-prompt, no-manifest mode):
    //   1. `--provider` bare → pick provider then model
    //   2. `--provider openrouter` with no --model → pick model for that provider
    //   3. No --provider and no --model → pick provider then model
    bool needPicker = false;
    if (cli.prompt.empty() && cli.promptFile.empty() && !cli.replMode && cli.manifestPath.empty() &&
        !cli.manifestPickerRequested) {
        if (cli.providerPickerRequested)
            needPicker = true;  // bare --provider
        else if (cli.providerSet && !cli.modelSet && cli.model.empty())
            needPicker = true;  // --provider X but no --model
        else if (!cli.providerSet && cli.model.empty())
            needPicker = true;  // nothing specified at all
    }

    if (needPicker) {
        std::string pickedProvider, pickedModel;
        // If provider already chosen, skip provider step and go straight to models.
        if (cli.providerSet && !cli.provider.empty())
            pickedProvider = cli.provider;
        if (interactivePicker(pickedProvider, pickedModel)) {
            cli.provider = pickedProvider;
            cli.providerSet = true;
            if (!pickedModel.empty()) {
                cli.model = pickedModel;
                cli.modelSet = true;
            }
        } else {
            return 0;
        }
    }

    // Load prompt from file if specified
    if (cli.prompt.empty() && !cli.promptFile.empty()) {
        std::ifstream f(cli.promptFile);
        if (!f.good()) {
            std::cerr << "Error: cannot read " << cli.promptFile << "\n";
            return 1;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        cli.prompt = ss.str();
    }

    // Resuming should restore the runtime surface before agent construction.
    std::string activeSessionId;
    std::string forkedFrom;  // set when --fork was used, shown in banner
    bool didResume = false;  // true when -c/-r/--session/--fork resolved an existing session

    // --fork <id>: copy an existing session and continue under a fresh id.
    // Must run before the resume block so it sets cli.sessionId correctly.
    if (!cli.forkFrom.empty() && !cli.noSession) {
        session::SessionManager sm;
        if (!sm.exists(cli.forkFrom)) {
            std::cerr << "Error: --fork target session not found: " << cli.forkFrom << "\n";
            return 1;
        }
        // Deep-copy including ui_timeline (session audit S0.4).
        std::string newId = session::mintSessionId();
        Session fork = session::forkSession(sm, cli.forkFrom, newId, cli.sessionName);
        forkedFrom = cli.forkFrom;
        cli.sessionId = newId;
        cli.forkFrom.clear();
        didResume = true;  // fork is a form of resume (copied history)
        // Process-wide single id — inkcell + atexit flush see the fork.
        session::activeSession().set(newId, cli.noSession);
    }

    if (!cli.noSession && (cli.resumePicker || cli.continueSession || !cli.sessionId.empty())) {
        std::string resolved = resolveSessionId(cli, false);
        // If the user cancelled -r, the picker returns "". Exit cleanly
        // instead of dropping them into a fresh session.
        if (cli.resumePicker && resolved.empty()) {
            std::cerr << "\033[2m[session] Resume cancelled.\033[0m\n";
            return 0;
        }
        // Only restore metadata if we're truly loading an existing session.
        // resolveSessionId may mint a brand-new id when nothing exists yet.
        session::SessionManager sm;
        bool existed = !resolved.empty() && sm.exists(resolved);
        if (existed) {
            applySessionMetadata(cli, resolved);
            activeSessionId = resolved;
            cli.sessionId = resolved;
            didResume = true;
        } else {
            // No prior session — the resume flags were ignored. Stay with a
            // fresh id (already set by resolveSessionId's default branch).
            activeSessionId = resolved;
            cli.sessionId = resolved;
        }
        // Single process-wide id for inkcell + atexit/SIGINT flush (F1/F19).
        if (!resolved.empty())
            session::activeSession().set(resolved, cli.noSession);
        cli.resumePicker = false;
        cli.continueSession = false;

        // Resume banner — printed once, after metadata is restored.
        if (cli.sessionBanner) {
            const char* kindStr = forkedFrom.empty() ? "Resuming" : "Forked";
            printResumeBanner(activeSessionId, kindStr, 0, forkedFrom);
        }
    }

    // Dry run: validate and exit
    if (cli.dryRun) {
        std::cout << "[dry-run] Validating configuration...\n";

        AgentConfig dryCfg;
        dryCfg.provider = cli.provider;
        dryCfg.model = cli.model;
        dryCfg.harnessPath = cli.harnessPromptPath;
        dryCfg.systemPromptPath = cli.systemPromptPath;
        dryCfg.personaPath = "manifests/persona/default.md";

        if (!cli.manifestPath.empty()) {
            std::ifstream mf(cli.manifestPath);
            if (!mf.good()) {
                std::cerr << "  ✗ Manifest not found: " << cli.manifestPath << "\n";
                return 1;
            }
            dryCfg = ManifestLoader::loadAgentConfig(cli.manifestPath);
            ManifestLoader::loadEnv(cli.manifestPath, dryCfg);
            catalog::fixDefaultPromptPaths(dryCfg, cli.manifestDir);
            if (cli.providerSet)
                dryCfg.provider = cli.provider;
            if (cli.modelSet)
                dryCfg.model = cli.model;
            if (!cli.harnessPromptPath.empty()) {
                std::string herr;
                std::string hp =
                    catalog::resolveHarnessPath(cli.harnessPromptPath, cli.manifestDir, &herr);
                if (hp.empty()) {
                    std::cerr << "  ✗ " << herr << "\n";
                    return 1;
                }
                dryCfg.harnessPath = hp;
            }
        } else {
            if (dryCfg.harnessPath.empty())
                dryCfg.harnessPath = "manifests/harness/default.md";
            if (!cli.harnessPromptPath.empty()) {
                std::string herr;
                std::string hp =
                    catalog::resolveHarnessPath(cli.harnessPromptPath, cli.manifestDir, &herr);
                if (hp.empty()) {
                    std::cerr << "  ✗ " << herr << "\n";
                    return 1;
                }
                dryCfg.harnessPath = hp;
            }
            if (dryCfg.systemPromptPath.empty())
                dryCfg.systemPromptPath = "manifests/system/default.md";
            catalog::fixDefaultPromptPaths(dryCfg, cli.manifestDir);
        }

        std::cout << "  provider: " << dryCfg.provider << "\n";
        std::cout << "  model:    " << dryCfg.model << "\n";

        auto provider = providers::createProvider(dryCfg.provider, dryCfg.model);
        if (!provider) {
            std::cerr << "  ✗ Invalid provider: " << dryCfg.provider << "\n";
            return 1;
        }
        std::cout << "  ✓ Provider resolved\n";

        auto requireFile = [](const std::string& label, const std::string& path) -> bool {
            if (path.empty())
                return true;
            std::ifstream f(path);
            if (!f.good()) {
                std::cerr << "  ✗ " << label << " not found: " << path << "\n";
                return false;
            }
            std::cout << "  ✓ " << label << ": " << path << "\n";
            return true;
        };
        if (!requireFile("Harness prompt", dryCfg.harnessPath) ||
            !requireFile("System prompt", dryCfg.systemPromptPath) ||
            !requireFile("Persona prompt", dryCfg.personaPath))
            return 1;
        // Optional operator context — warn only if declared but missing.
        if (!dryCfg.userPath.empty()) {
            std::ifstream uf(dryCfg.userPath);
            if (uf.good())
                std::cout << "  ✓ User context: " << dryCfg.userPath << "\n";
            else
                std::cerr << "  ⚠ User context missing: " << dryCfg.userPath << "\n";
        }

        if (!cli.manifestPath.empty()) {
            std::cout << "  ✓ Manifest: " << cli.manifestPath << "\n";
        }

        std::cout << "  sandbox:  ";
        if (cli.sandbox || dryCfg.sandboxConfigured) {
            std::cout << (cli.sandboxReadOnly || dryCfg.sandboxReadonly ? "read-only" : "on")
                      << " mode=" << dryCfg.sandboxMode
                      << " binds=" << dryCfg.sandboxBinds.size()
                      << " files=" << dryCfg.sandboxFiles.size();
        } else {
            std::cout << "off";
        }
        std::cout << "\n";
        std::cout << "  ✓ Configuration valid. Use without --dry-run to execute.\n";
        return 0;
    }

    // ── Create agent ──
    AgentConfig acfg;
    ansi::colorEnabled() = true;

    // Resume without -m: if applySessionMetadata resolved a manifest via
    // agent_name, cli.manifestPath is set. If it only set sessionAgentName,
    // resolve once more here before falling through to builtin.
    if (cli.manifestPath.empty() && didResume && !cli.sessionAgentName.empty() &&
        cli.sessionAgentName != "cortext-builtin-agent" &&
        cli.sessionAgentName != "cortex") {
        std::string err;
        std::string resolved =
            catalog::resolveAgent(cli.sessionAgentName, cli.manifestDir, &err);
        if (!resolved.empty())
            cli.manifestPath = resolved;
        else
            std::cerr << "[resume] could not resolve agent '" << cli.sessionAgentName
                      << "': " << err << "\n";
    }

    if (!cli.manifestPath.empty()) {
        acfg = ManifestLoader::loadAgentConfig(cli.manifestPath);
        ManifestLoader::loadEnv(cli.manifestPath, acfg);
        catalog::fixDefaultPromptPaths(acfg, cli.manifestDir);
        // Cognitive engine priority:
        //   1) explicit CLI --provider / --model
        //   2) session file (last live engine, including /model switches)
        //   3) agent.yml cognitive_engine (keep acfg as loaded)
        //   4) config file / hardcoded — already in cli.* but must NOT beat (3)
        // Bug: didResume + cli.provider from ~/.config (opencode-go) used to
        // overwrite agent.yml free model → RegionError 403 on every -c.
        if (cli.providerSet || cli.providerFromSession)
            acfg.provider = cli.provider;
        if (cli.modelSet || cli.modelFromSession)
            acfg.model = cli.model;
        if (!cli.harnessPromptPath.empty()) {
            std::string herr;
            std::string hp =
                catalog::resolveHarnessPath(cli.harnessPromptPath, cli.manifestDir, &herr);
            if (hp.empty()) {
                std::cerr << "Error: " << herr << "\n";
                return 1;
            }
            acfg.harnessPath = hp;
        }

        if (acfg.sandboxMode == "docker" && !fs::exists("/.dockerenv")) {
            // Live binds come from sandbox.bind / sandbox.files on AgentConfig.
            // import.files stays prompt-only and is intentionally not mounted.
            return sandbox::launchDocker(cli.manifestPath, acfg, acfg.sandboxFiles);
        }
    } else {
        // Last-ditch resume identity: session named a real agent but path resolve
        // failed — keep the name so the header is honest, still builtin surface.
        acfg.name = !cli.sessionAgentName.empty() ? cli.sessionAgentName
                                                  : "cortext-builtin-agent";
        if (didResume && acfg.name != "cortext-builtin-agent") {
            std::cerr << "[resume] warning: could not resolve manifest for agent '"
                      << acfg.name << "' — running builtin surface with session history\n";
        } else if (!didResume) {
            acfg.name = "cortext-builtin-agent";
        }
        // Builtin surface: session engine > config/cli defaults.
        if (cli.providerSet || cli.providerFromSession || !cli.provider.empty())
            acfg.provider = cli.provider;
        if (cli.modelSet || cli.modelFromSession) {
            acfg.model = cli.model;
        } else if (!cli.model.empty() && !didResume) {
            // Cold builtin launch may take config model; resume without session
            // model keeps provider default via createProvider.
            acfg.model = cli.model;
        } else if (!cli.model.empty() && didResume && cli.modelFromSession) {
            acfg.model = cli.model;
        }
        if (acfg.model.empty() && !cli.model.empty() &&
            (cli.modelSet || cli.modelFromSession))
            acfg.model = cli.model;
        if (!cli.harnessPromptPath.empty()) {
            std::string herr;
            std::string hp =
                catalog::resolveHarnessPath(cli.harnessPromptPath, cli.manifestDir, &herr);
            if (hp.empty()) {
                std::cerr << "Error: " << herr << "\n";
                return 1;
            }
            acfg.harnessPath = hp;
        } else {
            acfg.harnessPath = "manifests/harness/default.md";
        }
        if (!cli.systemPromptPath.empty()) {
            acfg.systemPromptPath = cli.systemPromptPath;
        } else {
            acfg.systemPromptPath = "manifests/system/default.md";
        }
        acfg.personaPath = "manifests/persona/default.md";
        catalog::fixDefaultPromptPaths(acfg, cli.manifestDir);
    }

    // Note: inkcell already normalized to experimental above.
    if (cli.tuiMode == "legacy") {
        std::cerr << "Error: --tui legacy was removed — inkcell is the only TUI backend "
                     "(use --tui experimental or --tui inkcell)\n";
        return 1;
    }
    if (cli.tuiMode != "experimental") {
        std::cerr << "Error: unknown TUI backend '" << cli.tuiMode
                  << "' (expected experimental|inkcell)\n";
        return 1;
    }

    auto provider = providers::createProvider(acfg.provider, acfg.model);
    if (!provider) {
        std::cerr << "Error: unknown provider '" << acfg.provider << "'\n";
        std::cerr << "Run 'cortex-mk3 list --providers' to see available providers.\n";
        return 1;
    }
    if (cli.modelSet && !validateExplicitModel(acfg.provider, acfg.model, provider))
        return 1;

    // TUI stderr is not a side channel; provider retry logs corrupt alternate-screen rendering.
    provider->setQuietLogs(true);

    Agent agent(acfg, provider);
    if (cli.iterations > 0)
        agent.setIterationCap(cli.iterations);
    // Manifest runtime.{no_session,ephemeral} OR with CLI flags (orthogonal).
    if (acfg.defaultNoSession)
        cli.noSession = true;
    if (acfg.defaultEphemeral)
        cli.ephemeral = true;

    // Manifest catalog semantics: ./manifests is a lookup catalog, not an
    // implicit capability surface. Capabilities are loaded only from the active
    // manifest's explicit import: block. This prevents unrelated catalog agents
    // (e.g. orchestrator) from appearing as sub-agents of assistant.
    std::vector<ToolSchema> allSchemas;
    std::string workflowXml;

    if (!cli.manifestPath.empty()) {
        ManifestLoader::loadFeeds(cli.manifestPath, agent);
        ManifestLoader::loadRelics(cli.manifestPath, agent);
        auto schemas = ManifestLoader::loadTools(cli.manifestPath, agent);
        allSchemas.insert(allSchemas.end(), schemas.begin(), schemas.end());
        ManifestLoader::loadSubAgents(cli.manifestPath, agent, acfg.provider);
        workflowXml += ManifestLoader::loadWorkflows(cli.manifestPath);
    } else {
        // No manifest → grant the standard built-in tool set so bare `run`
        // has a working surface. Without this, <action_available> renders
        // empty and every <action type="tool"> fails with "not imported by
        // active manifest". Feeds/relics/persona/harness stay manifest-only.
        auto schemas = ManifestLoader::loadBuiltinTools(agent);
        allSchemas.insert(allSchemas.end(), schemas.begin(), schemas.end());
    }

    const auto& rc = acfg.promptBuilding.runtimeCapabilities;
    std::string schemaXml = ManifestLoader::toolSchemasToXml(allSchemas, 8, rc.inputSchemas,
                                                             rc.returnSchemas, rc.usageExamples);
    if (!schemaXml.empty())
        agent.setEnv("__TOOL_SCHEMAS__", schemaXml);
    if (!workflowXml.empty())
        agent.setEnv("__WORKFLOW_XML__", workflowXml);
    if (!cli.manifestPath.empty()) {
        std::string skillsXml = ManifestLoader::loadSkillsXml(cli.manifestPath);
        if (!skillsXml.empty())
            agent.setEnv("__SKILLS_XML__", skillsXml);
        std::string modsXml = ManifestLoader::loadPromptModulesXml(cli.manifestPath);
        if (!modsXml.empty())
            agent.setEnv("__PROMPT_MODULES_XML__", modsXml);
    }

    if (cli.debug)
        agent.setEnv("__DEBUG_MODE__", "true");
    agent.setEnv("__TOOL_ANSI__", cli.toolAnsi ? "true" : "false");
    if (cli.raw)
        agent.setRaw(true);
    if (cli.verbose)
        agent.setVerbose(true);
    // Manifest runtime.dev_mode (or DEV_MODE) — auto full iteration dumps.
    // Also honor env CORTEX_DEV_MODE=1 for lazy live tests without editing YAML.
    if (acfg.devMode || (std::getenv("CORTEX_DEV_MODE") &&
                         std::string(std::getenv("CORTEX_DEV_MODE")) != "0" &&
                         std::string(std::getenv("CORTEX_DEV_MODE")) != "false")) {
        agent.setDevMode(true);
        if (!cli.raw)
            std::cerr << "[dev_mode] iteration dumps → ~/.cortex/dev/<session>/ + CWD copies\n";
    }

    // Sandbox — manifest sandbox: is source of truth; CLI --sandbox/--sandbox-ro
    // forces enable (and optional global RO) on top.
    {
        std::string cwd = fs::current_path().string();

        // Expand sandbox.files → binds and materialize process-mode symlinks
        // so bound paths are live CRUD surfaces that reflect on the host.
        if (acfg.sandboxConfigured || !acfg.sandboxBinds.empty() || !acfg.sandboxFiles.empty()) {
            sandbox::expandFilesToBinds(acfg);
            fs::path base =
                acfg.manifestDir.empty() ? fs::current_path() : fs::path(acfg.manifestDir);
            sandbox::resolveBindHosts(acfg, base);
            if (acfg.sandboxMode == "process" || acfg.sandboxMode.empty()) {
                auto mat = sandbox::materializeProcessBinds(acfg, fs::current_path());
                if (!cli.raw) {
                    for (const auto& w : mat.warnings)
                        std::cerr << "[sandbox] warn: " << w << "\n";
                    if (!mat.created.empty())
                        std::cerr << "[sandbox] binds: " << mat.created.size()
                                  << " symlink(s) materialized\n";
                }
            }
        }

        sandbox::SandboxPolicy policy = sandbox::makePolicyFromConfig(acfg, cwd);
        // allowed_paths from manifest are often relative to the agent module;
        // resolve them against manifestDir so "./" means the agent dir.
        if (!acfg.manifestDir.empty()) {
            for (auto& p : policy.allowedPaths) {
                fs::path hp(p);
                if (hp.is_relative())
                    p = (fs::path(acfg.manifestDir) / hp).lexically_normal().string();
            }
            policy.binds = acfg.sandboxBinds;
        }
        policy = sandbox::mergeCliSandbox(policy, cli.sandbox, cli.sandboxReadOnly, cwd);
        if (policy.enabled) {
            agent.setSandboxPolicy(policy);
            if (!cli.raw) {
                std::cerr << "[sandbox] " << (policy.readOnly ? "read-only" : "enabled")
                          << " mode=" << acfg.sandboxMode << " workspace=" << cwd;
                if (!policy.binds.empty())
                    std::cerr << " binds=" << policy.binds.size();
                if (!policy.allowedCommands.empty())
                    std::cerr << " cmds=" << policy.allowedCommands.size();
                std::cerr << "\n";
            }
        }
    }

    // ── One-shot mode ──
    if (!cli.prompt.empty() && !cli.replMode) {
        std::string promptSessionId =
            cli.noSession
                ? ""
                : (activeSessionId.empty() ? resolveSessionId(cli, false) : activeSessionId);
        // Vet-fix: bare launch with no `--resume/--continue/--session`
        // and no recorded session id stays entirely empty — no
        // auto-mint, no metadata file. The Sessions hub will only see
        // previously-persisted real sessions on disk.
        if (!cli.noSession && !promptSessionId.empty())
            persistSessionMetadata(promptSessionId, cli, acfg);

        // Native inkcell App (experimental; inkcell alias already normalized).
        if (cli.tuiMode == "experimental") {
            ui::InkcellAppConfig icfg;
            icfg.agentName = acfg.name;
            icfg.provider = acfg.provider;
            icfg.model = acfg.model;
            icfg.manifestPath = cli.manifestPath;
            icfg.manifestDir = cli.manifestDir;
            icfg.harnessPath = acfg.harnessPath;
            icfg.systemPromptPath = acfg.systemPromptPath;
            icfg.personaPath = acfg.personaPath;
            icfg.sessionId = promptSessionId;
            icfg.toolCount = static_cast<int>(allSchemas.size());
            icfg.feedCount = static_cast<int>(agent.feedNames().size());
            icfg.relicCount = static_cast<int>(agent.relicNames().size());
            icfg.subAgentCount = static_cast<int>(agent.subAgentNames().size());
            icfg.noSession = cli.noSession;
            icfg.ephemeral = cli.ephemeral;  // exit-on-done only
            icfg.showThoughts = cli.showThoughts;
            icfg.truncateBodies = cli.truncateBodies;
            // -p + experimental:
            //   --ephemeral → run turn, exit when done
            //   otherwise  → seed prompt into interactive REPL (multi-turn)
            if (cli.ephemeral) {
                return ui::runInkcellOneShot(icfg, agent, cli.prompt, promptSessionId, cli.noSession);
            }
            icfg.initialPrompt = cli.prompt;
            return ui::runInkcellRepl(icfg, agent, promptSessionId, cli.noSession);
        }

        // Unreachable: experimental is the only backend (validated above).
        return 1;
    }

    // ── Interactive inkcell app (product surface) ──
    // Default for --tui experimental|inkcell and for bare `cortex-mk3`
    // (no prompt, no manifest). Dashboard when no -m; agent scene with -m.
    if (cli.tuiMode == "experimental" ||
        (cli.prompt.empty() && cli.manifestPath.empty())) {
        std::string experimentalSessionId =
            cli.noSession
                ? ""
                : (activeSessionId.empty() ? resolveSessionId(cli, false) : activeSessionId);
        if (!cli.noSession && !experimentalSessionId.empty())
            persistSessionMetadata(experimentalSessionId, cli, acfg);
        ui::InkcellAppConfig icfg;
        icfg.agentName = acfg.name;
        icfg.provider = acfg.provider;
        icfg.model = acfg.model;
        icfg.manifestPath = cli.manifestPath;
        icfg.manifestDir = cli.manifestDir;
        icfg.startAtManifests = cli.manifestPickerRequested;
        icfg.harnessPath = acfg.harnessPath;
        icfg.systemPromptPath = acfg.systemPromptPath;
        icfg.personaPath = acfg.personaPath;
        icfg.sessionId = experimentalSessionId;
        icfg.toolCount = static_cast<int>(allSchemas.size());
        icfg.feedCount = static_cast<int>(agent.feedNames().size());
        icfg.relicCount = static_cast<int>(agent.relicNames().size());
        icfg.subAgentCount = static_cast<int>(agent.subAgentNames().size());
        icfg.noSession = cli.noSession;
        icfg.ephemeral = cli.ephemeral;
        icfg.showThoughts = cli.showThoughts;
        icfg.truncateBodies = cli.truncateBodies;
        return ui::runInkcellRepl(icfg, agent, experimentalSessionId, cli.noSession);
    }

    // Unreachable: inkcell is the only backend; legacy was removed.
    return 1;
}

// ═══════════════════════════════════════════════════════════════════════
// main
// ═══════════════════════════════════════════════════════════════════════

}  // namespace cortex::mk3::cli
