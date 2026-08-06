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
#include "src/cli/commands.hpp"
#include "src/cli/run.hpp"
#include "src/cli/serve.hpp"
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
