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

#include "src/core/agent.hpp"
#include "src/core/agent_catalog.hpp"
#include "src/core/manifest_autoload.hpp"
#include "src/core/manifest_loader.hpp"
#include "src/core/sandbox_launcher.hpp"
#include "src/providers/factory.hpp"
#include "src/sandbox/policy.hpp"
#include "src/session/manager.hpp"
#include "src/tui/dialog.hpp"
#include "src/tui/manifest_manager.hpp"
#include "src/tui/repl_session.hpp"
#include "src/tui/frame_clock.hpp"
#include "src/tui/input.hpp"
#include "src/tui/renderer.hpp"
#include "src/tui/session_view.hpp"
#include "src/tui/slash_commands.hpp"
#include "src/tui/status_prompt.hpp"
#include "src/ui/app/mk3_tui_app.hpp"
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
    bool manifestPickerRequested = false;  // bare -m / --manifest → manager
    bool listAgents = false;               // list --agents
    int iterations = 0;
    std::string sessionId;
    bool continueSession = false;
    bool resumePicker = false;
    std::string systemPromptPath;
    std::string harnessPromptPath;
    std::string personaPath;
    // Session + lifecycle flags are ORTHOGONAL and stackable:
    //   --no-session  → do not load/save a session record
    //   --ephemeral   → exit when the current agent turn finishes
    //   -p / --prompt → seed/run this prompt (does not imply either above)
    bool noSession = false;
    bool ephemeral = false;
    // Chat render modifiers (orthogonal to -p / session / lifecycle).
    bool showThoughts = true;     // --no-thoughts to hide Thought rows
    bool truncateBodies = true;   // --no-truncate for full bodies
    bool raw = false;
    bool toolAnsi = true;
    bool replMode = false;
    std::string tuiMode;  // legacy | inkcell | experimental (default from MK3_TUI or legacy)
    std::string tuiDebugDumpPath;
    std::string sessionName;    // --name <name>: human-readable session label
    std::string forkFrom;       // --fork <id>: copy an existing session and continue
    int showHistory = -1;       // --show-history N: render last N records on startup.
                                //   -1 (default) = auto: 5 on resume, 0 otherwise.
                                //    0 disables; positive overrides.
    bool sessionBanner = true;  // print resume banner (suppress with --quiet-session)

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
    bool listSessionsFlag = false;  // list --sessions

    // Completions
    std::string completionsShell;

    // Sessions subcommand
    std::string sessionsSubcommand;  // "list" | "show" | "rm" | "export" | ""
    std::string sessionsTarget;      // session id (or file path for export/import)
    std::string sessionsTargetArg;   // extra arg (e.g. export file path)

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
  --manifest-dir <dir> Extra manifests/ root (global surface is manifests/ only)
  -m, --manifest [name|path]  Agent under manifests/agents; bare -m opens manager
  --agent [name|path]  Alias for --manifest
  --iterations <n>     Max turns before forced response (default: 20)
  --provider <name>    LLM provider (deepseek, openrouter, xai, openai-codex, groq, zen, together, fireworks)
  --model <name>       Model name
  --sandbox            Enable sandbox mode (tool restrictions)
  --sandbox-ro         Read-only sandbox (no writes, restricted exec)
  --verbose, -V        Verbose: dump full prompts each iteration
  --debug              Enable debug output
  --raw                Pipe-clean output (no formatting, no banner)
  --no-tool-ansi       Strip ANSI/color escapes from tool result rendering
  -c, --continue       Continue previous session
  -r, --resume         Select a session to resume
  --session <id>       Use specific session id
  --name <name>        Human-readable session label (persisted in metadata)
  --fork <id>          Copy an existing session and continue under a new id
  --show-history N     Render the last N records of the resumed session on startup
  --quiet-session      Suppress the resume banner (printed to stderr)
  --no-session         Don't load/save a session record
  --ephemeral          Exit when the agent turn finishes
  --tui <legacy|inkcell|experimental>
                       Select TUI backend. legacy/inkcell use ReplSession oracle;
                       experimental uses the inkcell-native chat port (env: MK3_TUI)
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
  -p, --prompt <text>    Seed/run this prompt (orthogonal to session/lifecycle flags).
                         With experimental TUI: stays interactive unless --ephemeral.
                         Headless/legacy path: runs once and returns.
  -f, --file <path>      Read prompt from file
  -m, --manifest [name|path]  Agent name (catalog) or path; bare -m opens manager
  --agent [name|path]    Alias for --manifest
  --harness <size|path>  Protocol harness: small|medium|big|default or file path
  --system <path>        System prompt override
  --session <id>         Use specific session id
  -c, --continue         Continue previous session
  -r, --resume           Select a session to resume
  --no-session           Don't load/save a session record
  --ephemeral            Exit when the agent turn finishes (NOT the same as --no-session)
  --thoughts / --no-thoughts   Show/hide Thought rows (default: show)
  --truncate / --no-truncate   Cap long bodies pi-style (default: on)
  --no-tool-ansi         Strip ANSI/color escapes from tool result rendering
  --repl                 Force interactive mode even with --prompt
  --tui <legacy|inkcell|experimental> Select TUI backend
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
  --agents           List agents under manifests/ with ownership trees
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
    if (cli.tuiMode.empty()) {
        const char* envTui = std::getenv("MK3_TUI");
        cli.tuiMode = (envTui && envTui[0]) ? std::string(envTui) : get("tui", "legacy");
    }
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
                                       {"no-tool-ansi", no_argument, 0, 1004},
                                       {"tui", required_argument, 0, 1002},
                                       {"tui-debug-dump", required_argument, 0, 1001},
                                       {"dry-run", no_argument, 0, 'n'},
                                       {"help", no_argument, 0, 'h'},

                                       // Run
                                       {"prompt", required_argument, 0, 'p'},
                                       {"file", required_argument, 0, 'f'},
                                       {"manifest", optional_argument, 0, 'm'},
                                       {"agent", optional_argument, 0, 'm'},
                                       {"harness", required_argument, 0, 'H'},
                                       {"system", required_argument, 0, 'y'},
                                       {"session", required_argument, 0, 's'},
                                       {"continue", no_argument, 0, 'c'},
                                       {"resume", no_argument, 0, 'r'},
                                       {"name", required_argument, 0, 1010},
                                       {"fork", required_argument, 0, 1011},
                                       {"show-history", required_argument, 0, 1012},
                                       {"quiet-session", no_argument, 0, 1013},
                                       {"no-session", no_argument, 0, 'e'},
                                       {"ephemeral", no_argument, 0, 1035},
                                       {"repl", no_argument, 0, 'E'},
                                       {"thoughts", no_argument, 0, 1030},
                                       {"no-thoughts", no_argument, 0, 1031},
                                       {"truncate", no_argument, 0, 1032},
                                       {"no-truncate", no_argument, 0, 1033},

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
                                       {"agents", no_argument, 0, 1021},
                                       {"sessions", no_argument, 0, 1020},

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
            } else if (arg == "--agents") {
                cli.listAgents = true;
            } else if (arg == "--sessions") {
                cli.listSessionsFlag = true;
            } else if (arg == "--help" || arg == "-h") {
                cli.showHelp = true;
            }
        }
        return cli;
    }

    int opt;
    bool sawProviderFlag = false;
    bool providerFlagHadArg = false;
    bool sawManifestFlag = false;
    bool manifestFlagHadArg = false;
    while ((opt = getopt_long(argc, argv, "C:G:P::M:p:f:m::H:y:s:VhrSReEDnX:c", longOpts, nullptr)) !=
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
                cli.resumePicker = true;
                break;
            case 1003:
                cli.raw = true;
                break;
            case 1004:
                cli.toolAnsi = false;
                break;
            case 1001:
                cli.tuiDebugDumpPath = optarg;
                break;
            case 1002:
                cli.tuiMode = optarg;
                break;
            case 1010:
                cli.sessionName = optarg;
                break;
            case 1011:
                cli.forkFrom = optarg;
                break;
            case 1012:
                cli.showHistory = std::atoi(optarg);
                if (cli.showHistory < 0)
                    cli.showHistory = 0;
                break;
            case 1013:
                cli.sessionBanner = false;
                break;
            case 1020:
                cli.listSessionsFlag = true;
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
                sawManifestFlag = true;
                if (optarg) {
                    manifestFlagHadArg = true;
                    cli.manifestPath = optarg;
                    cli.manifestPickerRequested = false;
                } else {
                    // bare -m / --manifest → manager; may be normalized below
                    // if a name follows as a separate argv token.
                    cli.manifestPickerRequested = true;
                }
                break;
            case 1021:
                cli.listAgents = true;
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
            case 'e':
                cli.noSession = true;
                break;
            case 1035:
                cli.ephemeral = true;  // exit when turn finishes — NOT the same as --no-session
                break;
            case 'E':
                cli.replMode = true;
                break;
            case 1030:
                cli.showThoughts = true;
                break;
            case 1031:
                cli.showThoughts = false;
                break;
            case 1032:
                cli.truncateBodies = true;
                break;
            case 1033:
                cli.truncateBodies = false;
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

    // Same for --manifest / -m: optional args only attach as -mname or --manifest=name.
    // `-m default` leaves "default" at optind — treat as agent name, not manager.
    if (sawManifestFlag && !manifestFlagHadArg && optind < argc && argv[optind][0] != '-') {
        cli.manifestPath = argv[optind];
        cli.manifestPickerRequested = false;
        optind++;
    }

    // Subcommand (first positional after flags)
    if (optind < argc) {
        std::string cmd = argv[optind];
        if (cmd == "run" || cmd == "serve" || cmd == "list" || cmd == "config" ||
            cmd == "completions" || cmd == "version" || cmd == "help" || cmd == "sessions") {
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

    // Sessions subcommand: sessions [list|show <id>|rm <id>|export <id> <file>]
    if (cli.command == "sessions") {
        if (optind < argc) {
            std::string sub = argv[optind++];
            if (sub == "list" || sub == "ls") {
                cli.sessionsSubcommand = "list";
            } else if (sub == "show" || sub == "cat") {
                cli.sessionsSubcommand = "show";
                if (optind < argc)
                    cli.sessionsTarget = argv[optind++];
            } else if (sub == "rm" || sub == "remove" || sub == "delete") {
                cli.sessionsSubcommand = "rm";
                if (optind < argc)
                    cli.sessionsTarget = argv[optind++];
            } else if (sub == "export") {
                cli.sessionsSubcommand = "export";
                if (optind < argc)
                    cli.sessionsTarget = argv[optind++];
                if (optind < argc)
                    cli.sessionsTargetArg = argv[optind++];
            } else if (!sub.empty() && sub[0] != '-') {
                // Default: `sessions <id>` shows it
                cli.sessionsSubcommand = "show";
                cli.sessionsTarget = sub;
            } else {
                cli.sessionsSubcommand = "list";
            }
        } else {
            cli.sessionsSubcommand = "list";
        }
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
static std::vector<session::SessionManager::SessionInfo> sortedSessions() {
    session::SessionManager sm;
    auto sessions = sm.list();
    std::sort(sessions.begin(), sessions.end(),
              [](const auto& a, const auto& b) { return a.updated > b.updated; });
    return sessions;
}

static std::string slugPart(std::string s) {
    for (char& c : s) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_';
        if (!ok)
            c = '-';
    }
    while (!s.empty() && s.front() == '-')
        s.erase(s.begin());
    while (!s.empty() && s.back() == '-')
        s.pop_back();
    return s.empty() ? "project" : s;
}

static std::string newSessionId() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    std::string project = slugPart(fs::current_path().filename().string());
    std::ostringstream id;
    id << project << "-" << ms;
    return id.str();
}

static std::string humanTime(const std::string& iso);
static std::string humanAge(const std::string& iso);
static std::string pickSessionInteractive(bool defaultIfEmpty) {
    auto sessions = sortedSessions();
    if (sessions.empty())
        return defaultIfEmpty ? newSessionId() : "";
    if (!isatty(STDIN_FILENO))
        return sessions[0].id;

    // ── Raw mode for j/k/arrow navigation ──
    struct termios oldt;
    tcgetattr(STDIN_FILENO, &oldt);
    struct termios raw = oldt;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    std::cout << tui::ansi::hideCursor() << tui::ansi::clearScreen() << tui::ansi::moveTo(1, 1)
              << std::flush;

    auto restore = [&]() {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
        std::cout << tui::ansi::showCursor() << tui::ansi::clearScreen() << tui::ansi::moveTo(1, 1)
                  << "\n"
                  << std::flush;
    };

    auto readKey = [&]() -> std::pair<tui::KeyAction, char> {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        select(STDIN_FILENO + 1, &fds, nullptr, nullptr, nullptr);
        char buf[64];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0)
            return {tui::KeyAction::NONE, 0};
        std::string seq(buf, buf + n);
        if (seq[0] == 27 && seq.size() == 1) {
            struct timeval tv = {0, 30000};
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
                char buf2[64];
                ssize_t n2 = read(STDIN_FILENO, buf2, sizeof(buf2));
                if (n2 > 0)
                    seq.append(buf2, n2);
            }
        }
        char outChar = 0;
        tui::KeyMap keymap;
        tui::KeyAction act = keymap.resolve(seq, outChar);
        return {act, outChar};
    };

    // Load metadata for each session.
    struct PickerRow {
        std::string id;
        std::string shortId;  // last 16 chars
        std::string name;
        std::string age;  // "2h ago"
        std::string agentName;
        size_t turnCount = 0;
        std::string model;
    };
    std::vector<PickerRow> rows;
    rows.reserve(sessions.size());
    session::SessionManager sm;
    for (const auto& s : sessions) {
        PickerRow r;
        r.id = s.id;
        r.shortId = s.id.size() > 16 ? "…" + s.id.substr(s.id.size() - 15) : s.id;
        r.name = s.agentName;
        r.age = humanAge(s.updated);
        if (r.age.empty())
            r.age = s.updated.substr(11, 5);  // HH:MM
        r.agentName = s.agentName;
        r.turnCount = s.turnCount;
        r.model = s.model;
        if (sm.exists(s.id)) {
            Session loaded = sm.load(s.id);
            auto it = loaded.metadata.find("name");
            if (it != loaded.metadata.end() && !it->second.empty()) {
                r.name = it->second;
            }
        }
        rows.push_back(std::move(r));
    }

    // Render with viewport scrolling: show up to 20 rows starting at `offset`.
    const size_t VIEW_H = 20;
    size_t offset = 0;
    int sel = 0;

    auto renderPicker = [&]() {
        std::cout << tui::ansi::clearScreen() << tui::ansi::moveTo(1, 1);
        // Header
        std::cout << "\033[1;36m┌─ Resume session\033[0m  \033[2m" << rows.size()
                  << " total — j/k or ↑↓ move, 1-9 jump, d/u page, g/G top/bottom, "
                     "Enter select, q/Esc cancel\033[0m\r\n";
        size_t end = std::min(offset + VIEW_H, rows.size());
        for (size_t i = offset; i < end; ++i) {
            const auto& r = rows[i];
            bool isSel = (int)i == sel;
            std::string num = std::to_string(i + 1);
            // Pad number for alignment
            while (num.size() < 3)
                num = " " + num;
            std::string numColor = isSel ? "\033[1;36m" : "\033[2;34m";
            std::string shortId = r.shortId;
            while (shortId.size() < 16)
                shortId = " " + shortId;
            std::string agePad = r.age;
            while (agePad.size() < 9)
                agePad = " " + agePad;
            std::string msg = std::to_string(r.turnCount) + " msg";
            while (msg.size() < 7)
                msg = " " + msg;
            // Truncate fields to fit in a typical 100-col terminal.
            std::string model = r.model;
            if (model.size() > 28)
                model = model.substr(0, 25) + "…";
            std::string name = r.name;
            if (name.size() > 24)
                name = name.substr(0, 21) + "…";
            if (isSel) {
                std::cout << "\033[1;36m│\033[0m  \033[7;36m" << num << " " << shortId << "  "
                          << agePad << "  " << msg << "  " << std::left << std::setw(28) << model
                          << "  " << std::setw(24) << name << "\033[0m\r\n";
            } else {
                std::cout << "\033[2m│\033[0m  " << numColor << num << "\033[0m " << shortId
                          << "  \033[2m" << agePad << "  " << msg << "  \033[0m" << std::left
                          << std::setw(28) << model << "  \033[3m" << std::setw(24) << name
                          << "\033[0m\r\n";
            }
        }
        // Footer with scroll position
        if (rows.size() > VIEW_H) {
            size_t pos = offset + 1;
            size_t total = rows.size();
            std::cout << "\033[2m└─ showing " << pos << "–" << end << " of " << total
                      << "\033[0m\r\n";
        } else {
            std::cout << "\033[2m└─ " << rows.size() << " session" << (rows.size() == 1 ? "" : "s")
                      << "\033[0m\r\n";
        }
        std::cout << std::flush;
    };

    bool picked = false;
    std::string selectedId;
    while (!picked) {
        // Keep cursor in view
        if (sel < (int)offset)
            offset = sel;
        if (sel >= (int)(offset + VIEW_H))
            offset = sel - VIEW_H + 1;
        renderPicker();
        auto [act, ch] = readKey();
        if (act == tui::KeyAction::ENTER) {
            picked = true;
            selectedId = rows[sel].id;
        } else if (act == tui::KeyAction::CANCEL || act == tui::KeyAction::EXIT ||
                   (act == tui::KeyAction::CHAR && (ch == 'q' || ch == 'Q'))) {
            restore();
            // User explicitly cancelled — never mint a new session.
            return "";
        } else if (act == tui::KeyAction::HISTORY_DOWN ||
                   (act == tui::KeyAction::CHAR && ch == 'j')) {
            sel = (sel + 1) % (int)rows.size();
        } else if (act == tui::KeyAction::HISTORY_UP ||
                   (act == tui::KeyAction::CHAR && ch == 'k')) {
            sel = (sel - 1 + (int)rows.size()) % (int)rows.size();
        } else if (act == tui::KeyAction::CHAR && ch == 'd') {
            // Page down
            sel = std::min((int)rows.size() - 1, sel + (int)VIEW_H);
        } else if (act == tui::KeyAction::CHAR && ch == 'u') {
            // Page up
            sel = std::max(0, sel - (int)VIEW_H);
        } else if (act == tui::KeyAction::CHAR && ch >= '1' && ch <= '9') {
            int idx = ch - '1';
            if (idx < (int)rows.size())
                sel = idx;
        } else if (act == tui::KeyAction::CHAR && ch == 'g') {
            sel = 0;
        } else if (act == tui::KeyAction::CHAR && ch == 'G') {
            sel = (int)rows.size() - 1;
        }
    }

    restore();
    return selectedId;
}

