// =============================================================================
// agent-lib-MK3 — CLI Runner v3.1
// Subcommand-based CLI with getopt_long, config file, progress indicators.
// =============================================================================

#include <getopt.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "src/cli/list_picker.hpp"
#include "src/cli/options.hpp"
#include "src/cli/session.hpp"
#include "src/core/agent.hpp"
#include "src/core/agent_catalog.hpp"
#include "src/core/manifest_autoload.hpp"
#include "src/core/manifest_loader.hpp"
#include "src/core/sandbox_launcher.hpp"
#include "src/providers/factory.hpp"
#include "src/sandbox/policy.hpp"
#include "src/session/manager.hpp"
#include "src/session/controller.hpp"
#include "src/ui/app/mk3_tui_app.hpp"
#include "src/utils/ansi.hpp"

using namespace cortex::mk3;
using namespace cortex::mk3::cli;

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════════════
// Signal handler
// ═══════════════════════════════════════════════════════════════════════
void signalHandler(int sig) {
    if (sig != SIGWINCH)
        cortex::mk3::g_running = false;
}

// ═══════════════════════════════════════════════════════════════════════
// Command: version
// ═══════════════════════════════════════════════════════════════════════
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

        if (!cli.manifestPath.empty()) {
            std::cout << "  ✓ Manifest: " << cli.manifestPath << "\n";
        }

        std::cout << "  sandbox:  "
                  << (cli.sandbox ? (cli.sandboxReadOnly ? "read-only" : "on") : "off") << "\n";
        std::cout << "  ✓ Configuration valid. Use without --dry-run to execute.\n";
        return 0;
    }

    // ── Create agent ──
    AgentConfig acfg;
    ansi::colorEnabled() = true;

    if (!cli.manifestPath.empty()) {
        acfg = ManifestLoader::loadAgentConfig(cli.manifestPath);
        ManifestLoader::loadEnv(cli.manifestPath, acfg);
        catalog::fixDefaultPromptPaths(acfg, cli.manifestDir);
        if (cli.providerSet)
            acfg.provider = cli.provider;
        if (cli.modelSet)
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
            auto files = ManifestLoader::loadFiles(cli.manifestPath);
            return sandbox::launchDocker(cli.manifestPath, acfg, files);
        }
    } else {
        acfg.name = "cortext-builtin-agent";
        acfg.provider = cli.provider;
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

    // Sandbox
    if (cli.sandbox) {
        std::string cwd = fs::current_path().string();
        if (cli.sandboxReadOnly) {
            agent.setSandboxPolicy(sandbox::makeReadOnlySandbox(cwd));
            if (!cli.raw)
                std::cerr << "[sandbox] read-only — " << cwd << "\n";
        } else {
            agent.setSandboxPolicy(sandbox::makeHarnessSandbox(cwd));
            if (!cli.raw)
                std::cerr << "[sandbox] enabled — " << cwd << "\n";
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
// Command: serve
// ═══════════════════════════════════════════════════════════════════════
static int cmdServe(const CliConfig& cli) {
    std::cout << "Server mode — use cortex-mk3-server binary instead.\n";
    std::cout << "  cortex-mk3-server --host " << cli.serverHost << " --port " << cli.serverPort
              << " --threads " << cli.serverThreads;
    if (!cli.serverApiKey.empty())
        std::cout << " --api-key " << cli.serverApiKey;
    if (cli.serverNoCors)
        std::cout << " --no-cors";
    std::cout << "\n";
    return 1;
}

// ═══════════════════════════════════════════════════════════════════════
// main
// ═══════════════════════════════════════════════════════════════════════
int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);  // atexit still runs on default-handled SIGTERM if we catch it
    signal(SIGWINCH, signalHandler);

    try {
        CliConfig cli = parseArgs(argc, argv);

        // Load config file
        if (cli.configPath.empty())
            cli.configPath = defaultConfigPath();
        auto cfg = loadConfigFile(cli.configPath);
        applyConfig(cli, cfg);

        // Dispatch
        if (cli.showHelp)
            return cmdHelp(cli);
        if (cli.command == "version")
            return cmdVersion();
        if (cli.command == "help")
            return cmdHelp(cli);
        if (cli.command == "list")
            return cmdList(cli);
        if (cli.command == "config")
            return cmdConfig(cli);
        if (cli.command == "completions")
            return cmdCompletions(cli);
        if (cli.command == "serve")
            return cmdServe(cli);
        if (cli.command == "sessions")
            return cmdSessions(cli);

        // Default: run
        return cmdRun(cli);
    } catch (const std::exception& e) {
        // Operator-locked fail-soft entry path: a missing harness prompt or
        // a torn config file used to terminate() mid-main with a noisy
        // SIGABRT. Pretty-print the exception and exit 2 so operators can
        // see the message and continue without a core dump.
        std::cerr << "\033[31mcortex-mk3:\033[0m " << e.what() << "\n";
        // Only nudge CORTEX_HOME when the error looks path/manifest related —
        // bare std::bad_alloc / stoi noise is not fixed by env vars.
        const std::string msg = e.what();
        const bool pathish =
            msg.find("manifest") != std::string::npos ||
            msg.find("harness") != std::string::npos ||
            msg.find("CORTEX_HOME") != std::string::npos ||
            msg.find("No such file") != std::string::npos ||
            msg.find("not found") != std::string::npos;
        if (pathish) {
            if (const char* hint = std::getenv("CORTEX_HOME"); !hint || !*hint) {
                std::cerr << "  \033[2mhint\033[0m: configure with "
                             "`--manifest-dir /path/to/cortex` or "
                             "`CORTEX_HOME=/path/to/cortex`.\n";
            }
        }
        return 2;
    }
}
