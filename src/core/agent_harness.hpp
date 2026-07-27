#pragma once
// Harness path resolution (ctor of Agent).

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace cortex::mk3 {

inline std::string findHarnessPath(const std::string &fromManifest,
                                   std::vector<std::string> &looked) {
    auto tryOpen = [](const std::string &cand,
                      std::vector<std::string> &looked) -> std::string {
        looked.push_back(cand);
        std::ifstream f(cand);
        if (f.good())
            return cand;
        return {};
    };
    auto appendIf = [](std::string base,
                       const std::string &tail) -> std::string {
        if (base.empty())
            return tail;
        if (base.back() != '/')
            base += '/';
        return base + tail;
    };
    const std::string hintRel = [&]() -> std::string {
        if (fromManifest.empty())
            return "default.md";
        std::string stem = fromManifest;
        size_t slash = stem.find_last_of('/');
        if (slash != std::string::npos)
            stem = stem.substr(slash + 1);
        size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos)
            stem = stem.substr(0, dot);
        if (stem.empty())
            return "default.md";
        return stem + ".md";
    }();
    auto tryRoot = [&](const std::string &root) -> std::string {
        std::string cand =
            appendIf(root, appendIf("manifests/harness", hintRel));
        return tryOpen(cand, looked);
    };
    auto tryRootDefault = [&](const std::string &root) -> std::string {
        std::string cand = appendIf(root, "manifests/harness/default.md");
        return tryOpen(cand, looked);
    };
    // Compute the FHS-install prefix path exactly once and cache; called
    // in two places (specific + default.md fallback). Each call returns
    // either the resolved path or empty; populates `looked` so the
    // error message below tells the operator where we looked. Static
    // helper — no IIFE recursion, no self-capture UB path.
    auto tryFhsInstall = [&](const std::string &suffix) -> std::string {
        std::error_code ec;
        auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (ec || self.empty())
            return {};
        std::string p = self.string();
        size_t slash = p.find_last_of('/');
        if (slash == std::string::npos)
            return {};
        std::string bindir = p.substr(0, slash);
        size_t last = bindir.find_last_of('/');
        if (last == std::string::npos)
            return {};
        std::string prefix = bindir.substr(0, last);
        std::string cand = prefix + "/share/cortex-mk3/" + suffix;
        looked.push_back(cand);
        std::ifstream f(cand);
        if (f.good())
            return cand;
        return {};
    };
    auto suffixFor = [&](const std::string &relOrDefault) -> std::string {
        return std::string("manifests/harness/") + relOrDefault;
    };
    // 1. Exactly what the manifest loader provided first.
    if (!fromManifest.empty() &&
        fromManifest.find("default.md") == std::string::npos) {
        if (auto r = tryOpen(fromManifest, looked); !r.empty())
            return r;
    }
    // 2. CORTEX_HOME
    if (const char *home = std::getenv("CORTEX_HOME"); home && *home) {
        if (auto r = tryRoot(home); !r.empty())
            return r;
    }
    // 2'. Exe-relative: the binary lives at <install>/cortex-mk3 and
    // shares an install tree with manifests/. When launched from any
    // cwd (e.g. ~) this is the only reliable way to find the harness
    // without a CORTEX_HOME hint.
    try {
        std::error_code ec;
        auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec && !self.empty()) {
            std::string exe = self.string();
            size_t slash = exe.find_last_of('/');
            if (slash != std::string::npos) {
                std::string exeDir = exe.substr(0, slash);
                if (auto r = tryRoot(exeDir); !r.empty())
                    return r;
            }
        }
    } catch (...) {
    }
    // 2''. FHS-style install root: <prefix>/share/cortex-mk3/manifests/...
    // for binaries installed via pacman / apt / a future install script.
    if (auto r = tryFhsInstall(suffixFor(hintRel)); !r.empty())
        return r;
    // 3. cwd-relative (any cwd, not hardcoded developer box)
    tryRoot(std::filesystem::current_path().string());
    // 4. ~/.config/cortex-mk3 (installed layout)
    if (const char *home = std::getenv("HOME"); home && *home) {
        tryRoot(std::string(home) + "/.config/cortex-mk3");
    }
    // Final fallback: default.md in same roots, in the same order.
    if (hintRel != "default.md") {
        if (const char *home = std::getenv("CORTEX_HOME"); home && *home) {
            if (auto r = tryRootDefault(home); !r.empty())
                return r;
        }
        // Same FHS install sibling fallback for default.md.
        if (auto r = tryFhsInstall(suffixFor("default.md")); !r.empty())
            return r;
        // Same exe-dir fallback as above for default.md.
        try {
            std::error_code ec;
            auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
            if (!ec && !self.empty()) {
                std::string exe = self.string();
                size_t slash = exe.find_last_of('/');
                if (slash != std::string::npos) {
                    std::string exeDir = exe.substr(0, slash);
                    if (auto r = tryRootDefault(exeDir); !r.empty())
                        return r;
                }
            }
        } catch (...) {
        }
        tryRootDefault(std::filesystem::current_path().string());
        if (const char *home = std::getenv("HOME"); home && *home) {
            tryRootDefault(std::string(home) + "/.config/cortex-mk3");
        }
    }
    return {};
}


}  // namespace cortex::mk3
