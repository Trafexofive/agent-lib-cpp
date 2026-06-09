// =============================================================================
// agent-lib-MK3 — CLI Runner v3.1
// Subcommand-based CLI with getopt_long, config file, progress indicators.
// =============================================================================

#include <csignal>
#include <cstdio>
#include <cstring>
#include <getopt.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <atomic>

#include "src/core/agent.hpp"
#include "src/core/manifest_loader.hpp"
#include "src/core/manifest_autoload.hpp"
#include "src/core/sandbox_launcher.hpp"
#include "src/sandbox/policy.hpp"
#include "src/providers/factory.hpp"
#include "src/utils/ansi.hpp"
#include "src/tui/renderer.hpp"
#include "src/tui/input.hpp"
#include "src/tui/slash_commands.hpp"
#include "src/session/manager.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>

using namespace cortex::mk3;

static volatile bool g_resized = false;

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════════════
// Version
// ═══════════════════════════════════════════════════════════════════════
static const char* VERSION = "3.1.0";

// ═══════════════════════════════════════════════════════════════════════
// CLI state
// ═══════════════════════════════════════════════════════════════════════
struct CliConfig {
    // Subcommand
    std::string command;  // "run", "serve", "list", "config", "completions", "version", "help"

    // Provider
    std::string provider = "deepseek";
    std::string model = "deepseek-v4-pro";
    bool providerSet = false;
    bool modelSet = false;

    // Run mode
    std::string prompt;
    std::string promptFile;
    std::string manifestPath;
    std::string manifestDir;
    std::string sessionId;
    std::string systemPromptPath;
    std::string harnessPromptPath;
    bool ephemeral = false;
    bool raw = false;
    bool replMode = false;

    // Debug
    bool debug = false;
    bool verbose = false;

    // Sandbox
    bool sandbox = false;
    bool sandboxReadOnly = false;

    // Server
    std::string serverHost = "0.0.0.0";
    int serverPort = 8080;
    int serverThreads = 4;
    std::string serverApiKey;
    bool serverNoCors = false;

    // Config
    std::string configPath;
    bool configShow = false;
    std::string configSet;
    bool configInit = false;

    // List
    bool listProviders = false;
    bool listModels = false;
    std::string listModelsProvider;
    bool listTools = false;

    // Completions
    std::string completionsShell;

    // Dry run
    bool dryRun = false;

    // Help
    bool showHelp = false;
    std::string helpCommand;
};

// ═══════════════════════════════════════════════════════════════════════
// Progress spinner
// ═══════════════════════════════════════════════════════════════════════
class Spinner {
public:
    void start(const std::string& msg) {
        if (running_) return;
        running_ = true;
        thread_ = std::thread([this, msg]() {
            const char* frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
            int i = 0;
            while (running_) {
                std::cerr << "\r" << frames[i % 10] << " " << msg << std::flush;
                i++;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            std::cerr << "\r" << std::string(msg.size() + 3, ' ') << "\r" << std::flush;
        });
    }
    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }
    ~Spinner() { stop(); }
private:
    std::atomic<bool> running_{false};
    std::thread thread_;
};

// ═══════════════════════════════════════════════════════════════════════
// Signal handler
// ═══════════════════════════════════════════════════════════════════════
void signalHandler(int sig) {
    if (sig == SIGWINCH) g_resized = true;
    else cortex::mk3::g_running = false;
}

// ═══════════════════════════════════════════════════════════════════════
// Help
// ═══════════════════════════════════════════════════════════════════════
void printBanner() {
    std::cout << ansi::bold << ansi::cyan
              << "  Cortex MK3 " << VERSION << " — Agent Runtime\n"
              << ansi::reset;
}

void printHelpGeneral() {
    std::cout << R"(Usage: cortex-mk3 [global-flags] <command> [command-flags]

Global flags:
  --config <path>      Config file (default: ~/.config/cortex-mk3/config)
  --manifest-dir <dir> Autoload manifest root (default: ./manifests)
  --provider <name>    LLM provider (deepseek, openrouter, groq, zen, together, fireworks)
  --model <name>       Model name
  --sandbox            Enable sandbox mode (tool restrictions)
  --sandbox-ro         Read-only sandbox (no writes, restricted exec)
  --verbose, -V        Verbose: dump full prompts each iteration
  --debug              Enable debug output
  --raw                Pipe-clean output (no formatting, no banner)
  --dry-run            Validate config + prompt without calling LLM
  --help               Show this help

Commands:
  run                  Run agent (default if no command given)
  serve                Start HTTP server
  list                 List providers, models, tools
  config               Show, set, or init configuration
  completions <shell>  Generate shell completions (bash, zsh, fish)
  version              Show version
  help [command]       Show help for a command

Examples:
  cortex-mk3 run -p "List files"
  cortex-mk3 --provider groq run -p "Search for TODOs"
  cortex-mk3 --sandbox run -p "Run make" --harness config/harness/default.md
  cortex-mk3 serve --port 9090 --api-key my-secret
  cortex-mk3 list --providers
  cortex-mk3 list --models deepseek
  cortex-mk3 completions bash > /etc/bash_completion.d/cortex-mk3
)";
}

