// ─────────────────────────────────────────────────────────────────────────────
// Docker Sandbox Launcher — builds and runs agent in a Docker container
// Seamless: user just passes --manifest, gets dropped into container CLI
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <string>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace cortex {
namespace mk3 {
namespace sandbox {

namespace fs = std::filesystem;

inline bool dockerAvailable() {
    return system("docker info >/dev/null 2>&1") == 0;
}

// Build a Docker image for the agent and launch it interactively
inline int launchDocker(const std::string& manifestPath, const AgentConfig& cfg,
                         const std::vector<std::string>& files) {
    if (!dockerAvailable()) {
        std::cerr << "Docker is not available. Install Docker or use sandbox.mode: process\n";
        return 1;
    }

    fs::path manifestDir = fs::path(manifestPath).parent_path();
    std::string imageName = "cortex-agent-" + cfg.name;
    std::string dockerfile = "/tmp/cortex-dockerfile-" + cfg.name;

    // Generate Dockerfile
    {
        std::ofstream df(dockerfile);
        df << "FROM ubuntu:24.04\n";
        df << "RUN apt-get update && apt-get install -y --no-install-recommends \\\n";
        df << "    libcurl4 libjsoncpp26 ca-certificates grep coreutils findutils \\\n";
        df << "    && rm -rf /var/lib/apt/lists/*\n";
        df << "RUN useradd -ms /bin/bash agent && mkdir -p /workspace && chown agent:agent /workspace\n";

        // Copy agent binary
        df << "COPY cortex-mk3 /usr/local/bin/cortex-mk3\n";

        // Copy manifest + files
        df << "COPY " << manifestPath << " /opt/cortex/agent.yml\n";
        for (auto& f : files) {
            if (fs::exists(f)) {
                df << "COPY " << f << " /workspace/" << fs::path(f).filename().string() << "\n";
            }
        }
        // Copy manifest directory for persona, tools, etc
        df << "COPY " << manifestDir.string() << " /opt/cortex/manifest/\n";

        df << "USER agent\n";
        df << "WORKDIR /workspace\n";
        df << "ENTRYPOINT [\"/usr/local/bin/cortex-mk3\"]\n";
        df << "CMD [\"-m\", \"/opt/cortex/agent.yml\"]\n";
    }

    // Build image
    std::string buildCmd = "docker build -t " + imageName + " -f " + dockerfile + " . 2>&1";
    std::cout << "Building Docker image " << imageName << "...\n";
    int rc = system(buildCmd.c_str());
    if (rc != 0) {
        std::cerr << "Docker build failed. Check " << dockerfile << "\n";
        return 1;
    }

    // Launch interactively
    std::string runCmd = "docker run -it --rm "
        "-e DEEPSEEK_API_KEY "
        "-e OPENROUTER_API_KEY "
        "-e GROQ_API_KEY "
        + imageName;
    return system(runCmd.c_str());
}

} // namespace sandbox
} // namespace mk3
} // namespace cortex
