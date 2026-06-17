// ─────────────────────────────────────────────────────────────────────────────
// Feed Engine — polls system feeds and injects context into agent prompts
// Now delegates to sovereign Feed objects (src/feeds/feed.hpp).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "feed.hpp"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <sys/utsname.h>
#include <unistd.h>
#include <fstream>
#include <cstdio>
#include <filesystem>
#include <json/json.h>

namespace cortex::mk3::feeds {

// ═══════════════════════════════════════════════════════════════════════════
// FeedEngine — orchestrates Feed objects, provides manifest loading
// ═══════════════════════════════════════════════════════════════════════════
class FeedEngine {
public:
    static FeedEngine& instance() {
        static FeedEngine engine;
        return engine;
    }

    ~FeedEngine() {
        if (refreshThread_.joinable()) refreshThread_.join();
    }

    // ── Registration ──

    /// Register a Feed object (sovereign)
    bool registerFeed(Feed&& feed) {
        if (!feed.isValid()) return false;
        if (has(feed.name())) return false;  // dedup
        feeds_[feed.name()] = std::move(feed);
        return true;
    }

    /// Register a feed by name + poll function (backward-compat convenience)
    void registerFeed(const std::string& name, FeedFn fn) {
        if (has(name)) return;  // dedup
        Feed feed(name, std::move(fn), true);  // poll immediate
        feeds_[name] = std::move(feed);

        // Async refresh (legacy behavior — keeps the refresh thread pattern)
        if (refreshThread_.joinable()) refreshThread_.join();
        refreshThread_ = std::thread([this, name]() {
            auto it = feeds_.find(name);
            if (it != feeds_.end()) {
                it->second.poll();
            }
        });
    }

    /// Check if a feed is registered
    bool has(const std::string& name) const {
        return feeds_.find(name) != feeds_.end();
    }

    /// Get a feed by name (returns nullptr if not found)
    const Feed* getFeed(const std::string& name) const {
        auto it = feeds_.find(name);
        return (it != feeds_.end()) ? &it->second : nullptr;
    }

    // ── Polling ──

    /// Poll all feeds and return their results
    std::vector<FeedResult> pollAll() {
        std::vector<FeedResult> results;
        for (auto& [name, feed] : feeds_) {
            results.push_back(feed.poll());
        }
        return results;
    }

    /// Poll a single feed by name. Returns empty result if not found.
    FeedResult pollOne(const std::string& name, bool forceFresh = false) {
        auto it = feeds_.find(name);
        if (it == feeds_.end()) return {name, "unknown feed", "{}", false};
        if (forceFresh) return it->second.poll();
        return it->second.get();
    }

    /// Get all cached results without polling
    std::vector<FeedResult> getAllCached() const {
        std::vector<FeedResult> results;
        for (const auto& [name, feed] : feeds_) {
            results.push_back(feed.cached());
        }
        return results;
    }

    /// List registered feed names
    std::vector<std::string> listFeeds() const {
        std::vector<std::string> names;
        for (const auto& [name, _] : feeds_) names.push_back(name);
        return names;
    }

    /// Feed count
    size_t count() const { return feeds_.size(); }

    // ── Prompt injection ──

    /// Format all feeds as a Markdown section for prompt injection
    std::string injectIntoPrompt() {
        if (feeds_.empty()) return "";
        std::ostringstream ss;
        ss << "\n## System Feeds\n";
        bool wrote = false;
        for (auto& [name, feed] : feeds_) {
            FeedResult r = feed.get();
            if (!r.ok) continue;
            wrote = true;
            ss << "### " << name << "\n";
            if (!r.summary.empty()) ss << r.summary << "\n";
        }
        return wrote ? ss.str() : "";
    }

    // ── Manifest loading ──

    struct ManifestResult {
        bool success = false;
        std::string name;
        std::string summary;
        std::string error;
    };