static std::string resolveSessionId(const CliConfig& cli, bool defaultIfEmpty) {
    if (cli.resumePicker) {
        std::string picked = pickSessionInteractive(defaultIfEmpty);
        // If the user cancelled the picker, picked is "". Don't fall back to a
        // brand-new session — the caller treats empty as "no session" and
        // exits cleanly.
        return picked;
    }
    if (cli.continueSession) {
        auto sessions = sortedSessions();
        if (sessions.empty())
            return defaultIfEmpty ? newSessionId() : "";
        // Prefer the most recent session that actually has an agent reply
        // (not just a user prompt). A bare user prompt usually means the user
        // exited before the LLM finished — resuming that just shows a
        // half-typed hello and a frozen prompt, which is worse than resuming
        // the previous real session.
        session::SessionManager sm;
        for (const auto& s : sessions) {
            auto loaded = sm.load(s.id);
            for (const auto& r : loaded.records) {
                if (r.role == SessionRecord::AGENT || r.role == SessionRecord::TOOL_CALL ||
                    r.role == SessionRecord::TOOL_RESULT) {
                    return s.id;
                }
            }
        }
        return sessions[0].id;
    }
    if (!cli.sessionId.empty())
        return cli.sessionId;
    return defaultIfEmpty ? newSessionId() : "";
}

