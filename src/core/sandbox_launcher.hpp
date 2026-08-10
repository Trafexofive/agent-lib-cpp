// ─────────────────────────────────────────────────────────────────────────────
// Sandbox launcher + process-mode bind materialization
//
// sandbox.bind / sandbox.files make host paths available inside the agent
// environment for live CRUD (reflects on host). RO binds are enforced by
// SandboxPolicy and by docker -v :ro.
//
// process: glorified symlinks (guest → host) under a writable guest parent,
//          plus a path-rewrite table on the policy for absolute guests.
// docker:  real bind mounts via `docker run -v host:guest[:ro]`
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "types.hpp"

namespace cortex {
namespace mk3 {
namespace sandbox {

namespace fs = std::filesystem;

inline bool dockerAvailable() {
    return system("docker info >/dev/null 2>&1") == 0;
}

// Parse "host:guest" or "host:guest:ro" (last :ro is the only flag we accept).
// host may be relative; guest should be absolute for docker, relative ok for process.
inline bool parseBindSpec(const std::string& spec, SandboxBind& out) {
    if (spec.empty())
        return false;
    // Find last ":ro" / ":rw" flag
    std::string body = spec;
    out.readOnly = false;
    if (body.size() > 3) {
        auto tail = body.substr(body.size() - 3);
        if (tail == ":ro") {
            out.readOnly = true;
            body = body.substr(0, body.size() - 3);
        } else if (tail == ":rw") {
            out.readOnly = false;
            body = body.substr(0, body.size() - 3);
        }
    }
    // Split host:guest on the last colon that leaves a non-empty guest.
    // Windows drives are not a concern on our Linux target.
    auto colon = body.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= body.size()) {
        // bare path → host only; guest derived later
        out.host = body;
        out.guest.clear();
        return !out.host.empty();
    }
    out.host = body.substr(0, colon);
    out.guest = body.substr(colon + 1);
    return !out.host.empty() && !out.guest.empty();
}

inline std::string shellEscape(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

// Expand sandbox.files shorthand into binds: host → /workspace/<basename>
inline void expandFilesToBinds(AgentConfig& cfg) {
    for (const auto& f : cfg.sandboxFiles) {
        if (f.empty())
            continue;
        SandboxBind b;
        b.host = f;
        b.guest = (fs::path("/workspace") / fs::path(f).filename()).string();
        b.readOnly = cfg.sandboxReadonly;
        // Avoid dup if already in binds
        bool exists = false;
        for (const auto& e : cfg.sandboxBinds) {
            if (e.host == b.host && e.guest == b.guest) {
                exists = true;
                break;
            }
        }
        if (!exists)
            cfg.sandboxBinds.push_back(b);
    }
}

// Resolve relative host paths against manifestDir (or CWD).
inline void resolveBindHosts(AgentConfig& cfg, const fs::path& baseDir) {
    for (auto& b : cfg.sandboxBinds) {
        if (b.host.empty())
            continue;
        fs::path h(b.host);
        if (h.is_relative())
            h = baseDir / h;
        std::error_code ec;
        fs::path canon = fs::weakly_canonical(h, ec);
        b.host = ec ? h.lexically_normal().string() : canon.string();
        if (b.guest.empty()) {
            b.guest = (fs::path("/workspace") / fs::path(b.host).filename()).string();
        }
    }
}

struct MaterializeResult {
    bool ok = true;
    std::vector<std::string> created;  // symlink paths created
    std::vector<std::string> warnings;
    std::string error;
};

// Process-mode: create symlinks so guest paths resolve to host (live CRUD).
// Absolute guests under /workspace/... land as ./workspace/... relative to CWD
// only when guest is relative; absolute guests outside CWD rely on policy rewrite.
inline MaterializeResult materializeProcessBinds(const AgentConfig& cfg,
                                                 const fs::path& workspaceRoot) {
    MaterializeResult r;
    std::error_code ec;
    fs::create_directories(workspaceRoot, ec);

    for (const auto& b : cfg.sandboxBinds) {
        if (b.host.empty() || b.guest.empty())
            continue;

        if (!fs::exists(b.host, ec)) {
            r.warnings.push_back("bind host missing: " + b.host + " → " + b.guest);
            // Still create parent + symlink target may be dangling — useful for late create.
        }

        fs::path guestPath(b.guest);
        fs::path linkPath;
        if (guestPath.is_absolute()) {
            // Prefer placing under workspace mirror for absolute container-style guests:
            // /workspace/foo → <workspaceRoot>/foo
            // /home/ctx      → <workspaceRoot>/.sandbox-binds/home/ctx  + policy rewrite still
            // maps /home/ctx → host. Also try direct symlink if parent is writable.
            std::string g = guestPath.lexically_normal().string();
            const std::string wsPrefix = "/workspace/";
            if (g == "/workspace") {
                linkPath = workspaceRoot;
                // workspace root itself — nothing to symlink; host should be workspace
                continue;
            }
            if (g.rfind(wsPrefix, 0) == 0) {
                linkPath = workspaceRoot / g.substr(wsPrefix.size());
            } else {
                // Mirror absolute guest under .sandbox-binds for discoverability.
                std::string stripped = g;
                while (!stripped.empty() && stripped[0] == '/')
                    stripped.erase(stripped.begin());
                linkPath = workspaceRoot / ".sandbox-binds" / stripped;

                // Also attempt a direct symlink at the absolute guest when possible
                // (dev machines where /home/ctx etc. is writable).
                fs::path direct = guestPath;
                fs::create_directories(direct.parent_path(), ec);
                if (!ec) {
                    if (fs::exists(direct, ec) || fs::is_symlink(direct, ec)) {
                        if (fs::is_symlink(direct, ec)) {
                            fs::remove(direct, ec);
                        } else {
                            r.warnings.push_back("bind guest exists (not symlink), skip direct: " +
                                                 direct.string());
                            direct.clear();
                        }
                    }
                    if (!direct.empty()) {
                        fs::create_directory_symlink(b.host, direct, ec);
                        if (ec) {
                            // file symlink fallback
                            ec.clear();
                            fs::create_symlink(b.host, direct, ec);
                        }
                        if (!ec) {
                            r.created.push_back(direct.string());
                        } else {
                            r.warnings.push_back("direct symlink failed " + direct.string() +
                                                 ": " + ec.message());
                            ec.clear();
                        }
                    }
                } else {
                    ec.clear();
                }
            }
        } else {
            linkPath = workspaceRoot / guestPath;
        }

        if (linkPath.empty())
            continue;

        fs::create_directories(linkPath.parent_path(), ec);
        ec.clear();
        if (fs::exists(linkPath, ec) || fs::is_symlink(linkPath, ec)) {
            if (fs::is_symlink(linkPath, ec)) {
                fs::remove(linkPath, ec);
            } else if (fs::equivalent(linkPath, b.host, ec)) {
                continue;  // already the same inode
            } else {
                r.warnings.push_back("bind guest path occupied: " + linkPath.string());
                continue;
            }
        }
        ec.clear();
        // Prefer directory_symlink; falls back to symlink for files.
        if (fs::is_directory(b.host, ec) || !fs::exists(b.host, ec)) {
            ec.clear();
            fs::create_directory_symlink(b.host, linkPath, ec);
            if (ec) {
                ec.clear();
                fs::create_symlink(b.host, linkPath, ec);
            }
        } else {
            fs::create_symlink(b.host, linkPath, ec);
        }
        if (ec) {
            r.warnings.push_back("symlink " + linkPath.string() + " → " + b.host + " failed: " +
                                 ec.message());
            ec.clear();
            continue;
        }
        r.created.push_back(linkPath.string());
    }
    return r;
}

inline std::string dockerNetworkFlag(const std::string& network) {
    if (network == "none")
        return " --network=none";
    if (network == "full" || network == "host")
        return " --network=host";
    // out = default bridge
    return "";
}

// Build a Docker image for the agent and launch it with live bind mounts.
inline int launchDocker(const std::string& manifestPath, const AgentConfig& cfgIn,
                        const std::vector<std::string>& /*legacyFiles*/) {
    if (!dockerAvailable()) {
        std::cerr << "Docker is not available. Install Docker or use sandbox.mode: process\n";
        return 1;
    }

    AgentConfig cfg = cfgIn;
    expandFilesToBinds(cfg);
    fs::path manifestDir = fs::path(manifestPath).parent_path();
    resolveBindHosts(cfg, manifestDir.empty() ? fs::current_path() : manifestDir);

    std::string imageName = "cortex-agent-" + cfg.name;
    std::string dockerfile = "/tmp/cortex-dockerfile-" + cfg.name;
    std::string image = cfg.sandboxImage.empty()
                            ? (cfg.sandboxRuntime.empty() ? "ubuntu:24.04" : cfg.sandboxRuntime)
                            : cfg.sandboxImage;

    // Generate Dockerfile — binary + manifest only; project data comes via -v
    {
        std::ofstream df(dockerfile);
        df << "FROM " << image << "\n";
        df << "RUN if command -v apt-get >/dev/null 2>&1; then \\\n";
        df << "      apt-get update && apt-get install -y --no-install-recommends \\\n";
        df << "        libcurl4 libjsoncpp26 ca-certificates grep coreutils findutils \\\n";
        df << "        && rm -rf /var/lib/apt/lists/*; \\\n";
        df << "    fi\n";
        df << "RUN id agent >/dev/null 2>&1 || "
              "useradd -ms /bin/bash agent; "
              "mkdir -p /workspace /opt/cortex && chown -R agent:agent /workspace /opt/cortex\n";
        df << "COPY cortex-mk3 /usr/local/bin/cortex-mk3\n";
        df << "COPY " << fs::path(manifestPath).filename().string() << " /opt/cortex/agent.yml\n";
        // Copy whole manifest directory for persona/tools relative imports
        df << "COPY . /opt/cortex/manifest/\n";
        df << "USER agent\n";
        df << "WORKDIR /workspace\n";
        df << "ENTRYPOINT [\"/usr/local/bin/cortex-mk3\"]\n";
        df << "CMD [\"-m\", \"/opt/cortex/agent.yml\"]\n";
    }

    // Build context: temp dir with binary + manifest tree
    std::string ctx = "/tmp/cortex-docker-ctx-" + cfg.name + "-" + std::to_string(::getpid());
    {
        std::error_code ec;
        fs::remove_all(ctx, ec);
        fs::create_directories(ctx, ec);
        // copy binary
        fs::path self;
        {
            char buf[4096];
            ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = 0;
                self = buf;
            }
        }
        if (self.empty() || !fs::exists(self)) {
            std::cerr << "Cannot locate cortex-mk3 binary for docker build\n";
            return 1;
        }
        fs::copy_file(self, fs::path(ctx) / "cortex-mk3", fs::copy_options::overwrite_existing, ec);
        // copy manifest file + directory contents
        fs::copy_file(manifestPath, fs::path(ctx) / fs::path(manifestPath).filename(),
                      fs::copy_options::overwrite_existing, ec);
        // shallow copy of manifest dir (tools, prompts, etc.)
        for (auto& ent : fs::directory_iterator(manifestDir, ec)) {
            const auto dest = fs::path(ctx) / ent.path().filename();
            if (ent.is_directory())
                fs::copy(ent.path(), dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            else if (ent.is_regular_file())
                fs::copy_file(ent.path(), dest, fs::copy_options::overwrite_existing, ec);
        }
        fs::copy_file(dockerfile, fs::path(ctx) / "Dockerfile", fs::copy_options::overwrite_existing,
                      ec);
    }

    std::string buildCmd = "docker build -t " + shellEscape(imageName) + " " + shellEscape(ctx) +
                           " 2>&1";
    std::cout << "Building Docker image " << imageName << "...\n";
    int rc = system(buildCmd.c_str());
    if (rc != 0) {
        std::cerr << "Docker build failed. Context: " << ctx << " Dockerfile: " << dockerfile
                  << "\n";
        return 1;
    }

    // Run with live binds
    std::ostringstream run;
    run << "docker run -it --rm";
    run << dockerNetworkFlag(cfg.sandboxNetwork);
    run << " -e DEEPSEEK_API_KEY -e OPENROUTER_API_KEY -e GROQ_API_KEY -e OPENAI_API_KEY";
    run << " -e XAI_API_KEY -e CORTEX_DEV_MODE";

    for (const auto& b : cfg.sandboxBinds) {
        if (b.host.empty() || b.guest.empty())
            continue;
        std::string mount = b.host + ":" + b.guest;
        if (b.readOnly || cfg.sandboxReadonly)
            mount += ":ro";
        run << " -v " << shellEscape(mount);
    }

    // Always mount CWD to /workspace if no explicit /workspace bind
    bool hasWorkspace = false;
    for (const auto& b : cfg.sandboxBinds) {
        if (b.guest == "/workspace" || b.guest.rfind("/workspace/", 0) == 0) {
            hasWorkspace = true;
            break;
        }
    }
    if (!hasWorkspace) {
        std::string mount = fs::current_path().string() + ":/workspace";
        if (cfg.sandboxReadonly)
            mount += ":ro";
        run << " -v " << shellEscape(mount);
    }

    run << " " << shellEscape(imageName);
    std::cout << "Launching: " << run.str() << "\n";
    return system(run.str().c_str());
}

}  // namespace sandbox
}  // namespace mk3
}  // namespace cortex