    ManifestResult loadFeedManifest(const std::string& path) {
        ManifestResult mr;
        std::ifstream f(path);
        if (!f) { mr.error = "cannot read manifest"; return mr; }

        // Parse minimal YAML (kind: Feed, name:, runtime:, entrypoint:)
        std::string line, name, runtime, entrypoint;
        while (std::getline(f, line)) {
            size_t colon = line.find(": ");
            if (colon == std::string::npos) continue;
            std::string key = line.substr(0, colon);
            size_t start = key.find_first_not_of(" \t");
            if (start != std::string::npos) key = key.substr(start);
            std::string val = line.substr(colon + 2);
            if (key == "name") name = val;
            else if (key == "runtime") runtime = val;
            else if (key == "entrypoint") entrypoint = val;
        }

        if (name.empty()) { mr.error = "no name in manifest"; return mr; }
        if (runtime.empty()) runtime = "builtin";

        mr.name = name;

        // Resolve entrypoint relative to manifest
        std::filesystem::path scriptPath;
        if (!entrypoint.empty()) {
            std::filesystem::path manifestDir = std::filesystem::path(path).parent_path();
            scriptPath = manifestDir / entrypoint;
        }

        // Execute and capture output — pass CALL_TOOL env var so scripts can invoke tools
        std::string output;
        if (runtime == "builtin") {
            mr.success = true;
            mr.summary = "builtin feed " + name;
            return mr;
        }

        // Set CALL_TOOL env for script subprocess — enables feed scripts to call tools
        std::string callToolPath = findCallTool();
        if (runtime == "python3" || runtime == "python") {
            output = runScriptWithEnv("python3", scriptPath.string(), callToolPath);
        } else if (runtime == "bash" || runtime == "sh") {
            output = runScriptWithEnv("bash", scriptPath.string(), callToolPath);
        } else if (runtime == "node") {
            output = runScriptWithEnv("node", scriptPath.string(), callToolPath);
        } else {
            mr.error = "unknown runtime: " + runtime;
            return mr;
        }

        if (output.empty()) {
            registerFeed(name, [name]() -> FeedResult {
                return {name, "", "{}", true};
            });
            mr.success = true;
            mr.summary = "";
            return mr;
        }

        // Parse JSON output from script
        Json::Value parsed;
        Json::CharReaderBuilder r;
        std::string errs;
        std::istringstream ss(output);
        if (!Json::parseFromStream(r, ss, &parsed, &errs)) {
            registerFeed(name, [name, output]() -> FeedResult {
                return {name, output, "{}", true};
            });
            mr.success = true;
            mr.summary = output;
            return mr;
        }

        // Register feed that re-executes on each poll (with tool-call support)
        std::string toolPath = callToolPath;
        auto pollFn = [name, runtime, scriptPath, toolPath]() -> FeedResult {
            FeedResult fr;
            fr.name = name;
            fr.ok = true;
            std::string out = runScriptWithEnvStatic(runtime, scriptPath.string(), toolPath);
            if (out.empty()) {
                fr.summary = "(empty)";
                return fr;
            }
            Json::Value p;
            Json::CharReaderBuilder r2;
            std::string e2;
            std::istringstream ss2(out);
            if (!Json::parseFromStream(r2, ss2, &p, &e2)) {
                fr.summary = out;
                return fr;
            }
            std::ostringstream sum;
            for (auto& key : p.getMemberNames()) {
                if (!sum.str().empty()) sum << "\n";
                if (p[key].isString()) sum << key << ": " << p[key].asString();
                else {
                    Json::StreamWriterBuilder wb;
                    wb["indentation"] = "";
                    sum << key << ": " << Json::writeString(wb, p[key]);
                }
            }
            fr.summary = sum.str();
            fr.json = out;
            return fr;
        };

        registerFeed(name, pollFn);
        mr.success = true;
        mr.summary = "loaded feed with tool-call support from " + scriptPath.string();
        return mr;
    }

private:
    std::map<std::string, Feed> feeds_;
    std::thread refreshThread_;

    // ── Script execution helpers (kept for manifest loading) ──

    static std::string runScript(const std::string& runtime, const std::string& script) {
        return runScriptStatic(runtime, script);
    }

    static std::string runScriptWithEnv(const std::string& runtime, const std::string& script,
                                         const std::string& callToolPath) {
        return runScriptWithEnvStatic(runtime, script, callToolPath);
    }

    static std::string runScriptStatic(const std::string& runtime, const std::string& script) {
        return runScriptWithEnvStatic(runtime, script, "");
    }

    static std::string runScriptWithEnvStatic(const std::string& runtime, const std::string& script,
                                                const std::string& callToolPath) {
        std::string cmd = runtime + " " + script + " 2>/dev/null";
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) return "";

        // Set CALL_TOOL env for the subprocess
        if (!callToolPath.empty()) {
            setenv("CALL_TOOL", callToolPath.c_str(), 1);
        }

        std::string output;
        char buf[4096];
        while (fgets(buf, sizeof(buf), p)) output += buf;
        pclose(p);
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
            output.pop_back();
        return output;
    }

    static std::string findCallTool() {
        // Look for call-tool binary in same directory as cortex-mk3
        std::filesystem::path binDir = std::filesystem::canonical("/proc/self/exe").parent_path();
        std::filesystem::path callTool = binDir / "call-tool";
        if (std::filesystem::exists(callTool)) return callTool.string();
        // Fallback: look in current directory
        if (std::filesystem::exists("./call-tool")) return "./call-tool";
        return "call-tool"; // hope it's in PATH
    }
};

// ── Built-in feed functions ──