static void applySessionMetadata(CliConfig& cli, const std::string& sessionId) {
    if (sessionId.empty())
        return;
    session::SessionManager sm;
    if (!sm.exists(sessionId))
        return;
    auto session = sm.load(sessionId);
    auto get = [&](const std::string& key) -> std::string {
        auto it = session.metadata.find(key);
        return it == session.metadata.end() ? "" : it->second;
    };
    if (cli.manifestPath.empty())
        cli.manifestPath = get("manifest_path");
    if (cli.harnessPromptPath.empty())
        cli.harnessPromptPath = get("harness_path");
    if (cli.systemPromptPath.empty())
        cli.systemPromptPath = get("system_prompt_path");
    if (cli.personaPath.empty())
        cli.personaPath = get("persona_path");
    if (!cli.providerSet && !get("provider").empty())
        cli.provider = get("provider");
    if (!cli.modelSet && !get("model").empty())
        cli.model = get("model");
}

static void persistSessionMetadata(const std::string& sessionId, const CliConfig& cli,
                                   const AgentConfig& acfg) {
    if (sessionId.empty())
        return;
    session::SessionManager sm;
    auto session = sm.exists(sessionId)
                       ? sm.load(sessionId)
                       : sm.create(sessionId, acfg.name, acfg.model, acfg.provider);
    session.agentName = acfg.name;
    session.model = acfg.model;
    session.provider = acfg.provider;
    session.metadata["cwd"] = fs::current_path().string();
    session.metadata["provider"] = acfg.provider;
    session.metadata["model"] = acfg.model;
    if (!cli.manifestPath.empty())
        session.metadata["manifest_path"] = cli.manifestPath;
    if (!acfg.harnessPath.empty())
        session.metadata["harness_path"] = acfg.harnessPath;
    if (!acfg.systemPromptPath.empty())
        session.metadata["system_prompt_path"] = acfg.systemPromptPath;
    if (!acfg.personaPath.empty())
        session.metadata["persona_path"] = acfg.personaPath;
    if (!cli.sessionName.empty())
        session.metadata["name"] = cli.sessionName;
    else if (session.metadata.count("name"))
        session.metadata.erase("name");
    session.updated = session::SessionManager::iso8601();
    sm.save(session);
}