void printHelpRun() {
    std::cout << R"(Usage: cortex-mk3 run [flags]

Flags:
  -p, --prompt <text>    One-shot prompt
  -f, --file <path>      Read prompt from file
  -m, --manifest <path>  Agent manifest YAML (recursive imports + global scope)
  --harness <path>       Harness prompt (XML protocol spec)
  --system <path>        System prompt override
  --session <id>         Session ID for persistence
  --ephemeral            Don't save session
  --repl                 Force interactive mode even with --prompt
)";
}

void printHelpServe() {
    std::cout << R"(Usage: cortex-mk3 serve [flags]

Flags:
  --host <addr>     Bind address (default: 0.0.0.0)
  --port <port>     Port (default: 8080)
  --threads <n>     Thread pool size (default: 4)
  --api-key <key>   Bearer token for auth
  --no-cors         Disable CORS
)";
}

void printHelpList() {
    std::cout << R"(Usage: cortex-mk3 list [flags]

Flags:
  --providers        List available providers
  --models [name]    List models for a provider (or all)
  --tools            List available tools
)";
}

void printHelpConfig() {
    std::cout << R"(Usage: cortex-mk3 config [flags]

Flags:
  --show             Show current configuration
  --set key=value    Set a configuration value
  --init             Create default config file
)";
}

// ═══════════════════════════════════════════════════════════════════════
// Config file
// ═══════════════════════════════════════════════════════════════════════
static std::string defaultConfigPath() {
    const char* home = getenv("HOME");
    if (!home) return "";
    return std::string(home) + "/.config/cortex-mk3/config";
}

static std::map<std::string, std::string> loadConfigFile(const std::string& path) {
    std::map<std::string, std::string> cfg;
    std::ifstream f(path);
    if (!f.good()) return cfg;
    std::string line;
    while (std::getline(f, line)) {
        // Skip comments and blanks
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // Trim
        while (!key.empty() && key.back() == ' ') key.pop_back();
        while (!val.empty() && val.front() == ' ') val.erase(0, 1);
        cfg[key] = val;
    }
    return cfg;
}

static void saveConfigFile(const std::string& path, const std::map<std::string, std::string>& cfg) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream f(path);
    f << "# Cortex MK3 Configuration\n";
    for (auto& [k, v] : cfg) {
        f << k << "=" << v << "\n";
    }
}

static void applyConfig(CliConfig& cli, const std::map<std::string, std::string>& cfg) {
    auto get = [&](const std::string& k, const std::string& d) -> std::string {
        auto it = cfg.find(k);
        return it != cfg.end() ? it->second : d;
    };
    if (!cli.providerSet) cli.provider = get("provider", cli.provider);
    if (!cli.modelSet) cli.model = get("model", cli.model);
    if (cli.systemPromptPath.empty()) cli.systemPromptPath = get("system_prompt", cli.systemPromptPath);
    if (cli.manifestDir.empty()) cli.manifestDir = get("manifest_dir", cli.manifestDir);
    if (cli.configPath.empty()) cli.configPath = get("config_path", defaultConfigPath());
}

