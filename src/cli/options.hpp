#pragma once
// CLI option parsing, config merge, and help for cortex-mk3.

#include <getopt.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace cortex::mk3::cli {

namespace fs = std::filesystem;

static const char* VERSION = "3.1.0";
// CLI state
// ═══════════════════════════════════════════════════════════════════════
struct CliConfig {
    // Subcommand
    std::string command;  // "run", "serve", "list", "config", "completions", "version", "help"

    // Provider
    std::string provider = "openai-codex";
    std::string model;  // empty → provider's defaultModel is used
    bool providerSet = false;   // explicit CLI --provider
    bool modelSet = false;      // explicit CLI --model
    // Set by applySessionMetadata when session file carried engine fields.
    // Distinguishes session restore from config-file defaults (config must NOT
    // beat agent.yml or session on -c).
    bool providerFromSession = false;
    bool modelFromSession = false;
    bool providerPickerRequested = false;  // --provider with no arg, no model

    // Run mode
    std::string prompt;
    std::string promptFile;
    std::string manifestPath;
    std::string manifestDir;
    // Filled by applySessionMetadata on resume — agent name from the session
    // file when manifest_path was missing (used for resolve + banner).
    std::string sessionAgentName;
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
    bool noAnsi = false;  // --no-ansi: strip escapes from headless output (implies --no-tool-ansi)
    bool replMode = false;
    std::string tuiMode;  // experimental (alias: inkcell); default: experimental (MK3_TUI / config)
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
void printHelpGeneral() {
    std::cout << R"(Usage: cortex-mk3 [global-flags] <command> [command-flags]

Global flags:
  --config <path>      Config file (default: ~/.config/cortex-mk3/config)
  --manifest-dir <dir> Extra manifests/ root (global surface is manifests/ only)
  -m, --manifest [name|path]  Agent under manifests/agents; bare -m opens the manifest browser
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
  --no-ansi            Strip ANSI from headless --ephemeral output (implies --no-tool-ansi)
  -c, --continue       Continue previous session
  -r, --resume         Select a session to resume
  --session <id>       Use specific session id
  --name <name>        Human-readable session label (persisted in metadata)
  --fork <id>          Copy an existing session and continue under a new id
  --show-history N     Render the last N records of the resumed session on startup
  --quiet-session      Suppress the resume banner (printed to stderr)
  --no-session         Don't load/save a session record
  --ephemeral          Exit when the agent turn finishes
  --tui <experimental|inkcell>
                       TUI backend (env: MK3_TUI). Default: experimental.
                       inkcell = alias of experimental (native inkcell App).
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
                         With inkcell/experimental TUI: stays interactive unless --ephemeral.
                         Headless/legacy path: runs once and returns.
  -f, --file <path>      Read prompt from file
  -m, --manifest [name|path]  Agent name (catalog) or path; bare -m opens the manifest browser
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
  --tui <legacy|inkcell|experimental>  inkcell≡experimental (native); legacy=oracle
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
        cli.tuiMode = (envTui && envTui[0]) ? std::string(envTui) : get("tui", "experimental");
    }
    // Honest alias: --tui inkcell is the native inkcell App (not ReplSession).
    if (cli.tuiMode == "inkcell")
        cli.tuiMode = "experimental";
    // Legacy backend was removed; env/config values fall back softly.
    if (cli.tuiMode == "legacy") {
        std::cerr << "warning: --tui legacy was removed — using inkcell (experimental).\n";
        cli.tuiMode = "experimental";
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
                                       {"no-ansi", no_argument, 0, 1005},
                                       {"tui", required_argument, 0, 1002},
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
            case 1005:
                cli.noAnsi = true;
                break;
            case 1002:
                if (std::strcmp(optarg, "legacy") == 0) {
                    std::cerr << "Error: --tui legacy was removed — inkcell is the only TUI "
                                 "backend (use --tui experimental or --tui inkcell)\n";
                    std::exit(1);
                }
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

    // GNU getopt only consumes optional option args with --opt=value. Space
    // forms (`-m default`, `--provider opencode-go`) leave names at optind.
    // Claim free tokens carefully: BOTH flags may be waiting — do NOT always
    // give the first free token to provider (bug: `-m default --provider X`
    // assigned provider=default, agent=X).
    {
        auto isFlagTok = [](const char* s) { return s && s[0] == '-'; };
        auto looksLikeProvider = [](const std::string& t) {
            static const char* kKnown[] = {
                "deepseek",    "openrouter", "xai",         "x-ai",
                "openai-codex","openai",     "groq",        "zen",
                "together",    "fireworks",  "opencode",    "opencode-go",
                "minimax",     "anthropic",  "google",      nullptr};
            for (int i = 0; kKnown[i]; ++i)
                if (t == kKnown[i]) return true;
            // dotted vendor/model routes still providers when used as --provider
            if (t.find('/') != std::string::npos) return true;
            return false;
        };

        // Gather contiguous free tokens at optind (until a subcommand/flag).
        std::vector<std::string> free;
        int freeStart = optind;
        while (freeStart + static_cast<int>(free.size()) < argc &&
               !isFlagTok(argv[freeStart + static_cast<int>(free.size())])) {
            std::string t = argv[freeStart + static_cast<int>(free.size())];
            if (t == "run" || t == "serve" || t == "list" || t == "config" ||
                t == "completions" || t == "version" || t == "help" ||
                t == "sessions")
                break;
            free.push_back(t);
        }

        int used = 0;
        auto takeAt = [&](size_t idx) -> std::string {
            if (idx >= free.size()) return {};
            std::string v = free[idx];
            free[idx].clear(); // mark consumed
            ++used;
            return v;
        };

        // Prefer matching shapes when both flags need a free arg.
        if (sawManifestFlag && !manifestFlagHadArg) {
            int pick = -1;
            for (size_t i = 0; i < free.size(); ++i) {
                if (free[i].empty()) continue;
                if (!looksLikeProvider(free[i])) { pick = static_cast<int>(i); break; }
            }
            if (pick < 0) {
                for (size_t i = 0; i < free.size(); ++i)
                    if (!free[i].empty()) { pick = static_cast<int>(i); break; }
            }
            if (pick >= 0) {
                cli.manifestPath = takeAt(static_cast<size_t>(pick));
                cli.manifestPickerRequested = false;
            }
        }
        if (sawProviderFlag && !providerFlagHadArg) {
            int pick = -1;
            for (size_t i = 0; i < free.size(); ++i) {
                if (free[i].empty()) continue;
                if (looksLikeProvider(free[i])) { pick = static_cast<int>(i); break; }
            }
            if (pick < 0) {
                for (size_t i = 0; i < free.size(); ++i)
                    if (!free[i].empty()) { pick = static_cast<int>(i); break; }
            }
            if (pick >= 0) {
                cli.provider = takeAt(static_cast<size_t>(pick));
                cli.providerSet = true;
                cli.providerPickerRequested = false;
            }
        }
        // Advance optind past consumed free tokens (preserve order holes).
        int advanced = 0;
        for (size_t i = 0; i < free.size(); ++i) {
            if (free[i].empty())
                ++advanced;
            else
                break; // stop at first unconsumed — left for subcommand/prompt
        }
        // If we consumed non-prefix holes, compact: only safe when all free claimed
        bool allClaimed = true;
        for (const auto& t : free)
            if (!t.empty()) allClaimed = false;
        if (allClaimed)
            optind = freeStart + static_cast<int>(free.size());
        else if (advanced > 0)
            optind = freeStart + advanced;
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

}  // namespace cortex::mk3::cli