// ═══════════════════════════════════════════════════════════════════════
// Resume banner — printed to stderr so stdout stays clean for piping
// ═══════════════════════════════════════════════════════════════════════
static std::string humanTime(const std::string& iso) {
    if (iso.empty())
        return "unknown";
    // Accept "YYYY-MM-DDTHH:MM:SSZ" or "YYYY-MM-DDTHH:MM:SS.fffZ".
    if (iso.size() >= 16) {
        std::string out = iso.substr(0, 16);
        if (out.size() > 10)
            out[10] = ' ';
        return out;
    }
    return iso;
}

static std::string humanAge(const std::string& iso) {
    if (iso.empty())
        return "";
    std::tm tm{};
    if (iso.size() < 19 || !strptime(iso.substr(0, 19).c_str(), "%Y-%m-%dT%H:%M:%S", &tm))
        return "";
    auto t = timegm(&tm);
    auto now = std::time(nullptr);
    auto diff = static_cast<long>(now - t);
    if (diff < 0)
        return "in the future";
    if (diff < 60)
        return std::to_string(diff) + "s ago";
    if (diff < 3600)
        return std::to_string(diff / 60) + "m ago";
    if (diff < 86400)
        return std::to_string(diff / 3600) + "h ago";
    return std::to_string(diff / 86400) + "d ago";
}

