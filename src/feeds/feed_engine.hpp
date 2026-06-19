// ─────────────────────────────────────────────────────────────────────────────
// Feed Engine — polls system feeds and injects context into agent prompts
// Now delegates to sovereign Feed objects (src/feeds/feed.hpp).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <json/json.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "feed.hpp"

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
        if (refreshThread_.joinable())
            refreshThread_.join();
    }

    // ── Registration ──

    /// Register a Feed object (sovereign)
    bool registerFeed(Feed&& feed) {
        if (!feed.isValid())
            return false;
        if (has(feed.name()))
            return false;  // dedup
        feeds_[feed.name()] = std::move(feed);
        return true;
    }

    /// Register a feed by name + poll function (backward-compat convenience)
    void registerFeed(const std::string& name, FeedFn fn) {
        if (has(name))
            return;                            // dedup
        Feed feed(name, std::move(fn), true);  // poll immediate
        feeds_[name] = std::move(feed);

        // Async refresh (legacy behavior — keeps the refresh thread pattern)
        if (refreshThread_.joinable())
            refreshThread_.join();
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
        if (it == feeds_.end())
            return {name, "unknown feed", "{}", false};
        if (forceFresh)
            return it->second.poll();
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
        for (const auto& [name, _] : feeds_)
            names.push_back(name);
        return names;
    }

    /// Feed count
    size_t count() const {
        return feeds_.size();
    }

    // ── Prompt injection ──

    /// Format all feeds as a Markdown section for prompt injection
    std::string injectIntoPrompt() {
        if (feeds_.empty())
            return "";
        std::ostringstream ss;
        ss << "\n## System Feeds\n";
        bool wrote = false;
        for (auto& [name, feed] : feeds_) {
            FeedResult r = feed.get();
            if (!r.ok)
                continue;
            wrote = true;
            ss << "### " << name << "\n";
            if (!r.summary.empty())
                ss << r.summary << "\n";
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
        if (!f) {
            mr.error = "cannot read manifest";
            return mr;
        }

        // Parse minimal YAML (kind: Feed, name:, runtime:, entrypoint:)
        std::string line, name, runtime, entrypoint;
        while (std::getline(f, line)) {
            size_t colon = line.find(": ");
            if (colon == std::string::npos)
                continue;
            std::string key = line.substr(0, colon);
            size_t start = key.find_first_not_of(" \t");
            if (start != std::string::npos)
                key = key.substr(start);
            std::string val = line.substr(colon + 2);
            if (key == "name")
                name = val;
            else if (key == "runtime")
                runtime = val;
            else if (key == "entrypoint")
                entrypoint = val;
        }

        if (name.empty()) {
            mr.error = "no name in manifest";
            return mr;
        }
        if (runtime.empty())
            runtime = "builtin";

        mr.name = name;

        // Resolve entrypoint relative to manifest
        std::filesystem::path scriptPath;
        if (!entrypoint.empty()) {
            std::filesystem::path manifestDir = std::filesystem::path(path).parent_path();
            scriptPath = manifestDir / entrypoint;
        }

        // Execute and capture output — pass CALL_TOOL env var so scripts can invoke tools.
        // runtime: process/binary/direct runs the entrypoint directly for compiled feeds.
        std::string output;
        if (runtime == "builtin") {
            mr.success = true;
            mr.summary = "builtin feed " + name;
            return mr;
        }

        std::string callToolPath = findCallTool();
        output = runScriptWithEnv(runtime, scriptPath.string(), callToolPath);

        if (output.empty()) {
            registerFeed(name, [name]() -> FeedResult { return {name, "", "{}", true}; });
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
            registerFeed(name,
                         [name, output]() -> FeedResult { return {name, output, "{}", true}; });
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
                if (!sum.str().empty())
                    sum << "\n";
                if (p[key].isString())
                    sum << key << ": " << p[key].asString();
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
        std::string cmd = runtimeCommand(runtime, script) + " 2>/dev/null";
        std::string oldCallTool;
        const char* old = getenv("CALL_TOOL");
        if (old)
            oldCallTool = old;
        if (!callToolPath.empty())
            setenv("CALL_TOOL", callToolPath.c_str(), 1);

        FILE* p = popen(cmd.c_str(), "r");
        if (!p) {
            if (!oldCallTool.empty())
                setenv("CALL_TOOL", oldCallTool.c_str(), 1);
            else
                unsetenv("CALL_TOOL");
            return "";
        }

        std::string output;
        char buf[4096];
        while (fgets(buf, sizeof(buf), p))
            output += buf;
        pclose(p);
        if (!oldCallTool.empty())
            setenv("CALL_TOOL", oldCallTool.c_str(), 1);
        else
            unsetenv("CALL_TOOL");
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
            output.pop_back();
        return output;
    }

    static std::string runtimeCommand(const std::string& runtime, const std::string& entrypoint) {
        std::string rt = runtime.empty() ? "python3" : runtime;
        std::string ep = shellEscape(entrypoint);
        if (rt == "process" || rt == "binary" || rt == "exec" || rt == "direct")
            return ep;
        if (rt == "python")
            rt = "python3";
        return rt + " " + ep;
    }

    static std::string shellEscape(const std::string& input) {
        std::string out(1, '\'');
        for (char c : input) {
            if (c == '\'')
                out += "'\\''";
            else
                out += c;
        }
        out += '\'';
        return out;
    }

    static std::string findCallTool() {
        // Look for call-tool binary in same directory as cortex-mk3
        std::filesystem::path binDir = std::filesystem::canonical("/proc/self/exe").parent_path();
        std::filesystem::path callTool = binDir / "call-tool";
        if (std::filesystem::exists(callTool))
            return callTool.string();
        // Fallback: look in current directory
        if (std::filesystem::exists("./call-tool"))
            return "./call-tool";
        return "call-tool";  // hope it's in PATH
    }
};

// ── Built-in feed functions ──
FeedResult pollSystemClock();
FeedResult pollSystemStats();
FeedResult pollWorkingDirectory();

// ── Register all built-in feeds as sovereign Feed objects ──
void registerFeeds();

}  // namespace cortex::mk3::feeds
