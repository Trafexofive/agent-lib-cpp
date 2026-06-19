// =============================================================================
// agent-lib-MK3 — CLI Runner v3.1
// Subcommand-based CLI with getopt_long, config file, progress indicators.
// =============================================================================

#include <getopt.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "src/core/agent.hpp"
#include "src/core/manifest_autoload.hpp"
#include "src/core/manifest_loader.hpp"
#include "src/core/sandbox_launcher.hpp"
#include "src/providers/factory.hpp"
#include "src/sandbox/policy.hpp"
#include "src/session/manager.hpp"
#include "src/tui/dialog.hpp"
#include "src/tui/input.hpp"
#include "src/tui/renderer.hpp"
#include "src/tui/session_view.hpp"
#include "src/tui/slash_commands.hpp"
#include "src/utils/ansi.hpp"

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
    std::string provider = "openai-codex";
    std::string model;  // empty → provider's defaultModel is used
    bool providerSet = false;
    bool modelSet = false;
    bool providerPickerRequested = false;  // --provider with no arg, no model

    // Run mode
    std::string prompt;
    std::string promptFile;
    std::string manifestPath;
    std::string manifestDir;
    int iterations = 0;
    std::string sessionId;
    bool continueSession = false;
    std::string resumeSessionId;
    bool listSessions = false;
    std::string systemPromptPath;
    std::string harnessPromptPath;
    bool ephemeral = false;
    bool raw = false;
    bool replMode = false;
    std::string tuiDebugDumpPath;

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
// ask_tool bridge — shared pending-dialog state between agent thread and TUI
// ═══════════════════════════════════════════════════════════════════════
struct AskDialogSession {
    std::string actionId;
    Json::Value params;
    cortex::mk3::tui::DialogState state;
    Json::Value result;
    bool active = false;
    bool complete = false;
    bool cancelled = false;
    std::mutex mutex;
    std::condition_variable cv;
};

// ═══════════════════════════════════════════════════════════════════════
// Progress spinner
// ═══════════════════════════════════════════════════════════════════════
class Spinner {
   public:
    void start(const std::string& msg) {
        if (running_)
            return;
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
        if (thread_.joinable())
            thread_.join();
    }
    ~Spinner() {
        stop();
    }

   private:
    std::atomic<bool> running_{false};
    std::thread thread_;
};

// ═══════════════════════════════════════════════════════════════════════
// Signal handler
// ═══════════════════════════════════════════════════════════════════════
void signalHandler(int sig) {
    if (sig == SIGWINCH)
        g_resized = true;
    else
        cortex::mk3::g_running = false;
}

// ═══════════════════════════════════════════════════════════════════════
// Help
// ═══════════════════════════════════════════════════════════════════════
void printBanner() {
    std::cout << ansi::bold << ansi::cyan << "  Cortex MK3 " << VERSION << " — Agent Runtime\n"
              << ansi::reset;
}

void printHelpGeneral() {
    std::cout << R"(Usage: cortex-mk3 [global-flags] <command> [command-flags]

Global flags:
  --config <path>      Config file (default: ~/.config/cortex-mk3/config)
  --manifest-dir <dir> Manifest catalog root (default: ./manifests; explicit imports only)
  --iterations <n>     Max turns before forced response (default: 20)
  --provider <name>    LLM provider (deepseek, openrouter, openai-codex, groq, zen, together, fireworks)
  --model <name>       Model name
  --sandbox            Enable sandbox mode (tool restrictions)
  --sandbox-ro         Read-only sandbox (no writes, restricted exec)
  --verbose, -V        Verbose: dump full prompts each iteration
  --debug              Enable debug output
  --raw                Pipe-clean output (no formatting, no banner)
  -r, --sessions       List saved sessions
  --tui-debug-dump <path> Auto-write TUI render/debug state (env: MK3_TUI_DEBUG_DUMP)
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
  -c, --continue         Continue the most recent session
  --resume <id>          Resume a specific session
  -r, --sessions         List saved sessions
  --ephemeral            Don't save session
  --repl                 Force interactive mode even with --prompt
  --tui-debug-dump <path> Auto-write TUI render/debug state
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
    if (!home)
        return "";
    return std::string(home) + "/.config/cortex-mk3/config";
}