static void printResumeBanner(const std::string& sessionId, const std::string& kind,
                              size_t messageCount = 0, const std::string& forkSource = "") {
    if (sessionId.empty())
        return;
    session::SessionManager sm;
    auto session = sm.load(sessionId);
    std::string name = session.metadata.count("name") ? session.metadata.at("name") : "";
    std::string agent = session.agentName;
    std::string model = session.model;
    std::string provider = session.provider;
    std::string manifest =
        session.metadata.count("manifest_path") ? session.metadata.at("manifest_path") : "";
    std::string created = humanTime(session.created);
    std::string updated = humanTime(session.updated);
    std::string age = humanAge(session.updated);
    size_t turns = messageCount > 0 ? messageCount : session.records.size();

    std::cerr << "\033[2m[session]\033[0m \033[1m" << kind << " session: " << sessionId
              << "\033[0m";
    if (!name.empty())
        std::cerr << " (\033[3m" << name << "\033[0m)";
    std::cerr << "\n";
    if (!forkSource.empty())
        std::cerr << "\033[2m[session]\033[0m   forked from: " << forkSource << "\n";
    if (!agent.empty())
        std::cerr << "\033[2m[session]\033[0m   agent:     " << agent << "\n";
    if (!provider.empty() || !model.empty())
        std::cerr << "\033[2m[session]\033[0m   model:     " << provider << "/" << model << "\n";
    if (!manifest.empty())
        std::cerr << "\033[2m[session]\033[0m   manifest:  " << manifest << "\n";
    std::cerr << "\033[2m[session]\033[0m   created:   " << created << "\n";
    std::cerr << "\033[2m[session]\033[0m   updated:   " << updated << " (" << age << ")\n";
    std::cerr << "\033[2m[session]\033[0m   messages:  " << turns << "\n";
    if (turns > 0)
        std::cerr
            << "\033[2m[session]\033[0m Last records will render in the TUI above the prompt.\n";
    else
        std::cerr << "\033[2m[session]\033[0m (no records — this is an empty session)\n";
    std::cerr << "\033[2m[session]\033[0m To start fresh: \033[1mcortex-mk3 --no-session\033[0m\n";
}

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
        // Also clear the state checkpoint if present.
        fs::path statePath =
            fs::current_path() / ".cortex" / "state" / (cli.sessionsTarget + ".json");
        std::error_code ec;
        fs::remove(statePath, ec);
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
    // ── Gather providers ──
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
// Global manifest manager (bare -m / --manifest)
// ═══════════════════════════════════════════════════════════════════════
// Returns selected absolute agent.yml path, or empty on cancel.
// ═══════════════════════════════════════════════════════════════════════
// Global manifest manager (bare -m / --manifest)
// ═══════════════════════════════════════════════════════════════════════
static std::string interactiveManifestManager(const std::string& manifestDirOverride) {
    return tui::runManifestManager(manifestDirOverride);
}

