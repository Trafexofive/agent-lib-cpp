#pragma once
// Non-run CLI commands: version, help, list, sessions, config, completions.

#include <iostream>
#include <string>
#include <vector>

#include "src/cli/options.hpp"
#include "src/cli/session.hpp"
#include "src/core/agent_catalog.hpp"
#include "src/providers/factory.hpp"
#include "src/session/manager.hpp"

namespace cortex::mk3::cli {

static int cmdVersion() {
    std::cout << "cortex-mk3 v" << VERSION << "\n"
              << "  Build: " << __DATE__ << " " << __TIME__ << "\n"
              << "  Standard: C++17\n"
              << "  Protocol: MK3 Agent Protocol v3\n";
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════
// Command: help
// ═══════════════════════════════════════════════════════════════════════
static int cmdHelp(const CliConfig& cli) {
    if (cli.helpCommand.empty()) {
        printHelpGeneral();
    } else if (cli.helpCommand == "run") {
        printHelpRun();
    } else if (cli.helpCommand == "serve") {
        printHelpServe();
    } else if (cli.helpCommand == "list") {
        printHelpList();
    } else if (cli.helpCommand == "config") {
        printHelpConfig();
    } else {
        std::cout << "Unknown command: " << cli.helpCommand << "\n";
        printHelpGeneral();
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════
// Command: sessions
// ═══════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════
// Command: list
// ═══════════════════════════════════════════════════════════════════════
static int cmdList(const CliConfig& cli);
static int cmdSessions(const CliConfig& cli);

static int cmdList(const CliConfig& cli) {
    if (cli.listProviders) {
        std::cout << "Available providers:\n\n";
        // Pull from the factory so the list stays in sync with availableProviders().
        static const std::vector<std::pair<std::string, std::string>> providerInfo = {
            {"deepseek", "DeepSeek API        (DEEPSEEK_API_KEY)"},
            {"openrouter", "OpenRouter          (OPENROUTER_API_KEY)"},
            {"xai", "xAI / Grok         (XAI_API_KEY, XAI_AUTH_TOKEN, or ~/.grok/auth.json)"},
            {"openai-codex", "OpenAI Codex        (OPENAI_API_KEY or ~/.codex/auth.json)"},
            {"groq", "Groq                (GROQ_API_KEY)"},
            {"zen", "OpenCode Zen        (free tier)"},
            {"opencode-go", "OpenCode Go         (OPENCODE_API_KEY, $10/mo)"},
            {"opencode", "OpenCode (alias→zen)"},
            {"together", "Together AI         (TOGETHER_API_KEY)"},
            {"fireworks", "Fireworks AI        (FIREWORKS_API_KEY)"},
            {"sambanova", "SambaNova"},
            {"cerebras", "Cerebras"},
            {"hyperbolic", "Hyperbolic"},
            {"llm7", "LLM7"},
            {"nvidia", "NVIDIA"},
        };
        // Only show providers the factory actually knows about.
        auto avail = providers::availableProviders();
        for (const auto& [name, desc] : providerInfo) {
            if (std::find(avail.begin(), avail.end(), name) != avail.end()) {
                std::cout << "  " << name;
                for (size_t i = name.size(); i < 14; ++i)
                    std::cout << ' ';
                std::cout << desc << "\n";
            }
        }
        return 0;
    }

    if (cli.listModels) {
        auto formatContextWindow = [](int tokens) -> std::string {
            if (tokens <= 0)
                return "0";
            if (tokens >= 1000000)
                return std::to_string(tokens / 1000000) + "M";
            return std::to_string(tokens / 1000) + "K";
        };

        auto printModels = [&](const std::string& p) -> bool {
            auto provider = providers::createProvider(p, "");
            if (!provider) {
                std::cerr << "Unknown provider: " << p << "\n";
                return false;
            }

            auto models = provider->listModels();
            if (p == "openrouter") {
                std::vector<ILlmProvider::ModelInfo> freeOnly;
                for (auto& m : models)
                    if (m.isFree)
                        freeOnly.push_back(m);
                models = std::move(freeOnly);
            }

            std::cout << "Models for " << p << ":\n";
            for (auto& m : models) {
                std::cout << "  " << m.id;
                if (!m.name.empty() && m.name != m.id)
                    std::cout << " (" << m.name << ")";
                if (m.isFree)
                    std::cout << " [free]";
                std::cout << " — " << formatContextWindow(m.contextWindow) << " ctx\n";
            }
            if (models.empty()) {
                std::string fallback = providers::defaultProviderModel(p);
                if (!fallback.empty()) {
                    std::cout << "  " << fallback << " (default)\n";
                    if (p == "openrouter")
                        std::cout << "  (OpenRouter listing is filtered to free models; specify "
                                     "paid model IDs in manifests.)\n";
                } else {
                    std::cout << "  (model listing not supported by this provider)\n";
                }
            }
            return true;
        };

        if (!cli.listModelsProvider.empty()) {
            return printModels(cli.listModelsProvider) ? 0 : 1;
        }

        std::cout << "Models for all providers. Use `--models <provider>` to filter.\n\n";
        for (const auto& p : {"deepseek", "openrouter", "xai", "openai-codex", "groq", "zen",
                              "together", "fireworks"}) {
            if (!printModels(p))
                return 1;
            std::cout << "\n";
        }
        return 0;
    }

    if (cli.listTools) {
        std::cout << "Built-in tools:\n\n";
        std::cout << "  exec          Run a shell command\n";
        std::cout << "  list          List directory contents\n";
        std::cout << "  grep          Search files for a pattern\n";
        std::cout << "  context_pin   Pin file to context\n";
        std::cout << "  context_peek  Read file ephemerally\n";
        std::cout << "  context_unpin Remove pinned file\n";
        std::cout << "  ask_tool      Interactive dialog (ask user)\n";
        return 0;
    }

    if (cli.listSessionsFlag) {
        return cmdSessions(cli);
    }

    if (cli.listAgents) {
        auto agents = catalog::discoverAgents(cli.manifestDir);
        if (agents.empty()) {
            std::cout << "No agents found under manifests/agents.\n";
            std::cout << "Global surface is manifests/ only. Search roots:\n";
            for (const auto& [root, source] : catalog::manifestsSearchRoots(cli.manifestDir))
                std::cout << "  [" << source << "] " << root << "\n";
            std::cout << "\nInstall:\n"
                         "  $CORTEX_HOME/manifests/agents/<name>/agent.yml\n"
                         "  ~/.config/cortex/manifests/agents/<name>/agent.yml\n"
                         "  <repo>/manifests/agents/<name>/agent.yml\n";
            return 0;
        }
        bool color = isatty(STDOUT_FILENO);
        std::cout << "Agents in manifests/ (" << agents.size() << ")\n";
        std::cout << "Legend: ✓ path ok  ·  ◆ builtin  ·  ✗ missing\n\n";
        for (size_t i = 0; i < agents.size(); ++i) {
            for (const auto& line : catalog::formatOwnershipTree(agents[i], color))
                std::cout << line << "\n";
            if (i + 1 < agents.size())
                std::cout << "\n";
        }
        return 0;
    }

    // Default: show providers (most useful; use --models/--tools for the rest)
    CliConfig showProviders = cli;
    showProviders.listProviders = true;
    return cmdList(showProviders);
}

// ═══════════════════════════════════════════════════════════════════════
// Command: sessions — list/show/rm/export
// ═══════════════════════════════════════════════════════════════════════
static int cmdSessions(const CliConfig& cli) {
    session::SessionManager sm;

    auto printTable = [](const std::vector<session::SessionManager::SessionInfo>& sessions) {
        if (sessions.empty()) {
            std::cout << "(no sessions found — run cortex-mk3 to create one)\n";
            return;
        }
        // Compute column widths.
        size_t idW = 2, agentW = 4, modelW = 5, updatedW = 7;
        for (const auto& s : sessions) {
            idW = std::max(idW, s.id.size());
            agentW = std::max(agentW, s.agentName.size());
            modelW = std::max(modelW, s.model.size());
            updatedW = std::max(updatedW, s.updated.size());
        }
        std::cout << "\033[1m" << std::left << std::setw((int)idW + 2) << "ID"
                  << std::setw((int)agentW + 2) << "AGENT" << std::setw((int)modelW + 2) << "MODEL"
                  << std::setw((int)updatedW + 2) << "UPDATED" << std::right << std::setw(7)
                  << "TURNS" << "\033[0m\n";
        for (const auto& s : sessions) {
            std::cout << std::left << std::setw((int)idW + 2) << s.id << std::setw((int)agentW + 2)
                      << s.agentName << std::setw((int)modelW + 2) << s.model
                      << std::setw((int)updatedW + 2) << s.updated << std::right << std::setw(7)
                      << s.turnCount << "\n";
        }
        std::cout << "\n"
                  << "Resume latest:  cortex-mk3 --continue\n"
                  << "Pick one:       cortex-mk3 --resume\n"
                  << "By id:          cortex-mk3 --session <id>\n"
                  << "Fork:           cortex-mk3 --fork <id>\n";
    };

    if (cli.sessionsSubcommand == "list" || cli.sessionsSubcommand.empty()) {
        printTable(sortedSessions());
        return 0;
    }

    if (cli.sessionsSubcommand == "show") {
        if (cli.sessionsTarget.empty()) {
            std::cerr << "Usage: cortex-mk3 sessions show <id>\n";
            return 1;
        }
        if (!sm.exists(cli.sessionsTarget)) {
            std::cerr << "Session not found: " << cli.sessionsTarget << "\n";
            return 1;
        }
        Session s = sm.load(cli.sessionsTarget);
        std::cout << "\033[1mSession:\033[0m " << s.id << "\n";
        if (s.metadata.count("name"))
            std::cout << "\033[1mName:\033[0m    " << s.metadata.at("name") << "\n";
        std::cout << "\033[1mAgent:\033[0m   " << s.agentName << "\n";
        std::cout << "\033[1mModel:\033[0m   " << s.provider << "/" << s.model << "\n";
        std::cout << "\033[1mCreated:\033[0m " << s.created << "\n";
        std::cout << "\033[1mUpdated:\033[0m " << s.updated << "\n";
        std::cout << "\033[1mRecords:\033[0m " << s.records.size() << "\n";
        std::cout << "\033[1mFeeds:\033[0m   " << s.contextFeeds.size() << "\n";
        if (s.metadata.count("manifest_path"))
            std::cout << "\033[1mManifest:\033[0m " << s.metadata.at("manifest_path") << "\n";
        if (s.metadata.count("forked_from"))
            std::cout << "\033[1mForked from:\033[0m " << s.metadata.at("forked_from") << "\n";
        std::cout << "\n\033[2m─── records ───\033[0m\n";
        size_t n = 0;
        for (const auto& r : s.records) {
            std::string role;
            switch (r.role) {
                case SessionRecord::USER:
                    role = "user";
                    break;
                case SessionRecord::AGENT:
                    role = "agent";
                    break;
                case SessionRecord::TOOL_CALL:
                    role = "tool";
                    break;
                case SessionRecord::TOOL_RESULT:
                    role = "result";
                    break;
                default:
                    role = "system";
                    break;
            }
            std::string content = r.content;
            // Strip a single trailing newline for readability.
            if (!content.empty() && content.back() == '\n')
                content.pop_back();
            // Truncate long content.
            if (content.size() > 200)
                content = content.substr(0, 197) + "...";
            std::cout << "[" << n++ << "] " << role << ": " << content << "\n";
        }
        return 0;
    }

    if (cli.sessionsSubcommand == "rm") {
        if (cli.sessionsTarget.empty()) {
            std::cerr << "Usage: cortex-mk3 sessions rm <id>\n";
            return 1;
        }
        if (!sm.exists(cli.sessionsTarget)) {
            std::cerr << "Session not found: " << cli.sessionsTarget << "\n";
            return 1;
        }
        sm.remove(cli.sessionsTarget);
        // Also clear the state checkpoint if present (stable root + legacy CWD).
        std::error_code ec;
        fs::path statePath =
            fs::path(session::defaultStateDir()) / (cli.sessionsTarget + ".json");
        fs::remove(statePath, ec);
        fs::path legacyState =
            fs::current_path() / ".cortex" / "state" / (cli.sessionsTarget + ".json");
        fs::remove(legacyState, ec);
        std::cout << "Removed session " << cli.sessionsTarget << "\n";
        return 0;
    }

    if (cli.sessionsSubcommand == "export") {
        if (cli.sessionsTarget.empty()) {
            std::cerr << "Usage: cortex-mk3 sessions export <id> <file>\n";
            return 1;
        }
        std::string outPath = cli.sessionsTargetArg.empty()
                                  ? (cli.sessionsTarget + ".portable.json")
                                  : cli.sessionsTargetArg;
        if (sm.exportToFile(cli.sessionsTarget, outPath)) {
            std::cout << "Exported " << cli.sessionsTarget << " to " << outPath << "\n";
            return 0;
        }
        std::cerr << "Export failed: session empty or not found\n";
        return 1;
    }

    std::cerr << "Unknown sessions subcommand: " << cli.sessionsSubcommand << "\n"
              << "Try: cortex-mk3 sessions [list|show <id>|rm <id>|export <id> <file>]\n";
    return 1;
}

// ═══════════════════════════════════════════════════════════════════════
// Command: config
// ═══════════════════════════════════════════════════════════════════════
static int cmdConfig(CliConfig& cli) {
    std::string path = cli.configPath.empty() ? defaultConfigPath() : cli.configPath;

    if (cli.configInit) {
        std::map<std::string, std::string> defaults = {
            {"provider", "openai-codex"},
            {"model", "gpt-5.5"},
        };
        saveConfigFile(path, defaults);
        std::cout << "Created config: " << path << "\n";
        return 0;
    }

    auto cfg = loadConfigFile(path);

    if (cli.configShow) {
        if (cfg.empty()) {
            std::cout << "No config found at " << path << "\n";
            std::cout << "Run 'cortex-mk3 config --init' to create one.\n";
        } else {
            std::cout << "Configuration (" << path << "):\n";
            for (auto& [k, v] : cfg) {
                std::cout << "  " << k << " = " << v << "\n";
            }
        }
        return 0;
    }

    if (!cli.configSet.empty()) {
        auto eq = cli.configSet.find('=');
        if (eq == std::string::npos) {
            std::cerr << "Usage: --set key=value\n";
            return 1;
        }
        std::string key = cli.configSet.substr(0, eq);
        std::string val = cli.configSet.substr(eq + 1);
        cfg[key] = val;
        saveConfigFile(path, cfg);
        std::cout << "Set " << key << " = " << val << " in " << path << "\n";
        return 0;
    }

    // Default: show config
    if (cfg.empty()) {
        std::cout << "No config found at " << path << "\n";
        std::cout << "Run 'cortex-mk3 config --init' to create one.\n";
    } else {
        std::cout << "Configuration (" << path << "):\n";
        for (auto& [k, v] : cfg) {
            std::cout << "  " << k << " = " << v << "\n";
        }
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════
// Command: completions
// ═══════════════════════════════════════════════════════════════════════
static int cmdCompletions(const CliConfig& cli) {
    std::string shell = cli.completionsShell;

    if (shell == "bash") {
        std::cout << R"(# cortex-mk3 bash completion
_cortex_mk3() {
    local cur prev words cword
    _init_completion || return
    case $prev in
        cortex-mk3)
            COMPREPLY=($(compgen -W "run serve list config completions version help" -- "$cur"))
            ;;
        completions)
            COMPREPLY=($(compgen -W "bash zsh fish" -- "$cur"))
            ;;
        help)
            COMPREPLY=($(compgen -W "run serve list config" -- "$cur"))
            ;;
        *)
            COMPREPLY=()
            ;;
    esac
}
complete -F _cortex_mk3 cortex-mk3
)";
    } else if (shell == "zsh") {
        std::cout << R"(#compdef cortex-mk3
_cortex_mk3() {
    local -a commands
    commands=(
        'run:Run agent'
        'serve:Start HTTP server'
        'list:List resources'
        'config:Manage configuration'
        'completions:Generate shell completions'
        'version:Show version'
        'help:Show help'
    )
    _describe 'command' commands
}
compdef _cortex_mk3 cortex-mk3
)";
    } else if (shell == "fish") {
        std::cout << R"(# cortex-mk3 fish completion
complete -c cortex-mk3 -f
complete -c cortex-mk3 -a "run serve list config completions version help"
complete -c cortex-mk3 -n "__fish_seen_subcommand_from completions" -a "bash zsh fish"
complete -c cortex-mk3 -n "__fish_seen_subcommand_from help" -a "run serve list config"
)";
    } else {
        std::cerr << "Unknown shell: " << shell << ". Use bash, zsh, or fish.\n";
        return 1;
    }
    return 0;
}


}  // namespace cortex::mk3::cli