static std::map<std::string, std::string> loadConfigFile(const std::string& path) {
    std::map<std::string, std::string> cfg;
    std::ifstream f(path);
    if (!f.good())
        return cfg;
    std::string line;
    while (std::getline(f, line)) {
        // Skip comments and blanks
        if (line.empty() || line[0] == '#')
            continue;
        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // Trim
        while (!key.empty() && key.back() == ' ')
            key.pop_back();
        while (!val.empty() && val.front() == ' ')
            val.erase(0, 1);
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
    if (!cli.providerSet)
        cli.provider = get("provider", cli.provider);
    // Only inherit model from config when provider was ALSO inherited (not
    // explicitly set). Otherwise `--provider zen` would drag in a stale
    // `model=gpt-5.5` from the config file and 401 on a model that provider
    // can't serve. When --model is omitted, fall through to the provider's
    // own defaultModel.
    if (!cli.providerSet && !cli.modelSet)
        cli.model = get("model", cli.model);
    if (cli.systemPromptPath.empty())
        cli.systemPromptPath = get("system_prompt", cli.systemPromptPath);
    if (cli.manifestDir.empty())
        cli.manifestDir = get("manifest_dir", cli.manifestDir);
    if (cli.configPath.empty())
        cli.configPath = get("config_path", defaultConfigPath());
}

// ═══════════════════════════════════════════════════════════════════════
// Argument parsing
// ═══════════════════════════════════════════════════════════════════════
static CliConfig parseArgs(int argc, char* argv[]) {
    CliConfig cli;

    // Long options
    static struct option longOpts[] = {// Global
                                       {"config", required_argument, 0, 'C'},
                                       {"manifest-dir", required_argument, 0, 'G'},
                                       {"iterations", required_argument, 0, 'X'},
                                       {"provider", optional_argument, 0, 'P'},
                                       {"model", required_argument, 0, 'M'},
                                       {"sandbox", no_argument, 0, 'S'},
                                       {"sandbox-ro", no_argument, 0, 'R'},
                                       {"verbose", no_argument, 0, 'V'},
                                       {"debug", no_argument, 0, 'D'},
                                       {"raw", no_argument, 0, 1003},
                                       {"sessions", no_argument, 0, 'r'},
                                       {"tui-debug-dump", required_argument, 0, 1001},
                                       {"dry-run", no_argument, 0, 'n'},
                                       {"help", no_argument, 0, 'h'},

                                       // Run
                                       {"prompt", required_argument, 0, 'p'},
                                       {"file", required_argument, 0, 'f'},
                                       {"manifest", required_argument, 0, 'm'},
                                       {"harness", required_argument, 0, 'H'},
                                       {"system", required_argument, 0, 'y'},
                                       {"session", required_argument, 0, 's'},
                                       {"continue", no_argument, 0, 'c'},
                                       {"resume", required_argument, 0, 1002},
                                       {"ephemeral", no_argument, 0, 'e'},
                                       {"repl", no_argument, 0, 'E'},

                                       // Serve
                                       {"host", required_argument, 0, 'o'},
                                       {"port", required_argument, 0, 'O'},
                                       {"threads", required_argument, 0, 'T'},
                                       {"api-key", required_argument, 0, 'K'},
                                       {"no-cors", no_argument, 0, 'N'},

                                       // List
                                       {"providers", no_argument, 0, 'L'},
                                       {"models", optional_argument, 0, 'l'},
                                       {"tools", no_argument, 0, 't'},

                                       // Config
                                       {"show", no_argument, 0, 'w'},
                                       {"set", required_argument, 0, 'W'},
                                       {"init", no_argument, 0, 'I'},

                                       {0, 0, 0, 0}};

    if (argc >= 2 && std::string(argv[1]) == "list") {
        cli.command = "list";
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--providers" || arg == "-L") {
                cli.listProviders = true;
            } else if (arg.rfind("--provider=", 0) == 0) {
                cli.provider = arg.substr(std::string("--provider=").size());
                cli.providerSet = true;
                cli.listModels = true;
                cli.listModelsProvider = cli.provider;
            } else if (arg == "--provider") {
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    cli.provider = argv[++i];
                    cli.providerSet = true;
                    cli.listModels = true;
                    cli.listModelsProvider = cli.provider;
                } else {
                    cli.listProviders = true;
                }
            } else if (arg == "--models" || arg == "-l") {
                cli.listModels = true;
                if (i + 1 < argc && argv[i + 1][0] != '-')
                    cli.listModelsProvider = argv[++i];
            } else if (arg.rfind("--models=", 0) == 0) {
                cli.listModels = true;
                cli.listModelsProvider = arg.substr(std::string("--models=").size());
            } else if (arg == "--tools" || arg == "-t") {
                cli.listTools = true;
            } else if (arg == "--help" || arg == "-h") {
                cli.showHelp = true;
            }
        }
        return cli;
    }

    int opt;
    bool sawProviderFlag = false;
    bool providerFlagHadArg = false;
    while ((opt = getopt_long(argc, argv, "C:G:P:M:p:f:m:H:y:s:VhrSReEDnX:c", longOpts, nullptr)) !=
           -1) {
        switch (opt) {
            // Global
            case 'C':
                cli.configPath = optarg;
                break;
            case 'G':
                cli.manifestDir = optarg;
                break;
            case 'P':
                sawProviderFlag = true;
                if (optarg) {
                    providerFlagHadArg = true;
                    cli.provider = optarg;
                    cli.providerSet = true;
                } else {
                    // `--provider` with no arg: in list context, show providers.
                    // In run context, launch interactive picker.
                    if (cli.command == "list")
                        cli.listProviders = true;
                    else
                        cli.providerPickerRequested = true;
                }
                break;
            case 'M':
                cli.model = optarg;
                cli.modelSet = true;
                break;
            case 'S':
                cli.sandbox = true;
                break;
            case 'R':
                cli.sandbox = true;
                cli.sandboxReadOnly = true;
                break;
            case 'V':
                cli.verbose = true;
                break;
            case 'D':
                cli.debug = true;
                break;
            case 'X':
                cli.iterations = std::stoi(optarg);
                break;
            case 'r':
                cli.listSessions = true;
                break;
            case 1003:
                cli.raw = true;
                break;
            case 1001:
                cli.tuiDebugDumpPath = optarg;
                break;
            case 'n':
                cli.dryRun = true;
                break;
            case 'h':
                cli.showHelp = true;
                break;

            // Run
            case 'p':
                cli.prompt = optarg;
                break;
            case 'f':
                cli.promptFile = optarg;
                break;
            case 'm':
                cli.manifestPath = optarg;
                break;
            case 'H':
                cli.harnessPromptPath = optarg;
                break;
            case 'y':
                cli.systemPromptPath = optarg;
                break;
            case 's':
                cli.sessionId = optarg;
                break;
            case 'c':
                cli.continueSession = true;
                break;
            case 1002:
                cli.resumeSessionId = optarg;
                break;
            case 'e':
                cli.ephemeral = true;
                break;
            case 'E':
                cli.replMode = true;
                break;

            // Serve
            case 'o':
                cli.serverHost = optarg;
                break;
            case 'O':
                cli.serverPort = std::stoi(optarg);
                break;
            case 'T':
                cli.serverThreads = std::stoi(optarg);
                break;
            case 'K':
                cli.serverApiKey = optarg;
                break;
            case 'N':
                cli.serverNoCors = true;
                break;

            // List
            case 'L':
                cli.listProviders = true;
                break;
            case 'l':
                cli.listModels = true;
                if (optarg)
                    cli.listModelsProvider = optarg;
                break;
            case 't':
                cli.listTools = true;
                break;

            // Config
            case 'w':
                cli.configShow = true;
                break;
            case 'W':
                cli.configSet = optarg;
                break;
            case 'I':
                cli.configInit = true;
                break;

            default:
                std::cerr << "Try 'cortex-mk3 --help'\n";
                exit(1);
        }
    }

    // GNU getopt only consumes optional option args with --opt=value. For
    // `--provider openrouter`, optarg is null and the provider name remains in
    // argv[optind], so normalize that form for all commands.
    if (sawProviderFlag && !providerFlagHadArg && optind < argc && argv[optind][0] != '-') {
        cli.provider = argv[optind];
        cli.providerSet = true;
        cli.providerPickerRequested = false;  // got a real provider name
        optind++;
    }

    // Subcommand (first positional after flags)
    if (optind < argc) {
        std::string cmd = argv[optind];
        if (cmd == "run" || cmd == "serve" || cmd == "list" || cmd == "config" ||
            cmd == "completions" || cmd == "version" || cmd == "help") {
            cli.command = cmd;
            optind++;
        } else {
            // Unknown command — treat as prompt for backward compat
            cli.prompt = cmd;
            for (int i = optind; i < argc; i++) {
                if (!cli.prompt.empty())
                    cli.prompt += " ";
                cli.prompt += argv[i];
            }
        }
    }

    // `list --provider <name>` → list models for that provider.
    // GNU getopt only consumes optional option args with --provider=name, so
    // `--provider opencode-go` leaves the provider name in argv[optind].
    if (cli.command == "list" && sawProviderFlag && !cli.listModels && !cli.listProviders) {
        if (!providerFlagHadArg && optind < argc && argv[optind][0] != '-') {
            cli.listModels = true;
            cli.listModelsProvider = argv[optind];
        } else {
            cli.listProviders = true;
        }
    }

    // List --models accepts both `--models=provider` and `--models provider`.
    if (cli.command == "list" && cli.listModels && cli.listModelsProvider.empty() &&
        optind < argc) {
        cli.listModelsProvider = argv[optind];
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
    if (cli.command.empty())
        cli.command = "run";

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
// Command: sessions
// ═══════════════════════════════════════════════════════════════════════
static int cmdSessions() {
    session::SessionManager sm;
    auto sessions = sm.list();
    std::sort(sessions.begin(), sessions.end(),
              [](const auto& a, const auto& b) { return a.updated > b.updated; });
    if (sessions.empty()) {
        std::cout << "No saved sessions.\n";
        return 0;
    }
    std::cout << "Saved sessions:\n\n";
    for (const auto& s : sessions) {
        std::cout << "  " << s.id << "  " << s.updated << "  " << s.turnCount << " turns";
        if (!s.model.empty()) std::cout << "  " << s.model;
        std::cout << "\n";
    }
    std::cout << "\nResume with: cortex-mk3 --resume <id>\n";
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════
// Command: list
// ═══════════════════════════════════════════════════════════════════════
static int cmdList(const CliConfig& cli) {
    if (cli.listProviders) {
        std::cout << "Available providers:\n\n";
        // Pull from the factory so the list stays in sync with availableProviders().
        static const std::vector<std::pair<std::string, std::string>> providerInfo = {
            {"deepseek", "DeepSeek API        (DEEPSEEK_API_KEY)"},
            {"openrouter", "OpenRouter          (OPENROUTER_API_KEY)"},
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
        for (const auto& p :
             {"deepseek", "openrouter", "openai-codex", "groq", "zen", "together", "fireworks"}) {
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

    // Default: show providers (most useful; use --models/--tools for the rest)
    CliConfig showProviders = cli;
    showProviders.listProviders = true;
    return cmdList(showProviders);
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
    // ── Gather providers ──
    static const std::vector<std::pair<std::string, std::string>> providerInfo = {
        {"deepseek", "DeepSeek API"},
        {"openrouter", "OpenRouter"},
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

    // ── Raw mode ──
    struct termios oldt;
    tcgetattr(STDIN_FILENO, &oldt);
    struct termios raw = oldt;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    std::cout << tui::ansi::hideCursor();

    auto renderList = [&](const std::string& title, const std::string& subtitle,
                          const std::vector<std::pair<std::string, std::string>>& items,
                          int selected) {
        tui::DialogState ds;
        ds.chainTitle = title;
        ds.message = subtitle;
        tui::DialogCard card;
        card.id = "pick";
        card.type = "choice";
        card.title = "";
        for (const auto& [val, label] : items)
            card.options.push_back({val, label, "", false});
        ds.cards.push_back(std::move(card));
        ds.selectedOption = selected;
        auto lines = tui::DialogRenderer::render(ds, 80);
        std::cout << tui::ansi::clearScreen() << tui::ansi::moveTo(1, 1);
        for (const auto& line : lines)
            std::cout << line << "\r\n";
        std::cout.flush();
    };

    auto readKey = [&]() -> std::pair<tui::KeyAction, char> {
        // Block until a key is available — prevents the spin loop.
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        select(STDIN_FILENO + 1, &fds, nullptr, nullptr, nullptr);  // no timeout = block

        char buf[64];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0)
            return {tui::KeyAction::NONE, 0};
        // Accumulate escape sequences
        std::string seq;
        for (ssize_t i = 0; i < n; i++) {
            seq += buf[i];
        }
        // Handle multi-byte escape sequences
        if (seq[0] == 27) {
            if (seq.size() == 1) {
                // Bare ESC — wait a bit for continuation
                struct timeval tv = {0, 5000};
                FD_ZERO(&fds);
                FD_SET(STDIN_FILENO, &fds);
                if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
                    char buf2[64];
                    ssize_t n2 = read(STDIN_FILENO, buf2, sizeof(buf2));
                    if (n2 > 0)
                        seq.append(buf2, n2);
                }
            }
        }
        char outChar = 0;
        tui::KeyMap keymap;
        tui::KeyAction act = keymap.resolve(seq, outChar);
        return {act, outChar};
    };

    // ── Provider selection (skip if already chosen) ──
    int sel = 0;
    bool picked = false;
    if (outProvider.empty()) {
        while (!picked) {
            renderList("Select Provider",
                       "j/k or arrows to navigate, Enter to select, Esc to cancel", providers, sel);
            auto [act, ch] = readKey();
            if (act == tui::KeyAction::ENTER) {
                picked = true;
            } else if (act == tui::KeyAction::CANCEL || act == tui::KeyAction::EXIT) {
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
                std::cout << tui::ansi::showCursor() << tui::ansi::clearScreen()
                          << tui::ansi::moveTo(1, 1);
                return false;
            } else if (act == tui::KeyAction::HISTORY_DOWN ||
                       (act == tui::KeyAction::CHAR && ch == 'j')) {
                if (sel < (int)providers.size() - 1)
                    sel++;
            } else if (act == tui::KeyAction::HISTORY_UP ||
                       (act == tui::KeyAction::CHAR && ch == 'k')) {
                if (sel > 0)
                    sel--;
            }
        }
        outProvider = providers[sel].first;
    }  // end provider selection

    // ── Model selection ──
    auto provider = providers::createProvider(outProvider, "");
    if (!provider) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
        std::cout << tui::ansi::showCursor() << tui::ansi::clearScreen() << tui::ansi::moveTo(1, 1);
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
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
        std::cout << tui::ansi::showCursor() << tui::ansi::clearScreen() << tui::ansi::moveTo(1, 1);
        std::cerr << "No models available for " << outProvider << ". Use --model to specify one.\n";
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

    sel = 0;
    picked = false;
    while (!picked) {
        renderList("Select Model — " + outProvider,
                   "j/k or arrows to navigate, Enter to select, Esc to cancel", modelItems, sel);
        auto [act, ch] = readKey();
        if (act == tui::KeyAction::ENTER) {
            picked = true;
        } else if (act == tui::KeyAction::CANCEL || act == tui::KeyAction::EXIT) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
            std::cout << tui::ansi::showCursor() << tui::ansi::clearScreen()
                      << tui::ansi::moveTo(1, 1);
            return false;
        } else if (act == tui::KeyAction::HISTORY_DOWN ||
                   (act == tui::KeyAction::CHAR && ch == 'j')) {
            if (sel < (int)modelItems.size() - 1)
                sel++;
        } else if (act == tui::KeyAction::HISTORY_UP ||
                   (act == tui::KeyAction::CHAR && ch == 'k')) {
            if (sel > 0)
                sel--;
        }
    }
    outModel = modelItems[sel].first;

    // ── Restore terminal ──
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
    std::cout << tui::ansi::showCursor() << tui::ansi::clearScreen() << tui::ansi::moveTo(1, 1);
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
// Command: run (agent execution)
// ═══════════════════════════════════════════════════════════════════════
static int cmdRun(CliConfig& cli) {
    // ── Interactive picker ──
    // Three cases trigger the picker (only in non-REPL, no-prompt, no-manifest mode):
    //   1. `--provider` bare → pick provider then model
    //   2. `--provider openrouter` with no --model → pick model for that provider
    //   3. No --provider and no --model → pick provider then model
    bool needPicker = false;
    if (cli.prompt.empty() && cli.promptFile.empty() && !cli.replMode && cli.manifestPath.empty()) {
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
        if (cli.providerSet)
            acfg.provider = cli.provider;
        if (cli.modelSet)
            acfg.model = cli.model;
        if (!cli.harnessPromptPath.empty())
            acfg.harnessPath = cli.harnessPromptPath;

        if (acfg.sandboxMode == "docker" && !fs::exists("/.dockerenv")) {
            auto files = ManifestLoader::loadFiles(cli.manifestPath);
            return sandbox::launchDocker(cli.manifestPath, acfg, files);
        }
    } else {
        acfg.name = "cortext-builtin-agent";
        acfg.provider = cli.provider;
        acfg.model = cli.model;
        if (!cli.harnessPromptPath.empty()) {
            acfg.harnessPath = cli.harnessPromptPath;
        } else {
            acfg.harnessPath = "manifests/harness/default.md";
        }
        if (!cli.systemPromptPath.empty()) {
            acfg.systemPromptPath = cli.systemPromptPath;
        } else {
            acfg.systemPromptPath = "manifests/system/default.md";
        }
        acfg.personaPath = "manifests/persona/default.md";
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
    tui::TuiRenderer renderer(80);

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

    if (cli.debug)
        agent.setEnv("__DEBUG_MODE__", "true");
    if (cli.raw)
        agent.setRaw(true);
    if (cli.verbose)
        agent.setVerbose(true);

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
            for (auto& l : md.render())
                std::cout << l << std::endl;
        } else {
            std::cout << result << std::endl;
        }
        return 0;
    }

    if (cli.tuiDebugDumpPath.empty()) {
        const char* dumpEnv = getenv("MK3_TUI_DEBUG_DUMP");
        if (dumpEnv && *dumpEnv) {
            cli.tuiDebugDumpPath =
                std::string(dumpEnv) == "1" ? "/tmp/mk3-tui-debug-dump.txt" : std::string(dumpEnv);
        }
    }

    // ── Enter alternate screen ──
    std::cout << "\033[?1049h\033[?25l" << std::flush;
    atexit([] { std::cout << "\033[?1049l\033[?25h" << std::flush; });

    int termW = 80, termH = 24;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        termW = ws.ws_col;
        termH = ws.ws_row;
    } else {
        const char* ec = getenv("COLUMNS");
        if (ec)
            termW = std::stoi(ec);
        const char* er = getenv("LINES");
        if (er)
            termH = std::stoi(er);
    }
    renderer.setWidth(termW);

    // Bottom-up layout: output anchors to bottom, above status/input bars
    tui::Input input;
    // ask_tool dialog state — shared between renderScreen and the prompt loop.
    auto askDialog = std::make_shared<AskDialogSession>();
    std::atomic<bool> dialogActive{false};
    std::string dialogInputLine;  // last submitted line during a dialog
    std::vector<std::string> historyLines;
    int scrollOffset = 0;      // lines scrolled above viewport
    bool showPrompts = false;  // /prompts toggle
    auto lastRenderTime = std::chrono::steady_clock::now();
    bool streaming = false;                              // true during LLM call, false when idle
    int spinnerFrame = 0;                                // animated spinner
    std::chrono::steady_clock::time_point streamStart_;  // for TTC
    static const char* spinnerFrames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    bool renderDirty = true;
    std::vector<std::string> tuiFrameLog;
    std::vector<std::string> tuiAnsiFrames;
    auto lastStatusTime = std::chrono::steady_clock::now();
    std::string streamPhase = "idle";
    size_t streamActionCount = 0;
    size_t streamResultCount = 0;
    size_t streamRespBytes = 0;
    size_t streamRawBytes = 0;
    auto statusBarText = [&](int displaySize) -> std::string {
        (void)displaySize;
        if (dialogActive.load(std::memory_order_acquire)) {
            return std::string(ansi::dim) + "  Esc to cancel" + ansi::reset;
        }
        std::string spinner = streaming ? std::string("\033[38;2;255;200;50m") +
                                              spinnerFrames[spinnerFrame] + "\033[0m "
                                        : "";
        std::string ttc;
        if (streaming) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - streamStart_)
                               .count();
            if (elapsed >= 1000)
                ttc = std::to_string(elapsed / 1000) + "." +
                      std::to_string((elapsed % 1000) / 100) + "s";
            else if (elapsed >= 100)
                ttc = "0." + std::to_string(elapsed / 100) + "s";
        }
        std::string telemetry;
        if (streaming) {
            telemetry = " " + streamPhase + " act=" + std::to_string(streamActionCount) +
                        " done=" + std::to_string(streamResultCount) + " " +
                        std::to_string(streamRespBytes) + "b";
        }
        std::string mode = tui::TuiRenderer::modeName(renderer.mode());
        std::string model = acfg.provider + "/" + acfg.model;
        return spinner + ttc + telemetry + ansi::dim + "  " + mode + " · " + model + ansi::reset;
    };
    auto inputLineText = [&]() -> std::string {
        std::ostringstream out;
        // ── Status bar (one line above prompt) ──
        out << "\033[" << (termH - 1) << ";1H\033[2K" << statusBarText(0);
        // ── Prompt line ──
        out << "\033[" << termH << ";1H\033[2K";
        if (dialogActive.load(std::memory_order_acquire)) {
            out << ansi::dim << "  Enter to submit" << ansi::reset;
            return out.str();
        }
        out << ansi::bold << "▸ " << ansi::reset << "\033[2m";
        if (input.searching()) {
            out << tui::ansi::fg(255, 200, 0) << input.searchLine();
        } else {
            size_t cp = input.cursorPos();
            std::string l = input.line();
            out << l.substr(0, cp);
            out << "\033[7m" << (cp < l.size() ? std::string(1, l[cp]) : " ") << "\033[27m";
            if (cp < l.size())
                out << l.substr(cp + 1);
        }
        out << ansi::reset << " ";
        return out.str();
    };
    auto redrawStatusOnly = [&](bool force = false) {
        if (!streaming)
            return;
        auto now = std::chrono::steady_clock::now();
        if (!force &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStatusTime).count() <
                100)
            return;
        lastStatusTime = now;
        spinnerFrame = (spinnerFrame + 1) % 10;
        std::cout << inputLineText() << std::flush;
    };
    auto captureAnsiFrame = [&](const std::vector<std::string>& visible, int startRow,
                                int visibleCount, int displaySize) {
        std::vector<std::string> frame(termH);
        for (int i = 0; i < visibleCount && i < (int)visible.size(); i++) {
            int row = startRow + i - 1;
            if (row >= 0 && row < termH)
                frame[row] = visible[i];
        }
        frame[termH - 2] = statusBarText(displaySize);
        // inputLineText now includes both status + prompt; just use it for the
        // last two lines for the frame capture.
        std::string fullInput = inputLineText();
        frame[termH - 1] = fullInput;
        std::ostringstream ss;
        for (const auto& line : frame)
            ss << line << "\n";
        tuiAnsiFrames.push_back(ss.str());
        while (tuiAnsiFrames.size() > 20)
            tuiAnsiFrames.erase(tuiAnsiFrames.begin());
    };
    tui::SessionView sessionView(termW, termH);
    auto renderScreen = [&]() {
        if (streaming) {
            if (!renderDirty)
                return;
            auto now = std::chrono::steady_clock::now();
            auto sinceRender =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRenderTime).count();
            if (sinceRender < 50)
                return;
            spinnerFrame = (spinnerFrame + 1) % 10;
            lastRenderTime = now;
        }

        std::vector<std::string> rendererLines;
        std::vector<std::string> dialogLines;
        bool showDialog = dialogActive.load(std::memory_order_acquire) && !askDialog->state.done();
        if (showDialog) {
            dialogLines = cortex::mk3::tui::DialogRenderer::render(askDialog->state, termW, input.line());
        } else if (showPrompts) {
            auto& prompts = agent.iterationPrompts();
            if (prompts.empty()) {
                rendererLines.push_back("\033[2m(no prompts captured — run a prompt first)\033[0m");
            } else {
                for (size_t i = 0; i < prompts.size(); i++) {
                    rendererLines.push_back(std::string("\033[1m") + tui::ansi::fg(200, 200, 100) +
                                            "─── Iter " + std::to_string(i + 1) + " ───\033[0m");
                    std::istringstream ps(prompts[i]);
                    std::string pline;
                    while (std::getline(ps, pline)) {
                        rendererLines.push_back(std::string("\033[2m") + pline + "\033[0m");
                    }
                    rendererLines.push_back("");
                }
            }
        } else {
            rendererLines = renderer.render();
        }

        tui::SessionViewport vp = sessionView.build(historyLines, rendererLines, dialogLines, showDialog, scrollOffset);
        if (!cli.tuiDebugDumpPath.empty())
            captureAnsiFrame(vp.visible, vp.startRow, vp.visibleCount, vp.displaySize);

        std::cout << sessionView.render(vp, statusBarText, inputLineText()) << std::flush;
        renderDirty = false;
    };

    std::string cmd;
    bool quit = false;
    session::SessionManager sm;
    std::string sessionId;
    if (!cli.resumeSessionId.empty()) {
        sessionId = cli.resumeSessionId;
    } else if (cli.continueSession) {
        auto sessions = sm.list();
        std::sort(sessions.begin(), sessions.end(),
                  [](const auto& a, const auto& b) { return a.updated > b.updated; });
        sessionId = sessions.empty() ? "default" : sessions[0].id;
    } else {
        sessionId = cli.sessionId.empty() ? "default" : cli.sessionId;
    }
    auto sess = sm.exists(sessionId) ? sm.load(sessionId)
                                     : sm.create(sessionId, "mk3", cli.model, cli.provider);
    input.start([&](const std::string& s) {
        if (dialogActive.load(std::memory_order_acquire))
            dialogInputLine = s;
        else
            cmd = s;
    });

    // Load history
    const char* home = getenv("HOME");
    std::string histPath = home ? std::string(home) + "/.mk3_history" : "/tmp/.mk3_history";
    input.history().load(histPath);
    input.setCompleter(
        [](const std::string& prefix) { return tui::SlashCommands::complete(prefix); });
    input.scrollUp = [&] {
        scrollOffset += (termH - 2) / 2;
        renderScreen();
    };
    input.scrollDown = [&] {
        scrollOffset -= (termH - 2) / 2;
        renderScreen();
    };
    input.clearScreen = [&] {
        std::cout << "\033[2J\033[H" << std::flush;
        renderScreen();
    };

    auto pushTuiLine = [&](const std::string& line) {
        historyLines.push_back(std::string("\033[2m\033[3m") + line + ansi::reset);
        renderDirty = true;
    };
    auto pushTuiSection = [&](const std::string& title, const std::vector<std::string>& items) {
        pushTuiLine("[" + title + "] " + (items.empty() ? "none" : std::to_string(items.size())));
        for (const auto& item : items)
            pushTuiLine("  - " + item);
    };
    auto dumpTuiState = [&](const std::string& path, const std::string& reason,
                            bool notify) -> bool {
        if (path.empty())
            return false;
        std::ofstream f(path);
        auto lines = renderer.render();
        if (!f) {
            if (notify)
                pushTuiLine("Failed to write " + path);
            return false;
        }
        const auto& acts = agent.protocolActions();
        const auto& ress = agent.protocolResults();
        f << "# Cortex MK3 TUI debug dump\n";
        f << "reason: " << reason << "\n";
        f << "mode: " << tui::TuiRenderer::modeName(renderer.mode()) << "\n";
        f << "term: " << termW << "x" << termH << "\n";
        f << "streaming: " << (streaming ? "true" : "false") << "\n";
        f << "history_lines: " << historyLines.size() << "\n";
        f << "render_lines: " << lines.size() << "\n";
        f << "protocol_actions: " << acts.size() << "\n";
        f << "protocol_results: " << ress.size() << "\n";
        f << "frame_log_lines: " << tuiFrameLog.size() << "\n";
        f << "ansi_snapshot_count: " << tuiAnsiFrames.size() << "\n\n";
        f << "## Frame log\n";
        for (const auto& l : tuiFrameLog)
            f << l << "\n";
        f << "\n## Raw ANSI snapshots\n";
        for (size_t i = 0; i < tuiAnsiFrames.size(); i++) {
            f << "--- ansi frame " << i << " ---\n";
            f << tuiAnsiFrames[i];
        }
        f << "\n## Protocol events\n";
        for (const auto& a : acts) {
            f << "ACTION " << a.type << " " << a.name << "#" << a.id
              << " sync=" << (a.sync ? "true" : "false") << "\n";
            if (!a.body.empty())
                f << "  body: " << a.body.substr(0, 1200) << (a.body.size() > 1200 ? "..." : "")
                  << "\n";
        }
        for (const auto& r : ress) {
            f << "RESULT " << r.id << " ok=" << (r.ok ? "true" : "false") << " ms=" << r.elapsedMs
              << " bytes=" << r.outputBytes << "\n";
            if (!r.summary.empty())
                f << "  summary: " << r.summary.substr(0, 1200)
                  << (r.summary.size() > 1200 ? "..." : "") << "\n";
        }
        f << "\n## History\n";
        for (const auto& l : historyLines)
            f << l << "\n";
        f << "\n## Current Renderer\n";
        for (const auto& l : lines)
            f << l << "\n";
        if (notify) {
            pushTuiLine("Wrote " + path + " (reason " + reason + ", history " +
                        std::to_string(historyLines.size()) + ", current " +
                        std::to_string(lines.size()) + ", actions " + std::to_string(acts.size()) +
                        ", results " + std::to_string(ress.size()) + ")");
        }
        return true;
    };
    auto workflowNamesFromXml = [&]() {
        std::vector<std::string> names;
        size_t pos = 0;
        while ((pos = workflowXml.find("<workflow", pos)) != std::string::npos) {
            size_t namePos = workflowXml.find("name=\"", pos);
            if (namePos == std::string::npos) {
                pos += 9;
                continue;
            }
            namePos += 6;
            size_t end = workflowXml.find('"', namePos);
            if (end == std::string::npos) {
                pos += 9;
                continue;
            }
            names.push_back(workflowXml.substr(namePos, end - namePos));
            pos = end + 1;
        }
        return names;
    };
    auto showManifests = [&]() {
        std::vector<std::string> tools;
        for (const auto& s : allSchemas)
            tools.push_back(s.name + (s.description.empty() ? "" : " — " + s.description));
        pushTuiLine("─── Active Manifest Surface ───");
        pushTuiLine("agent: " + agent.name() + "  provider: " + agent.config().provider +
                    "  model: " + agent.config().model);
        pushTuiSection("tools", tools);
        pushTuiSection("feeds", agent.feedNames());
        pushTuiSection("relics", agent.relicNames());
        pushTuiSection("agents", agent.subAgentNames());
        pushTuiSection("workflows", workflowNamesFromXml());
    };

    renderScreen();

    while (cortex::mk3::g_running && !quit) {
        // Handle window resize
        if (g_resized) {
            g_resized = false;
            struct winsize ws;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
                termW = ws.ws_col;
                termH = ws.ws_row;
                renderer.setWidth(termW);
                sessionView.setWidthHeight(termW, termH);
            }
        }
        cmd.clear();
        while (cmd.empty() && cortex::mk3::g_running) {
            std::string before = input.line();
            size_t beforeCp = input.cursorPos();
            input.poll();
            if (input.line() != before || input.cursorPos() != beforeCp) {
                renderDirty = true;
                renderScreen();
            }
        }
        if (!cortex::mk3::g_running || cmd.empty())
            continue;

        if (cmd == "/quit" || cmd == "/exit") {
            quit = true;
            break;
        }
        if (cmd[0] == '/') {
            if (cmd == "/help" || cmd == "/commands") {
                for (auto& l : tui::SlashCommands::helpLines())
                    pushTuiLine(l);
            } else if (cmd == "/manifests") {
                showManifests();
            } else if (cmd == "/prompts") {
                showPrompts = !showPrompts;
                historyLines.clear();
            } else if (tui::SlashCommands::isDynamic(cmd)) {
                for (auto& l : tui::SlashCommands::renderDynamic(cmd))
                    pushTuiLine(l);
            } else if (cmd == "/cp-all") {
                std::string all;
                for (auto& l : historyLines)
                    all += l + "\n";
                auto rl = renderer.render();
                for (auto& l : rl)
                    all += l + "\n";
                // Try both clipboard tools
                int rc = system("which wl-copy >/dev/null 2>&1 && wl-copy");
                if (rc != 0)
                    rc = system("which xclip >/dev/null 2>&1 && xclip -selection clipboard");
                if (rc != 0) {
                    // Fallback: write to temp file
                    std::ofstream f("/tmp/mk3-cp-all.txt");
                    if (f) {
                        f << all;
                        f.close();
                    }
                } else {
                    FILE* p = popen(rc == 0 ? "wl-copy" : "xclip -selection clipboard", "w");
                    if (p) {
                        fwrite(all.c_str(), 1, all.size(), p);
                        pclose(p);
                    }
                }
            } else if (cmd == "/cp-raw") {
                std::string raw = agent.rawLlOutput();
                int rc = system("which wl-copy >/dev/null 2>&1");
                FILE* p = popen(rc == 0 ? "wl-copy" : "xclip -selection clipboard", "w");
                if (p) {
                    fwrite(raw.c_str(), 1, raw.size(), p);
                    pclose(p);
                } else {
                    std::ofstream f("/tmp/mk3-cp-raw.txt");
                    if (f) {
                        f << raw;
                    }
                }
            } else if (cmd == "/sessions") {
                auto list = sm.list();
                historyLines.push_back(std::string("\033[2m\033[3m") + "─── Sessions ───" +
                                       ansi::reset);
                for (auto& s : list)
                    historyLines.push_back(std::string("\033[2m\033[3m") + s.id + "  " + s.updated +
                                           "  " + std::to_string(s.turnCount) + " turns" +
                                           ansi::reset);
            } else if (cmd == "/dump-render" || cmd == "/dr") {
                std::string path = cli.tuiDebugDumpPath.empty() ? "/tmp/mk3-render-dump.txt"
                                                                : cli.tuiDebugDumpPath;
                dumpTuiState(path, "slash-command", true);
            } else if (cmd == "/dump-prompt" || cmd == "/dp") {
                // Export last prompt to /tmp/mk3-prompt-iterN.xml for inspection
                auto& prompts = agent.iterationPrompts();
                if (prompts.empty()) {
                    historyLines.push_back(
                        "\033[2m(no prompts captured — run a prompt first)\033[0m");
                } else {
                    for (size_t i = 0; i < prompts.size(); i++) {
                        std::string path = "/tmp/mk3-prompt-iter" + std::to_string(i + 1) + ".xml";
                        std::ofstream f(path);
                        f << "<!-- Cortex MK3 Prompt — Iteration " << (i + 1) << " -->\n";
                        f << prompts[i];
                        historyLines.push_back(std::string("\033[2m\033[3m") + "Wrote " + path +
                                               " (" + std::to_string(prompts[i].size()) +
                                               " bytes)" + ansi::reset);
                    }
                }
            }
            renderScreen();
            continue;
        }

        // ── Prompt (threaded — allows concurrent input + Escape cancel) ──
        std::string promptText = cmd;  // stable copy; input callback may mutate cmd while streaming
        cmd.clear();
        cortex::mk3::g_running = true;
        streaming = true;
        streamPhase = "waiting provider";
        streamActionCount = 0;
        streamResultCount = 0;
        streamRespBytes = 0;
        streamRawBytes = 0;
        streamStart_ = std::chrono::steady_clock::now();
        input.clearEscape();

        // Echo user prompt in history BEFORE streaming (visible during response)
        historyLines.push_back(std::string(ansi::bold) + "▸ " + promptText + ansi::reset);
        scrollOffset = 0;
        renderDirty = true;
        renderScreen();

        size_t lastAct = 0, lastRes = 0;
        std::atomic<bool> agentDone{false};
        std::mutex streamMtx;
        std::vector<cortex::mk3::ProtocolAction> snapActions;
        std::vector<cortex::mk3::ProtocolResult> snapResults;
        std::string snapResponse, snapRaw, snapThought, snapPhase = "waiting provider";
        bool snapDirty = false;
        bool snapClearRenderer = false;
        bool firstToken = true;

        // ── ask_tool bridge: shared pending-dialog state ──
        std::mutex askMtx;
        std::condition_variable askCv;
        Json::Value askParams;
        bool askParamsReady = false;
        std::atomic<bool> askPending{false};

        agent.setAskToolHandler([&](const Json::Value& params) -> Json::Value {
            // Agent thread: park the request and wait for the TUI loop to
            // collect the user's answer.
            {
                std::lock_guard<std::mutex> lk(askMtx);
                askParams = params;
                askParamsReady = true;
            }
            askPending.store(true, std::memory_order_release);
            askCv.notify_one();

            std::unique_lock<std::mutex> lk(askMtx);
            askCv.wait(lk, [&] {
                return askDialog->complete || askDialog->cancelled || !cortex::mk3::g_running;
            });
            Json::Value out;
            if (askDialog->cancelled) {
                out["success"] = false;
                out["cancelled"] = true;
                out["results"] = askDialog->state.results;
            } else {
                out["success"] = true;
                out["results"] = askDialog->state.results;
            }
            askDialog->active = false;
            askDialog->complete = false;
            askDialog->cancelled = false;
            askParamsReady = false;
            askPending.store(false, std::memory_order_release);
            dialogActive.store(false, std::memory_order_release);
            input.clearActionInterceptor();
            renderDirty = true;
            return out;
        });

        auto applyStreamSnapshot = [&]() {
            std::vector<cortex::mk3::ProtocolAction> acts;
            std::vector<cortex::mk3::ProtocolResult> ress;
            std::string response, raw, thought, phase;
            bool clearRenderer = false;
            {
                std::lock_guard<std::mutex> lk(streamMtx);
                if (!snapDirty)
                    return;
                acts = snapActions;
                ress = snapResults;
                response = snapResponse;
                raw = snapRaw;
                thought = snapThought;
                phase = snapPhase;
                clearRenderer = snapClearRenderer;
                snapClearRenderer = false;
                snapDirty = false;
            }

            if (clearRenderer)
                renderer.clear();
            streamActionCount = acts.size();
            streamResultCount = ress.size();
            streamRespBytes = response.size();
            streamRawBytes = raw.size();
            streamPhase = phase;

            while (lastAct < acts.size()) {
                const auto& a = acts[lastAct++];
                renderer.addProtocolAction(a.type, a.name, a.id, a.body, a.sync);
                renderDirty = true;
            }
            while (lastRes < ress.size()) {
                const auto& r = ress[lastRes++];
                std::string tn = r.toolName;
                if (tn.empty()) {
                    for (const auto& a : acts)
                        if (a.id == r.id) {
                            tn = a.name;
                            break;
                        }
                }
                renderer.addProtocolResult(r.id, r.ok, r.summary, tn, r.exitCode, r.elapsedMs,
                                           r.outputBytes);
                renderDirty = true;
            }
            if (renderer.mode() != tui::RenderMode::FULL)
                renderer.setRawStream(raw);
            renderer.setResponse(response);
            if (!response.empty())
                renderDirty = true;
            if (phase == "waiting provider" || phase == "parsing protocol") {
                renderer.setThought(thought);
                if (!thought.empty())
                    renderDirty = true;
            }
        };

        // Run agent in background thread. The provider callback must never render
        // or wait on terminal I/O; it only snapshots agent state for the TUI loop.
        std::thread agentThread([&]() {
            agent.prompt(
                promptText,
                [&](const std::string& /*token*/, bool) {
                    if (!cortex::mk3::g_running)
                        return;
                    auto& acts = agent.protocolActions();
                    auto& ress = agent.protocolResults();
                    const std::string& response = agent.responseOutput();
                    const std::string& raw = agent.rawLlOutput();
                    std::string phase = "waiting provider";
                    if (!acts.empty() && ress.size() < acts.size())
                        phase = "running tools";
                    else if (!response.empty())
                        phase = "streaming response";
                    else if (!raw.empty())
                        phase = "parsing protocol";
                    {
                        std::lock_guard<std::mutex> lk(streamMtx);
                        if (firstToken) {
                            snapClearRenderer = true;
                            firstToken = false;
                        }
                        snapActions = acts;
                        snapResults = ress;
                        snapResponse = response;
                        snapRaw = raw;
                        snapThought = agent.thoughtOutput();
                        snapPhase = phase;
                        snapDirty = true;
                    }
                },
                cli.sessionId, cli.ephemeral);
            {
                std::lock_guard<std::mutex> lk(streamMtx);
                snapActions = agent.protocolActions();
                snapResults = agent.protocolResults();
                snapResponse = agent.responseOutput();
                snapRaw = agent.rawLlOutput();
                snapThought = agent.thoughtOutput();
                snapPhase = "complete";
                snapDirty = true;
            }
            agentDone.store(true, std::memory_order_release);
        });

        // Main loop: poll input + render concurrently with agent
        while (!agentDone.load(std::memory_order_acquire) && cortex::mk3::g_running && !quit) {
            // ── ask_tool: pick up a pending dialog from the agent thread ──
            if (!dialogActive.load(std::memory_order_acquire) &&
                askPending.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lk(askMtx);
                if (askParamsReady) {
                    askDialog->active = true;
                    askDialog->complete = false;
                    askDialog->cancelled = false;
                    askDialog->params = askParams;
                    askDialog->state = cortex::mk3::tui::parseDialogState(askParams);
                    cortex::mk3::tui::completeNonInteractiveCards(askDialog->state);
                    if (askDialog->state.done()) {
                        askDialog->complete = true;
                        askDialog->result = askDialog->state.results;
                        askCv.notify_one();
                    } else {
                        dialogActive.store(true, std::memory_order_release);
                        input.clearEscape();
                        renderDirty = true;
                        // ── Dialog input interceptor: j/k/arrows navigate,
                        //    y/n for confirm, everything else passes through ──
                        input.setActionInterceptor([&](int act, char outChar) -> bool {
                            if (!dialogActive.load(std::memory_order_acquire))
                                return false;
                            const cortex::mk3::tui::DialogCard* card = askDialog->state.current();
                            if (!card)
                                return false;

                            // j / down → next option
                            if ((act == (int)cortex::mk3::tui::KeyAction::CHAR &&
                                 (outChar == 'j' || outChar == 'J')) ||
                                act == (int)cortex::mk3::tui::KeyAction::HISTORY_DOWN) {
                                if (card->type == "choice" || card->type == "multi_choice" ||
                                    card->type == "ranker") {
                                    if (askDialog->state.selectedOption <
                                        (int)card->options.size() - 1)
                                        askDialog->state.selectedOption++;
                                    renderDirty = true;
                                    return true;
                                }
                            }
                            // k / up → prev option
                            if ((act == (int)cortex::mk3::tui::KeyAction::CHAR &&
                                 (outChar == 'k' || outChar == 'K')) ||
                                act == (int)cortex::mk3::tui::KeyAction::HISTORY_UP) {
                                if (card->type == "choice" || card->type == "multi_choice" ||
                                    card->type == "ranker") {
                                    if (askDialog->state.selectedOption > 0)
                                        askDialog->state.selectedOption--;
                                    renderDirty = true;
                                    return true;
                                }
                            }
                            // y/n → immediate confirm (no Enter needed)
                            if (card->type == "confirm" &&
                                act == (int)cortex::mk3::tui::KeyAction::CHAR) {
                                if (outChar == 'y' || outChar == 'Y') {
                                    cortex::mk3::tui::advanceDialog(askDialog->state, true);
                                    renderDirty = true;
                                    if (askDialog->state.done()) {
                                        askDialog->complete = true;
                                        askDialog->result = askDialog->state.results;
                                        askDialog->active = false;
                                        dialogActive.store(false, std::memory_order_release);
                                        input.clearActionInterceptor();
                                                        renderDirty = true;
                                        askCv.notify_one();
                                    }
                                    return true;
                                }
                                if (outChar == 'n' || outChar == 'N') {
                                    cortex::mk3::tui::advanceDialog(askDialog->state, false);
                                    renderDirty = true;
                                    if (askDialog->state.done()) {
                                        askDialog->complete = true;
                                        askDialog->result = askDialog->state.results;
                                        askDialog->active = false;
                                        dialogActive.store(false, std::memory_order_release);
                                        input.clearActionInterceptor();
                                                        renderDirty = true;
                                        askCv.notify_one();
                                    }
                                    return true;
                                }
                            }
                            // Block slash commands, search, scroll, etc. during dialog
                            if (act == (int)cortex::mk3::tui::KeyAction::SEARCH ||
                                act == (int)cortex::mk3::tui::KeyAction::SCROLL_UP ||
                                act == (int)cortex::mk3::tui::KeyAction::SCROLL_DOWN ||
                                act == (int)cortex::mk3::tui::KeyAction::CLEAR_SCREEN ||
                                act == (int)cortex::mk3::tui::KeyAction::TAB)
                                return true;
                            // Let everything else through (text typing, Enter, Backspace)
                            return false;
                        });
                    }
                }
            }

            // Poll input non-blocking. Keystrokes must redraw immediately even
            // while the provider is waiting on first byte.
            std::string beforeInput = input.line();
            size_t beforeCursor = input.cursorPos();
            bool hadInput = input.poll();
            bool inputChanged = (input.line() != beforeInput || input.cursorPos() != beforeCursor);
            if (inputChanged)
                renderDirty = true;

            // Escape → cancel dialog (if active) or cancel agent
            if (input.escapePressed()) {
                if (dialogActive.load(std::memory_order_acquire)) {
                    askDialog->cancelled = true;
                    askDialog->active = false;
                    dialogActive.store(false, std::memory_order_release);
                    input.clearActionInterceptor();
                    renderDirty = true;
                    input.clearEscape();
                    renderDirty = true;
                    askCv.notify_one();
                } else {
                    cortex::mk3::g_running = false;
                    input.clearEscape();
                }
            }

            // Dialog input: route Enter to the dialog, not the prompt buffer
            if (dialogActive.load(std::memory_order_acquire) && hadInput) {
                std::string line = dialogInputLine;
                dialogInputLine.clear();
                bool done = cortex::mk3::tui::handleDialogLine(askDialog->state, line);
                input.clearEscape();
                renderDirty = true;
                if (done) {
                    askDialog->complete = true;
                    askDialog->result = askDialog->state.results;
                    askDialog->active = false;
                    dialogActive.store(false, std::memory_order_release);
                    input.clearActionInterceptor();
                    renderDirty = true;
                    askCv.notify_one();
                }
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
                    termW = ws.ws_col;
                    termH = ws.ws_row;
                    renderer.setWidth(termW);
                    sessionView.setWidthHeight(termW, termH);
                    renderDirty = true;
                }
            }

            applyStreamSnapshot();
            renderScreen();
            redrawStatusOnly(inputChanged);

            // Small sleep to avoid busy-wait CPU spin; skip on any input activity.
            if (!hadInput && !inputChanged)
                usleep(2000);
        }

        applyStreamSnapshot();

        // Wait for agent thread to finish
        if (agentThread.joinable())
            agentThread.join();

        // Flush final render state
        applyStreamSnapshot();
        renderScreen();

        if (!cortex::mk3::g_running) {
            streaming = false;
            std::cout << "\033[" << termH - 1 << ";1H\033[2K" << ansi::red << "Cancelled"
                      << ansi::reset;
            std::cout << "\033[" << termH << ";1H\033[2K" << ansi::bold << "▸ " << ansi::reset
                      << "\033[2m\033[3m";
            size_t cp = input.cursorPos();
            std::string l = input.line();
            std::cout << l.substr(0, cp);
            std::cout << "\033[7m" << (cp < l.size() ? std::string(1, l[cp]) : " ") << "\033[27m";
            if (cp < l.size())
                std::cout << l.substr(cp + 1);
            std::cout << ansi::reset << " ";
            std::cout << std::flush;
            continue;
        }

        // Archive this turn's rendered output to history
        auto turnLines = renderer.render();
        // User prompt already in historyLines (added before streaming started)
        historyLines.insert(historyLines.end(), turnLines.begin(), turnLines.end());
        if (historyLines.empty())
            historyLines.push_back("");
        streaming = false;
        if (!cli.tuiDebugDumpPath.empty()) {
            dumpTuiState(cli.tuiDebugDumpPath, "turn-complete", false);
        }
        renderer.clear();
        streamPhase = "idle";
        renderDirty = true;
        // AC15 — agent.prompt() already persisted the turn via Agent::saveSession.
        // Writing here again with a separate `sess` clobbered tool-call records;
        // refresh the local copy from disk instead so display/list stays current.
        sess = sm.load(sessionId);
        renderScreen();
    }

    input.stop();
    input.history().save(histPath);
    // No final save — agent owns persistence.
    std::cout << "\nBye.\n";
    return 0;
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
    signal(SIGWINCH, signalHandler);

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
    if (cli.listSessions)
        return cmdSessions();
    if (cli.command == "list")
        return cmdList(cli);
    if (cli.command == "config")
        return cmdConfig(cli);
    if (cli.command == "completions")
        return cmdCompletions(cli);
    if (cli.command == "serve")
        return cmdServe(cli);

    // Default: run
    return cmdRun(cli);
}