/// Feed: system_clock
inline FeedResult pollSystemClock() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()) % 1000000;

    std::tm tm;
    localtime_r(&t, &tm);

    char iso[32], human[64], date[16], tim[16];
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &tm);
    strftime(human, sizeof(human), "%A, %B %d %Y %H:%M:%S", &tm);
    strftime(date, sizeof(date), "%Y-%m-%d", &tm);
    strftime(tim, sizeof(tim), "%H:%M:%S", &tm);

    std::ostringstream ss;
    ss << "Current time: " << human << " (ISO: " << iso << "."
       << std::setfill('0') << std::setw(6) << us.count() << ")\n"
       << "Unix: " << t << " | Date: " << date << " | Time: " << tim;

    FeedResult r;
    r.name = "system_clock";
    r.summary = ss.str();
    return r;
}

/// Feed: system_stats
inline FeedResult pollSystemStats() {
    FeedResult r;
    r.name = "system_stats";

    struct utsname uts;
    if (uname(&uts) != 0) { r.ok = false; return r; }

    long cpuCount = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpuCount < 1) cpuCount = 1;

    // Memory from /proc/meminfo
    long memTotalKb = 0, memAvailKb = 0;
    std::ifstream mem("/proc/meminfo");
    if (mem) {
        std::string line;
        while (std::getline(mem, line)) {
            if (line.rfind("MemTotal:", 0) == 0) {
                sscanf(line.c_str(), "MemTotal: %ld kB", &memTotalKb);
            } else if (line.rfind("MemAvailable:", 0) == 0) {
                sscanf(line.c_str(), "MemAvailable: %ld kB", &memAvailKb);
            }
        }
    }

    char hostname[256];
    gethostname(hostname, sizeof(hostname));

    std::ostringstream ss;
    ss << "Host: " << hostname << "\n"
       << "Platform: " << uts.sysname << " " << uts.release
       << " | Arch: " << uts.machine << "\n"
       << "Kernel: " << uts.version << "\n"
       << "CPU cores: " << cpuCount << "\n"
       << "Memory: " << (memTotalKb / 1024) << " MB total, "
       << (memAvailKb / 1024) << " MB available\n"
       << "PID: " << getpid();

    r.summary = ss.str();
    return r;
}

/// Feed: working_directory
inline FeedResult pollWorkingDirectory() {
    FeedResult r;
    r.name = "working_directory";

    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) { r.ok = false; return r; }

    std::ostringstream ss;
    ss << "CWD: " << cwd;

    // Git detection
    std::string gitCmd = "git -C " + std::string(cwd) +
        " rev-parse --show-toplevel 2>/dev/null";
    FILE* gp = popen(gitCmd.c_str(), "r");
    if (gp) {
        char gitRoot[4096] = {};
        if (fgets(gitRoot, sizeof(gitRoot), gp)) {
            std::string root(gitRoot);
            if (!root.empty() && root.back() == '\n') root.pop_back();
            ss << "\nGit repo: " << root;

            // Branch
            std::string brCmd = "git -C " + root +
                " rev-parse --abbrev-ref HEAD 2>/dev/null";
            FILE* bp = popen(brCmd.c_str(), "r");
            if (bp) {
                char branch[256] = {};
                if (fgets(branch, sizeof(branch), bp)) {
                    std::string br(branch);
                    if (!br.empty() && br.back() == '\n') br.pop_back();
                    ss << " | Branch: " << br;
                }
                pclose(bp);
            }

            // Dirty
            std::string dirtyCmd = "git -C " + root +
                " diff --quiet 2>/dev/null; echo $?";
            FILE* dp = popen(dirtyCmd.c_str(), "r");
            if (dp) {
                char d[4] = {};
                if (fgets(d, sizeof(d), dp)) {
                    ss << " | Dirty: " << (d[0] == '0' ? "no" : "yes");
                }
                pclose(dp);
            }

            // Commit
            std::string hashCmd = "git -C " + root +
                " rev-parse --short HEAD 2>/dev/null";
            FILE* hp = popen(hashCmd.c_str(), "r");
            if (hp) {
                char hash[41] = {};
                if (fgets(hash, sizeof(hash), hp)) {
                    std::string h(hash);
                    if (!h.empty() && h.back() == '\n') h.pop_back();
                    ss << " | Commit: " << h;
                }
                pclose(hp);
            }
        }
        pclose(gp);
    }

    r.summary = ss.str();
    return r;
}

// ── Register all built-in feeds as sovereign Feed objects ──
inline void registerFeeds() {
    auto& engine = FeedEngine::instance();
    engine.registerFeed("system_clock", pollSystemClock);
    engine.registerFeed("system_stats", pollSystemStats);
    engine.registerFeed("working_directory", pollWorkingDirectory);
}

} // namespace cortex::mk3::feeds