// ═══════════════════════════════════════════════════════════════════════
// Argument parsing
// ═══════════════════════════════════════════════════════════════════════
static CliConfig parseArgs(int argc, char* argv[]) {
    CliConfig cli;

    // Long options
    static struct option longOpts[] = {
        // Global
        {"config",       required_argument, 0, 'C'},
        {"manifest-dir", required_argument, 0, 'G'},
        {"provider",     required_argument, 0, 'P'},
        {"model",        required_argument, 0, 'M'},
        {"sandbox",      no_argument,       0, 'S'},
        {"sandbox-ro",   no_argument,       0, 'R'},
        {"verbose",      no_argument,       0, 'V'},
        {"debug",        no_argument,       0, 'D'},
        {"raw",          no_argument,       0, 'r'},
        {"dry-run",      no_argument,       0, 'n'},
        {"help",         no_argument,       0, 'h'},

        // Run
        {"prompt",       required_argument, 0, 'p'},
        {"file",         required_argument, 0, 'f'},
        {"manifest",     required_argument, 0, 'm'},
        {"harness",      required_argument, 0, 'H'},
        {"system",       required_argument, 0, 'y'},
        {"session",      required_argument, 0, 's'},
        {"ephemeral",    no_argument,       0, 'e'},
        {"repl",         no_argument,       0, 'E'},

        // Serve
        {"host",         required_argument, 0, 'o'},
        {"port",         required_argument, 0, 'O'},
        {"threads",      required_argument, 0, 'T'},
        {"api-key",      required_argument, 0, 'K'},
        {"no-cors",      no_argument,       0, 'N'},

        // List
        {"providers",    no_argument,       0, 'L'},
        {"models",       optional_argument, 0, 'l'},
        {"tools",        no_argument,       0, 't'},

        // Config
        {"show",         no_argument,       0, 'w'},
        {"set",          required_argument, 0, 'W'},
        {"init",         no_argument,       0, 'I'},

        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "C:G:P:M:p:f:m:H:y:s:VhrSReEDn", longOpts, nullptr)) != -1) {
        switch (opt) {
        // Global
        case 'C': cli.configPath = optarg; break;
        case 'G': cli.manifestDir = optarg; break;
        case 'P': cli.provider = optarg; cli.providerSet = true; break;
        case 'M': cli.model = optarg; cli.modelSet = true; break;
        case 'S': cli.sandbox = true; break;
        case 'R': cli.sandbox = true; cli.sandboxReadOnly = true; break;
        case 'V': cli.verbose = true; break;
        case 'D': cli.debug = true; break;
        case 'r': cli.raw = true; break;
        case 'n': cli.dryRun = true; break;
        case 'h': cli.showHelp = true; break;

        // Run
        case 'p': cli.prompt = optarg; break;
        case 'f': cli.promptFile = optarg; break;
        case 'm': cli.manifestPath = optarg; break;
        case 'H': cli.harnessPromptPath = optarg; break;
        case 'y': cli.systemPromptPath = optarg; break;
        case 's': cli.sessionId = optarg; break;
        case 'e': cli.ephemeral = true; break;
        case 'E': cli.replMode = true; break;

        // Serve
        case 'o': cli.serverHost = optarg; break;
        case 'O': cli.serverPort = std::stoi(optarg); break;
        case 'T': cli.serverThreads = std::stoi(optarg); break;
        case 'K': cli.serverApiKey = optarg; break;
        case 'N': cli.serverNoCors = true; break;

        // List
        case 'L': cli.listProviders = true; break;
        case 'l': cli.listModels = true; if (optarg) cli.listModelsProvider = optarg; break;
        case 't': cli.listTools = true; break;

        // Config
        case 'w': cli.configShow = true; break;
        case 'W': cli.configSet = optarg; break;
        case 'I': cli.configInit = true; break;

        default:
            std::cerr << "Try 'cortex-mk3 --help'\n";
            exit(1);
        }
    }

    // Subcommand (first positional after flags)
    if (optind < argc) {
        std::string cmd = argv[optind];
        if (cmd == "run" || cmd == "serve" || cmd == "list" ||
            cmd == "config" || cmd == "completions" ||
            cmd == "version" || cmd == "help") {
            cli.command = cmd;
            optind++;
        } else {
            // Unknown command — treat as prompt for backward compat
            cli.prompt = cmd;
            for (int i = optind; i < argc; i++) {
                if (!cli.prompt.empty()) cli.prompt += " ";
                cli.prompt += argv[i];
            }
        }
    }

    // Completions subcommand takes shell name
    if (cli.command == "completions" && optind < argc) {
        cli.completionsShell = argv[optind];
    }

    // Help subcommand takes optional command name
    if (cli.command == "help" && optind < argc) {
        cli.helpCommand = argv[optind];
    }

    // Default command
    if (cli.command.empty()) cli.command = "run";

    return cli;
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
// Command: list
// ═══════════════════════════════════════════════════════════════════════
static int cmdList(const CliConfig& cli) {
    if (cli.listProviders) {
        std::cout << "Available providers:\n\n";
        std::cout << "  deepseek    DeepSeek API        (DEEPSEEK_API_KEY)\n";
        std::cout << "  openrouter  OpenRouter          (OPENROUTER_API_KEY)\n";
        std::cout << "  groq        Groq                (GROQ_API_KEY)\n";
        std::cout << "  zen         OpenCode Zen        (free tier)\n";
        std::cout << "  together    Together AI         (TOGETHER_API_KEY)\n";
        std::cout << "  fireworks   Fireworks AI        (FIREWORKS_API_KEY)\n";
        return 0;
    }

    if (cli.listModels) {
        std::string p = cli.listModelsProvider.empty() ? cli.provider : cli.listModelsProvider;
        auto provider = providers::createProvider(p, "");
        if (!provider) {
            std::cerr << "Unknown provider: " << p << "\n";
            return 1;
        }
        auto models = provider->listModels();
        std::cout << "Models for " << p << ":\n";
        for (auto& m : models) {
            std::cout << "  " << m.id;
            if (!m.name.empty() && m.name != m.id) std::cout << " (" << m.name << ")";
            if (m.isFree) std::cout << " [free]";
            std::cout << " — " << (m.contextWindow / 1024) << "K ctx\n";
        }
        if (models.empty()) {
            std::cout << "  (model listing not supported by this provider)\n";
        }
        return 0;
    }

    if (cli.listTools) {
        std::cout << "Built-in tools:\n\n";
        std::cout << "  exec          Run a shell command\n";
        std::cout << "  list          List directory contents\n";
        std::cout << "  grep          Search files for a pattern\n";
        std::cout << "  fs_read       Read a file\n";
        std::cout << "  fs_write      Write a file\n";
        std::cout << "  json          Validate/transform JSON\n";
        std::cout << "  web_fetch     Fetch a URL\n";
        std::cout << "  context_pin   Pin file to context\n";
        std::cout << "  context_peek  Read file ephemerally\n";
        std::cout << "  context_unpin Remove pinned file\n";
        std::cout << "  ask_tool      Interactive confirmation dialog\n";
        return 0;
    }

    // Default: show everything
    std::cout << "Use --providers, --models [name], or --tools\n";
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════
// Command: config
// ═══════════════════════════════════════════════════════════════════════
static int cmdConfig(CliConfig& cli) {
    std::string path = cli.configPath.empty() ? defaultConfigPath() : cli.configPath;

    if (cli.configInit) {
        std::map<std::string, std::string> defaults = {
            {"provider", "deepseek"},
            {"model", "deepseek-v4-pro"},
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

// ═══════════════════════════════════════════════════════════════════════
// Command: run (agent execution)
// ═══════════════════════════════════════════════════════════════════════
static int cmdRun(CliConfig& cli) {
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

    // Dry run: validate and exit
    if (cli.dryRun) {
        std::cout << "[dry-run] Validating configuration...\n";
        std::cout << "  provider: " << cli.provider << "\n";
        std::cout << "  model:    " << cli.model << "\n";

        auto provider = providers::createProvider(cli.provider, cli.model);
        if (!provider) {
            std::cerr << "  ✗ Invalid provider: " << cli.provider << "\n";
            return 1;
        }
        std::cout << "  ✓ Provider resolved\n";

        if (!cli.harnessPromptPath.empty()) {
            std::ifstream f(cli.harnessPromptPath);
            if (!f.good()) {
                std::cerr << "  ✗ Harness prompt not found: " << cli.harnessPromptPath << "\n";
                return 1;
            }
            std::cout << "  ✓ Harness prompt: " << cli.harnessPromptPath << "\n";
        }

        if (!cli.systemPromptPath.empty()) {
            std::ifstream f(cli.systemPromptPath);
            if (!f.good() && cli.manifestPath.empty()) {
                std::cerr << "  ✗ System prompt not found: " << cli.systemPromptPath << "\n";
                return 1;
            }
            std::cout << "  ✓ System prompt: " << cli.systemPromptPath << "\n";
        }

        if (!cli.manifestPath.empty()) {
            std::ifstream f(cli.manifestPath);
            if (!f.good()) {
                std::cerr << "  ✗ Manifest not found: " << cli.manifestPath << "\n";
                return 1;
            }
            std::cout << "  ✓ Manifest: " << cli.manifestPath << "\n";
        }

        std::cout << "  sandbox:  " << (cli.sandbox ? (cli.sandboxReadOnly ? "read-only" : "on") : "off") << "\n";
        std::cout << "  ✓ Configuration valid. Use without --dry-run to execute.\n";
        return 0;
    }

    // ── Create agent ──
    AgentConfig acfg;
    ansi::colorEnabled() = true;

    if (!cli.manifestPath.empty()) {
        acfg = ManifestLoader::loadAgentConfig(cli.manifestPath);
        ManifestLoader::loadConfigOverrides(cli.manifestPath, acfg);
        ManifestLoader::loadEnv(cli.manifestPath, acfg);
        if (cli.providerSet) acfg.provider = cli.provider;
        if (cli.modelSet) acfg.model = cli.model;
        if (!cli.harnessPromptPath.empty()) acfg.harnessPath = cli.harnessPromptPath;

        if (acfg.sandboxMode == "docker" && !fs::exists("/.dockerenv")) {
            auto files = ManifestLoader::loadFiles(cli.manifestPath);
            return sandbox::launchDocker(cli.manifestPath, acfg, files);
        }
    } else {
        acfg.name = "mk3-agent";
        acfg.provider = cli.provider;
        acfg.model = cli.model;
        if (!cli.harnessPromptPath.empty()) {
            acfg.harnessPath = cli.harnessPromptPath;
        }
        if (!cli.systemPromptPath.empty()) {
            acfg.systemPromptPath = cli.systemPromptPath;
        } else {
            acfg.systemPromptPath = "config/agents/assistant/system-prompts/assistant.md";
        }
    }

    auto provider = providers::createProvider(acfg.provider, acfg.model);
    if (!provider) {
        std::cerr << "Error: unknown provider '" << acfg.provider << "'\n";
        std::cerr << "Run 'cortex-mk3 list --providers' to see available providers.\n";
        return 1;
    }

    Agent agent(acfg, provider);
    tui::TuiRenderer renderer(80);

    // Manifest global scope: autoload ./manifests by default (or --manifest-dir/config manifest_dir).
    // Explicit --manifest then layers its recursive imports on top.
    std::vector<ToolSchema> allSchemas;
    std::string workflowXml;
    std::string manifestRoot = cli.manifestDir.empty() ? "manifests" : cli.manifestDir;
    if (fs::exists(manifestRoot)) {
        auto global = ManifestAutoload::loadGlobal(manifestRoot, agent, acfg.provider, cli.manifestPath);
        allSchemas.insert(allSchemas.end(), global.toolSchemas.begin(), global.toolSchemas.end());
        workflowXml += global.workflowXml;
        if (cli.verbose && !cli.raw) {
            std::cerr << "[manifest] autoload " << manifestRoot
                      << " tools=" << global.tools.size()
                      << " feeds=" << global.feeds.size()
                      << " relics=" << global.relics.size()
                      << " agents=" << global.agents.size()
                      << " workflows=" << global.workflows.size() << "\n";
        }
    }

    if (!cli.manifestPath.empty()) {
        ManifestLoader::loadFeeds(cli.manifestPath, agent);
        ManifestLoader::loadRelics(cli.manifestPath, agent);
        auto schemas = ManifestLoader::loadTools(cli.manifestPath, agent);
        allSchemas.insert(allSchemas.end(), schemas.begin(), schemas.end());
        ManifestLoader::loadSubAgents(cli.manifestPath, agent, acfg.provider);
        workflowXml += ManifestLoader::loadWorkflows(cli.manifestPath);
    }

    std::string schemaXml = ManifestLoader::toolSchemasToXml(allSchemas);
    if (!schemaXml.empty()) agent.setEnv("__TOOL_SCHEMAS__", schemaXml);
    if (!workflowXml.empty()) agent.setEnv("__WORKFLOW_XML__", workflowXml);

    if (cli.debug) agent.setEnv("__DEBUG_MODE__", "true");
    if (cli.raw) agent.setRaw(true);
    if (cli.verbose) agent.setVerbose(true);

    // Sandbox
    if (cli.sandbox) {
        std::string cwd = fs::current_path().string();
        if (cli.sandboxReadOnly) {
            agent.setSandboxPolicy(sandbox::makeReadOnlySandbox(cwd));
            if (!cli.raw) std::cerr << "[sandbox] read-only — " << cwd << "\n";
        } else {
            agent.setSandboxPolicy(sandbox::makeHarnessSandbox(cwd));
            if (!cli.raw) std::cerr << "[sandbox] enabled — " << cwd << "\n";
        }
    }

    // ── One-shot mode ──
    if (!cli.prompt.empty() && !cli.replMode) {
        Spinner spinner;
        if (!cli.raw) {
            printBanner();
            spinner.start("Thinking...");
        }

        std::string result = agent.prompt(cli.prompt, cli.sessionId, cli.ephemeral);
        spinner.stop();

        if (!cli.raw) {
            tui::Markdown md;
            md.setText(result);
            for (auto& l : md.render()) std::cout << l << std::endl;
        } else {
            std::cout << result << std::endl;
        }
        return 0;
    }

    // ── Enter alternate screen ──
    std::cout << "\033[?1049h\033[?25l" << std::flush;
    atexit([]{ std::cout << "\033[?1049l\033[?25h" << std::flush; });

    int termW = 80, termH = 24;
    struct winsize ws; if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        termW = ws.ws_col; termH = ws.ws_row;
    } else {
        const char* ec = getenv("COLUMNS"); if (ec) termW = std::stoi(ec);
        const char* er = getenv("LINES");   if (er) termH = std::stoi(er);
    }
    renderer.setWidth(termW);

    // Bottom-up layout: output anchors to bottom, above status/input bars
    tui::Input input;
    std::vector<std::string> historyLines;
    int scrollOffset = 0;  // lines scrolled above viewport
    bool showPrompts = false;  // /prompts toggle
    auto lastRenderTime = std::chrono::steady_clock::now();
    bool streaming = false;  // true during LLM call, false when idle
    int spinnerFrame = 0;  // animated spinner
    std::chrono::steady_clock::time_point streamStart_;  // for TTC
    static const char* spinnerFrames[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
    std::vector<std::string> prevDisplay;  // last rendered lines (for diff)
    int prevDisplaySize_ = 0;  // total display size of last frame
    auto renderScreen = [&]() {
        // Throttle: max ~120fps during streaming. No throttle when idle.
        if (streaming) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRenderTime).count();
            if (elapsed < 8) return;
            lastRenderTime = now;
            spinnerFrame = (spinnerFrame + 1) % 10;
        }

        std::vector<std::string> display = historyLines;
        if (showPrompts) {
            auto& prompts = agent.iterationPrompts();
            if (prompts.empty()) {
                display.push_back("\033[2m(no prompts captured — run a prompt first)\033[0m");
            } else {
                for (size_t i = 0; i < prompts.size(); i++) {
                    display.push_back(std::string("\033[1m") + tui::ansi::fg(200,200,100) + "─── Iter " + std::to_string(i+1) + " ───\033[0m");
                    std::istringstream ps(prompts[i]);
                    std::string pline;
                    while (std::getline(ps, pline)) {
                        display.push_back(std::string("\033[2m") + pline + "\033[0m");
                    }
                    display.push_back("");
                }
            }
        } else {
            auto lines = renderer.render();
            display.insert(display.end(), lines.begin(), lines.end());
        }
        // Viewport: which lines should be visible
        int overflow = std::max(0, (int)display.size() - (termH - 2));
        if (scrollOffset < 0) scrollOffset = 0;
        if (scrollOffset > overflow) scrollOffset = overflow;
        int startRow = termH - 2 - (int)display.size() + 1 + scrollOffset;
        int skip = 0;
        if (startRow < 1) { skip = 1 - startRow; startRow = 1; }
        int visibleCount = std::min((int)display.size() - skip, termH - 2);

        // Extract visible slice for diff comparison
        std::vector<std::string> visible;
        for (int i = skip; i < skip + visibleCount && i < (int)display.size(); i++)
            visible.push_back(display[i]);

        // Diff: find first and last changed lines
        int firstChange = -1, lastChange = -1;
        int prevSize = (int)prevDisplay.size();
        int visSize = (int)visible.size();
        int cmpMax = std::max(prevSize, visSize);
        for (int i = 0; i < cmpMax; i++) {
            std::string oldLine = i < prevSize ? prevDisplay[i] : "";
            std::string newLine = i < visSize ? visible[i] : "";
            if (oldLine != newLine) {
                if (firstChange == -1) firstChange = i;
                lastChange = i;
            }
        }

        bool needsFull = prevDisplay.empty() || (int)display.size() != prevDisplaySize_;

        if (needsFull) {
            std::cout << "\033[H\033[J";
            for (int i = skip; i < skip + visibleCount && i < (int)display.size(); i++)
                std::cout << "\033[" << (startRow + i - skip) << ";1H\033[2K" << display[i];
        } else if (firstChange >= 0) {
            // Only redraw changed region — absolute cursor positioning, no bare \n
            int screenRow = startRow + firstChange;
            for (int vi = firstChange; vi <= lastChange && vi < visSize; vi++) {
                std::cout << "\033[" << (screenRow + vi - firstChange)
                          << ";1H\033[2K" << visible[vi];
            }
            // Clear trailing lines if new content is shorter
            if (visSize < prevSize) {
                for (int vi = visSize; vi < prevSize; vi++) {
                    std::cout << "\033[" << (screenRow + vi - firstChange)
                              << ";1H\033[2K";
                }
            }
        } else if (visSize != prevSize) {
            // No content changes but size changed — new lines added or removed
            if (visSize > prevSize) {
                // New lines added at bottom
                for (int vi = prevSize; vi < visSize; vi++)
                    std::cout << "\033[" << (startRow + vi) << ";1H\033[2K" << visible[vi];
            } else {
                // Lines removed — clear trailing area
                for (int vi = visSize; vi < prevSize; vi++)
                    std::cout << "\033[" << (startRow + vi) << ";1H\033[2K";
            }
        }

        prevDisplay = visible;
        prevDisplaySize_ = (int)display.size();

        // Status bar + input line (always redraw — cost is negligible)
        int displaySize = (int)display.size() - skip;
        int visibleLines = std::min(displaySize, termH - 2);
        int scrollPct = displaySize > visibleLines ? (scrollOffset * 100 / (displaySize - visibleLines)) : 100;
        std::string spinner = streaming ? std::string("\033[38;2;255;200;50m") + spinnerFrames[spinnerFrame] + " \033[0m" : "";
        std::string ttc;
        if (streaming) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - streamStart_).count();
            if (elapsed >= 1000)
                ttc = std::to_string(elapsed / 1000) + "." + std::to_string((elapsed % 1000) / 100) + "s ";
            else if (elapsed >= 100)
                ttc = "0." + std::to_string(elapsed / 100) + "s ";
        }
        std::cout << "\033[" << termH-1 << ";1H\033[2K"
                  << spinner
                  << ttc
                  << ansi::dim << "─── Mode: " << tui::TuiRenderer::modeName(renderer.mode())
                  << (showPrompts ? " + PROMPTS" : "")
                  << (displaySize > visibleLines ? " · " + std::to_string(scrollPct) + "%" : "")
                  << " ───" << ansi::reset;
        std::cout << "\033[" << termH << ";1H\033[2K"
                  << ansi::bold << "▸ " << ansi::reset << "\033[2m\033[3m";
        if (input.searching()) {
            std::cout << tui::ansi::fg(255, 200, 0) << input.searchLine();
        } else {
            size_t cp = input.cursorPos();
            std::string l = input.line();
            std::cout << l.substr(0, cp);
            std::cout << "\033[7m" << (cp < l.size() ? std::string(1, l[cp]) : " ") << "\033[27m";
            if (cp < l.size()) std::cout << l.substr(cp + 1);
        }
        std::cout << ansi::reset << " ";
        std::cout << std::flush;
    };

    std::string cmd;
    bool quit = false;
    session::SessionManager sm;
    std::string sessionId = cli.sessionId.empty() ? "default" : cli.sessionId;
    auto sess = sm.exists(sessionId) ? sm.load(sessionId) : sm.create(sessionId, "mk3", cli.model, cli.provider);
    input.start([&](const std::string& s) { cmd = s; });

    // Load history
    const char* home = getenv("HOME");
    std::string histPath = home ? std::string(home) + "/.mk3_history" : "/tmp/.mk3_history";
    input.history().load(histPath);
    input.setCompleter([](const std::string& prefix) { return tui::SlashCommands::complete(prefix); });
    input.scrollUp   = [&]{ scrollOffset += (termH - 2) / 2; renderScreen(); };
    input.scrollDown = [&]{ scrollOffset -= (termH - 2) / 2; renderScreen(); };
    input.clearScreen = [&]{ std::cout << "\033[2J\033[H" << std::flush; renderScreen(); };

    renderScreen();

    while (cortex::mk3::g_running && !quit) {
        // Handle window resize
        if (g_resized) {
            g_resized = false;
            struct winsize ws;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
                termW = ws.ws_col; termH = ws.ws_row;
                renderer.setWidth(termW);
            }
        }
        cmd.clear();
        while (cmd.empty() && cortex::mk3::g_running) {
            std::string before = input.line();
            size_t beforeCp = input.cursorPos();
            input.poll();
            if (input.line() != before || input.cursorPos() != beforeCp) renderScreen();
        }
        if (!cortex::mk3::g_running || cmd.empty()) continue;

        if (cmd == "/quit" || cmd == "/exit") { quit = true; break; }
        if (cmd[0] == '/') {
            if (cmd == "/help") {
                for (auto& l : tui::SlashCommands::helpLines())
                    historyLines.push_back(std::string("\033[2m\033[3m") + l + ansi::reset);
            }
            else if (cmd == "/prompts") {
                showPrompts = !showPrompts;
                historyLines.clear();
            }
            else if (cmd == "/cp-all") {
                std::string all;
                for (auto& l : historyLines) all += l + "\n";
                auto rl = renderer.render();
                for (auto& l : rl) all += l + "\n";
                // Try both clipboard tools
                int rc = system("which wl-copy >/dev/null 2>&1 && wl-copy");
                if (rc != 0) rc = system("which xclip >/dev/null 2>&1 && xclip -selection clipboard");
                if (rc != 0) {
                    // Fallback: write to temp file
                    std::ofstream f("/tmp/mk3-cp-all.txt");
                    if (f) { f << all; f.close(); }
                } else {
                    FILE* p = popen(rc == 0 ? "wl-copy" : "xclip -selection clipboard", "w");
                    if (p) { fwrite(all.c_str(), 1, all.size(), p); pclose(p); }
                }
            }
            else if (cmd == "/cp-raw") {
                std::string raw = agent.rawLlOutput();
                int rc = system("which wl-copy >/dev/null 2>&1");
                FILE* p = popen(rc == 0 ? "wl-copy" : "xclip -selection clipboard", "w");
                if (p) { fwrite(raw.c_str(), 1, raw.size(), p); pclose(p); }
                else { std::ofstream f("/tmp/mk3-cp-raw.txt"); if (f) { f << raw; } }
            }
            else if (cmd == "/sessions") {
                auto list = sm.list();
                historyLines.push_back(std::string("\033[2m\033[3m") + "─── Sessions ───" + ansi::reset);
                for (auto& s : list)
                    historyLines.push_back(std::string("\033[2m\033[3m") + s.id + "  " + s.updated + "  " + std::to_string(s.turnCount) + " turns" + ansi::reset);
            }
            else if (cmd == "/dump-prompt" || cmd == "/dp") {
                // Export last prompt to /tmp/mk3-prompt-iterN.xml for inspection
                auto& prompts = agent.iterationPrompts();
                if (prompts.empty()) {
                    historyLines.push_back("\033[2m(no prompts captured — run a prompt first)\033[0m");
                } else {
                    for (size_t i = 0; i < prompts.size(); i++) {
                        std::string path = "/tmp/mk3-prompt-iter" + std::to_string(i+1) + ".xml";
                        std::ofstream f(path);
                        f << "<!-- Cortex MK3 Prompt — Iteration " << (i+1) << " -->\n";
                        f << prompts[i];
                        historyLines.push_back(std::string("\033[2m\033[3m") + "Wrote " + path + " (" + std::to_string(prompts[i].size()) + " bytes)" + ansi::reset);
                    }
                }
            }
            renderScreen();
            continue;
        }

        // ── Prompt (threaded — allows concurrent input + Escape cancel) ──
        cortex::mk3::g_running = true;
        streaming = true;
        streamStart_ = std::chrono::steady_clock::now();
        input.clearEscape();

        // Echo user prompt in history BEFORE streaming (visible during response)
        historyLines.push_back(std::string(ansi::bold) + "▸ " + cmd + ansi::reset);
        scrollOffset = 0;
        renderScreen();

        size_t lastAct = 0, lastRes = 0;
        bool firstToken = true;
        bool agentDone = false;
        std::mutex renderMtx;

        // Run agent in background thread
        std::thread agentThread([&]() {
            bool thinkingPhase = true;  // true until first action/response
            agent.prompt(cmd, [&](const std::string& /*token*/, bool) {
                if (!cortex::mk3::g_running) return;
                std::lock_guard<std::mutex> lk(renderMtx);
                if (firstToken) { renderer.clear(); firstToken = false; }
                agent.harvestPendingTools();
                auto& acts = agent.protocolActions();
                auto& ress = agent.protocolResults();
                if (!acts.empty() || !agent.responseOutput().empty()) thinkingPhase = false;
                while (lastAct < acts.size()) {
                    auto& a = acts[lastAct++];
                    renderer.addProtocolAction(a.type, a.name, a.id, a.body, a.sync);
                }
                while (lastRes < ress.size()) {
                    auto& r = ress[lastRes++];
                    std::string tn;
                    for (auto& a : agent.protocolActions())
                        if (a.id == r.id) { tn = a.name; break; }
                    renderer.addProtocolResult(r.id, r.ok, r.summary, tn, r.exitCode, r.elapsedMs, r.outputBytes);
                }
                renderer.setRawStream(agent.rawLlOutput());
                renderer.setResponse(agent.responseOutput());
                if (thinkingPhase) renderer.setThought(agent.thoughtOutput());
            }, cli.sessionId, cli.ephemeral);
            std::lock_guard<std::mutex> lk(renderMtx);
            agentDone = true;
        });

        // Main loop: poll input + render concurrently with agent
        while (!agentDone && cortex::mk3::g_running && !quit) {
            // Poll input (non-blocking: VMIN=0, VTIME=1 → ~100ms max wait)
            bool hadInput = input.poll();

            // Escape → cancel agent
            if (input.escapePressed()) {
                cortex::mk3::g_running = false;
                input.clearEscape();
            }

            // Ctrl-D / Ctrl-C during streaming → quit or cancel
            if (!cmd.empty() && (cmd == "/exit" || cmd == "/quit")) {
                cortex::mk3::g_running = false;
                quit = true;
            }

            // Handle window resize
            if (g_resized) {
                g_resized = false;
                struct winsize ws;
                if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
                    std::lock_guard<std::mutex> lk(renderMtx);
                    termW = ws.ws_col; termH = ws.ws_row;
                    renderer.setWidth(termW);
                }
            }

            // Render at display rate (no artificial throttle — diff is cheap)
            {
                std::lock_guard<std::mutex> lk(renderMtx);
                renderScreen();
            }

            // Small sleep to avoid busy-wait CPU spin (120Hz ceiling)
            if (!hadInput) usleep(8000);
        }

        // Wait for agent thread to finish
        if (agentThread.joinable()) agentThread.join();

        // Flush final render state
        {
            std::lock_guard<std::mutex> lk(renderMtx);
            renderScreen();
        }

        if (!cortex::mk3::g_running) {
            streaming = false;
            std::cout << "\033[" << termH-1 << ";1H\033[2K" << ansi::red << "Cancelled" << ansi::reset;
            std::cout << "\033[" << termH << ";1H\033[2K" << ansi::bold << "▸ " << ansi::reset << "\033[2m\033[3m";
            size_t cp = input.cursorPos(); std::string l = input.line();
            std::cout << l.substr(0, cp);
            std::cout << "\033[7m" << (cp < l.size() ? std::string(1, l[cp]) : " ") << "\033[27m";
            if (cp < l.size()) std::cout << l.substr(cp + 1);
            std::cout << ansi::reset << " ";
            std::cout << std::flush;
            continue;
        }

        // Archive this turn's rendered output to history
        auto turnLines = renderer.render();
        // User prompt already in historyLines (added before streaming started)
        historyLines.insert(historyLines.end(), turnLines.begin(), turnLines.end());
        if (historyLines.empty()) historyLines.push_back("");
        renderer.clear();
        streaming = false;
        sess.records.push_back({SessionRecord::USER, cmd, session::SessionManager::iso8601(), ""});
        sess.records.push_back({SessionRecord::AGENT, agent.responseOutput(), session::SessionManager::iso8601(), ""});
        sm.save(sess);
        renderScreen();
    }

    input.stop();
    input.history().save(histPath);
    sm.save(sess);
    std::cout << "\nBye.\n";
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════
// Command: serve
// ═══════════════════════════════════════════════════════════════════════
static int cmdServe(const CliConfig& cli) {
    std::cout << "Server mode — use cortex-mk3-server binary instead.\n";
    std::cout << "  cortex-mk3-server --host " << cli.serverHost
              << " --port " << cli.serverPort
              << " --threads " << cli.serverThreads;
    if (!cli.serverApiKey.empty()) std::cout << " --api-key " << cli.serverApiKey;
    if (cli.serverNoCors) std::cout << " --no-cors";
    std::cout << "\n";
    return 1;
}

// ═══════════════════════════════════════════════════════════════════════
// main
// ═══════════════════════════════════════════════════════════════════════
int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGWINCH, signalHandler);

    CliConfig cli = parseArgs(argc, argv);

    // Load config file
    if (cli.configPath.empty()) cli.configPath = defaultConfigPath();
    auto cfg = loadConfigFile(cli.configPath);
    applyConfig(cli, cfg);

    // Dispatch
    if (cli.showHelp) return cmdHelp(cli);
    if (cli.command == "version") return cmdVersion();
    if (cli.command == "help") return cmdHelp(cli);
    if (cli.command == "list") return cmdList(cli);
    if (cli.command == "config") return cmdConfig(cli);
    if (cli.command == "completions") return cmdCompletions(cli);
    if (cli.command == "serve") return cmdServe(cli);

    // Default: run
    return cmdRun(cli);
}