static bool resolveCliManifest(CliConfig& cli) {
    if (cli.manifestPickerRequested && cli.manifestPath.empty()) {
        std::string picked = interactiveManifestManager(cli.manifestDir);
        if (picked.empty()) {
            std::cerr << "\033[2m[manifest] cancelled.\033[0m\n";
            return false;
        }
        cli.manifestPath = picked;
        cli.manifestPickerRequested = false;
        std::cerr << "\033[2m[manifest]\033[0m " << cli.manifestPath << "\n";
        return true;
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
        Session src = sm.load(cli.forkFrom);
        std::string newId = newSessionId();
        Session fork = sm.create(newId, src.agentName, src.model, src.provider);
        fork.records = src.records;
        fork.contextFeeds = src.contextFeeds;
        fork.metadata = src.metadata;
        if (!cli.sessionName.empty())
            fork.metadata["name"] = cli.sessionName;
        fork.metadata["forked_from"] = cli.forkFrom;
        fork.updated = session::SessionManager::iso8601();
        sm.save(fork);
        forkedFrom = cli.forkFrom;
        cli.sessionId = newId;
        cli.forkFrom.clear();
        didResume = true;  // fork is a form of resume (copied history)
    }

    if (!cli.noSession && (cli.resumePicker || cli.continueSession || !cli.sessionId.empty())) {
        std::string resolved = resolveSessionId(cli, true);
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

    if (cli.tuiMode != "legacy" && cli.tuiMode != "inkcell" && cli.tuiMode != "experimental") {
        std::cerr << "Error: unknown TUI backend '" << cli.tuiMode
                  << "' (expected legacy|inkcell|experimental)\n";
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
    tui::TuiRenderer renderer(80);
    renderer.setToolAnsiPassthrough(cli.toolAnsi);

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
                : (activeSessionId.empty() ? resolveSessionId(cli, true) : activeSessionId);
        if (!cli.noSession)
            persistSessionMetadata(promptSessionId, cli, acfg);

        if (cli.tuiMode == "experimental") {
            ui::InkcellAppConfig icfg;
            icfg.agentName = acfg.name;
            icfg.provider = acfg.provider;
            icfg.model = acfg.model;
            icfg.manifestPath = cli.manifestPath;
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

        Spinner spinner;
        if (!cli.raw) {
            printBanner();
            spinner.start("Thinking...");
        }

        std::string result = agent.prompt(cli.prompt, promptSessionId, cli.noSession);
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

    // ── Interactive experimental inkcell app ──
    // Supports both the dashboard (no -m: startAtDashboard = true) and direct
    // chat (-m: startAtDashboard = false). It is now the default landing
    // surface for a bare `cortex-mk3` (no prompt, no manifest) so the operator
    // opens into the control surface instead of the legacy REPL. Explicit
    // `--tui experimental` continues to route here. With a -m manifest, the
    // app goes straight to the agent scene; with a -p prompt in one-shot mode,
    // runInkcellOneShot (above) is used instead.
    if (cli.tuiMode == "experimental" ||
        (cli.prompt.empty() && cli.manifestPath.empty())) {
        std::string experimentalSessionId =
            cli.noSession
                ? ""
                : (activeSessionId.empty() ? resolveSessionId(cli, true) : activeSessionId);
        if (!cli.noSession)
            persistSessionMetadata(experimentalSessionId, cli, acfg);
        ui::InkcellAppConfig icfg;
        icfg.agentName = acfg.name;
        icfg.provider = acfg.provider;
        icfg.model = acfg.model;
        icfg.manifestPath = cli.manifestPath;
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

    // ── Interactive REPL TUI / chat oracle ──
    // legacy and inkcell still route through ReplSession.
    std::string replSessionId =
        cli.noSession
            ? ""
            : (activeSessionId.empty() ? resolveSessionId(cli, true) : activeSessionId);
    if (!cli.noSession)
        persistSessionMetadata(replSessionId, cli, acfg);

    tui::ReplSessionConfig replCfg;
    replCfg.provider = acfg.provider;
    replCfg.model = acfg.model;
    replCfg.sessionId = replSessionId;
    replCfg.sessionName = cli.sessionName;
    replCfg.tuiDebugDumpPath = cli.tuiDebugDumpPath;
    replCfg.workflowXml = workflowXml;
    replCfg.allSchemas = allSchemas;
    replCfg.ephemeral = cli.noSession;  // ReplSession: ephemeral == no session persist
    replCfg.toolAnsi = cli.toolAnsi;
    replCfg.resizedFlag = &g_resized;
    return tui::ReplSession(std::move(replCfg)).run(agent);
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
}
