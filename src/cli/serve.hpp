#pragma once
// serve subcommand (points at the dedicated server binary).

#include <iostream>
#include <string>

#include "src/cli/options.hpp"

namespace cortex::mk3::cli {

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

}  // namespace cortex::mk3::cli
