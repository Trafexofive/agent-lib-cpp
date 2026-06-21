// ─────────────────────────────────────────────────────────────────────────────
// Feed Engine — polls system feeds and injects context into agent prompts
// Now delegates to sovereign Feed objects (src/feeds/feed.hpp).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <json/json.h>

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../core/mini_yaml.hpp"
#include "../utils/process.hpp"
#include "feed.hpp"

namespace cortex::mk3::feeds {

// ── Parsed manifest-side feed tool descriptor ────────────────────────────────
// Stored from feed.yml's `tools:` block. Distinct from FeedToolSpec, which is
// the prompt-side descriptor for C++-registered feed tools. These two
// sources are kept separate so manifest-declared tools that haven't been
// wired to a runtime handler are not advertised to the model as callable.
struct ManifestFeedTool {
    std::string name;
    std::string description;
    std::string runtime;    // optional — defaults to feed runtime
    std::string entrypoint; // optional — required for non-builtin tools
    std::string buildCommand;
    std::string buildCwd;
    std::string buildOutput;
};

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

    /// Register a tool handler on an existing feed. Returns false if the feed
    /// is unknown. Backward-compatible — feeds without tools still poll.
    bool registerFeedTool(const std::string& feedName, const std::string& toolName,
                          FeedToolFn handler) {
        auto it = feeds_.find(feedName);
        if (it == feeds_.end())
            return false;
        it->second.registerTool(toolName, std::move(handler));
        return true;
    }

    /// Register a tool descriptor (prompt-side metadata) on an existing feed.
    bool registerFeedToolSpec(const std::string& feedName, const FeedToolSpec& spec) {
        auto it = feeds_.find(feedName);
        if (it == feeds_.end())
            return false;
        it->second.registerToolSpec(spec);
        return true;
    }

    /// Call a tool on a feed by name. Returns {success, output, error} or an
    /// error Json::Value if the feed or tool is unknown.
    Json::Value callFeedTool(const std::string& feedName, const std::string& toolName,
                             const Json::Value& params) {
        auto it = feeds_.find(feedName);
        if (it == feeds_.end()) {
            Json::Value err;
            err["success"] = false;
            err["error"] = "unknown feed: " + feedName;
            return err;
        }
        if (!it->second.hasTool(toolName)) {
            Json::Value err;
            err["success"] = false;
            err["error"] = "feed has no tool: " + toolName;
            return err;
        }
        return it->second.callTool(toolName, params);
    }

    /// Returns true if the feed has a registered tool with the given name.
    bool feedHasTool(const std::string& feedName, const std::string& toolName) const {
        auto it = feeds_.find(feedName);
        return (it != feeds_.end()) && it->second.hasTool(toolName);
    }

    /// Snapshot of all registered tool specs, grouped by feed name. Useful for
    /// prompt injection so the model knows what it can reconfigure.
    std::map<std::string, std::vector<FeedToolSpec>> feedToolSpecs() const {
        std::map<std::string, std::vector<FeedToolSpec>> out;
        for (const auto& [name, feed] : feeds_) {
            auto specs = feed.toolSpecs();
            if (!specs.empty())
                out[name] = std::move(specs);
        }
        return out;
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
        std::vector<ManifestFeedTool> tools;
    };

    ManifestResult loadFeedManifest(const std::string& path) {
        ManifestResult mr;
        std::ifstream f(path);
        if (!f) {
            mr.error = "cannot read manifest";
            return mr;
        }

        std::string yaml((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto root = ManifestYaml::parse(yaml);
        std::string name = ManifestYaml::get(root, "name");
        std::string runtime = ManifestYaml::get(root, "runtime");
        std::string entrypoint = ManifestYaml::get(root, "entrypoint");
        std::string buildCommand;
        std::string buildCwd;
        std::string buildOutput;
        auto* build = ManifestYaml::find(root, "build");
        if (build) {
            buildCommand = ManifestYaml::get(*build, "command");
            buildCwd = ManifestYaml::get(*build, "cwd");
            buildOutput = ManifestYaml::get(*build, "output");
        }

        // Default off — empty output from a feed script is a real bug
        // surface, not a valid "nothing to report" state. Authors opt in
        // with `allow_empty: true` if their feed genuinely should be
        // considered successful when the script returns nothing.
        bool allowEmpty = false;
        std::string allowEmptyRaw = ManifestYaml::get(root, "allow_empty");
        if (!allowEmptyRaw.empty()) {
            std::string lower = allowEmptyRaw;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            allowEmpty = (lower == "true" || lower == "1" || lower == "yes");
        }

        if (name.empty()) {
            mr.error = "no name in manifest";
            return mr;
        }
        if (runtime.empty())
            runtime = "builtin";

        // Parse the optional `tools:` block. Manifest-declared feed tools are
        // stored separately and are NOT advertised to the model until a runtime
        // handler exists for them. That keeps the prompt truthful and lets
        // callers distinguish "unknown feed tool" from "feed tool not yet wired".
        std::vector<ManifestFeedTool> tools;
        if (auto* toolsNode = ManifestYaml::find(root, "tools")) {
            for (const auto& entry : toolsNode->children) {
                ManifestFeedTool tool;
                // Block-style list items put the first `key: value` on the
                // item node itself (entry.key / entry.value), not as a
                // child — so `- name: foo` surfaces "foo" on entry.value.
                // Fall back to that so authors can write the conventional
                // shape without an extra child `name:` key.
                tool.name = ManifestYaml::get(entry, "name");
                if (tool.name.empty() && entry.key == "name")
                    tool.name = entry.value;
                tool.description = ManifestYaml::get(entry, "description");
                tool.runtime = ManifestYaml::get(entry, "runtime", runtime);
                tool.entrypoint = ManifestYaml::get(entry, "entrypoint");
                if (auto* buildNode = ManifestYaml::find(entry, "build")) {
                    tool.buildCommand = ManifestYaml::get(*buildNode, "command");
                    tool.buildCwd = ManifestYaml::get(*buildNode, "cwd");
                    tool.buildOutput = ManifestYaml::get(*buildNode, "output");
                }
                if (!tool.name.empty())
                    tools.push_back(std::move(tool));
            }
        }

        mr.name = name;

        // Record any manifest-declared tools on the engine so callers can
        // retrieve them later (e.g. for prompt wiring + runtime binding).
        // This is independent of feed registration success — manifest-declared
        // tools are stored as parsed spec data, not as runtime handlers.
        mr.tools = tools;
        if (!tools.empty())
            manifestFeedTools_[name] = std::move(tools);

        // Resolve entrypoint relative to manifest
        std::filesystem::path scriptPath;
        std::filesystem::path manifestDir = std::filesystem::path(path).parent_path();
        if (!entrypoint.empty())
            scriptPath = manifestDir / entrypoint;
        std::filesystem::path buildDir = buildCwd.empty() ? manifestDir : manifestDir / buildCwd;
        std::filesystem::path buildArtifact = buildOutput.empty() ? std::filesystem::path() : manifestDir / buildOutput;

        // Execute and capture output — pass CALL_TOOL env var so scripts can invoke tools.
        // runtime: process/binary/direct runs the entrypoint directly for compiled feeds.
        std::string output;
        if (runtime == "builtin") {
            // Builtin manifests still register a Feed so manifest-declared
            // tools can be wired onto it. The poll function is a no-op since
            // builtin feeds poll through their C++ registration path, not
            // through the manifest.
            registerFeed(name, [name]() -> FeedResult {
                FeedResult fr;
                fr.name = name;
                fr.ok = true;
                fr.summary = "";
                fr.json = "{}";
                return fr;
            });
            registerManifestToolHandlers(feeds_.at(name), mr.tools, manifestDir);
            mr.success = true;
            mr.summary = "builtin feed " + name;
            return mr;
        }

        if (!ensureBuilt(buildCommand, buildDir.string(), buildArtifact.string())) {
            mr.error = "build failed for feed: " + name;
            return mr;
        }

        std::string callToolPath = findCallTool();
        output = runScriptWithEnv(runtime, scriptPath.string(), callToolPath);

        if (output.empty()) {
            if (!allowEmpty) {
                mr.success = false;
                mr.error = "feed script returned empty output (set allow_empty: true to allow)";
                mr.summary = "";
                return mr;
            }
            registerFeed(name, [name]() -> FeedResult { return {name, "", "{}", true}; });
            registerManifestToolHandlers(feeds_.at(name), mr.tools, manifestDir);
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
            registerManifestToolHandlers(feeds_.at(name), mr.tools, manifestDir);
            mr.success = true;
            mr.summary = output;
            return mr;
        }

        // Register feed that re-executes on each poll (with tool-call support)
        std::string toolPath = callToolPath;
        auto pollFn = [name, runtime, scriptPath, toolPath, buildCommand, buildDir,
                       buildArtifact]() -> FeedResult {
            FeedResult fr;
            fr.name = name;
            fr.ok = true;
            if (!ensureBuilt(buildCommand, buildDir.string(), buildArtifact.string())) {
                fr.ok = false;
                fr.summary = "build failed";
                return fr;
            }
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
        registerManifestToolHandlers(feeds_.at(name), mr.tools, manifestDir);
        mr.success = true;
        mr.summary = "loaded feed with tool-call support from " + scriptPath.string();
        return mr;
    }

    /// Register a default invocation handler for each parsed manifest tool on
    /// the feed. C++-registered handlers win (they're tested and known good);
    /// manifest handlers are skipped when the name collides with a built-in.
    /// The handler invokes the tool's runtime + entrypoint, passing params as
    /// JSON via the FEED_TOOL_PARAMS env var. Stdout is parsed as JSON when
    /// possible; otherwise returned as raw `output`. Exit status drives
    /// `success` and populates `error` on non-zero.
    void registerManifestToolHandlers(Feed& feed,
                                      const std::vector<ManifestFeedTool>& tools,
                                      const std::filesystem::path& manifestDir) {
        for (const auto& tool : tools) {
            if (tool.name.empty())
                continue;
            if (feed.hasTool(tool.name))
                continue;  // C++-registered handler wins.

            feed.registerToolSpec({tool.name, tool.description});

            std::filesystem::path entrypointPath;
            if (!tool.entrypoint.empty())
                entrypointPath = manifestDir / tool.entrypoint;
            else
                entrypointPath = manifestDir / tool.name;

            std::string toolName = tool.name;
            std::string toolRuntime = tool.runtime;
            std::string toolBuildCommand = tool.buildCommand;
            std::string toolBuildCwd = tool.buildCwd;
            std::string toolBuildOutput = tool.buildOutput;

            feed.registerTool(
                tool.name,
                [toolName, toolRuntime, toolBuildCommand, toolBuildCwd, toolBuildOutput,
                 entrypointPath](const Json::Value& params) -> Json::Value {
                    if (!toolBuildCommand.empty()) {
                        std::filesystem::path bCwd = toolBuildCwd.empty()
                                                        ? entrypointPath.parent_path()
                                                        : std::filesystem::path(toolBuildCwd);
                        if (!ensureBuilt(toolBuildCommand, bCwd.string(),
                                         toolBuildOutput)) {
                            Json::Value err;
                            err["success"] = false;
                            err["error"] = "build failed for feed tool: " + toolName;
                            return err;
                        }
                    }
                    return runFeedTool(toolName, toolRuntime, entrypointPath, params);
                });
        }
    }

    /// Manifest-declared tools for a feed. Empty for feeds with no `tools:` block
    /// or for feeds loaded before this slice shipped.
    std::vector<ManifestFeedTool> feedManifestTools(const std::string& name) const {
        auto it = manifestFeedTools_.find(name);
        if (it == manifestFeedTools_.end())
            return {};
        return it->second;
    }

   private:
    std::map<std::string, Feed> feeds_;
    std::map<std::string, std::vector<ManifestFeedTool>> manifestFeedTools_;
    std::thread refreshThread_;

    // ── Script execution helpers (kept for manifest loading) ──

    // Shared substrate for running feed scripts. All feed-side process
    // invocations go through this so per-call env, timeout, output caps,
    // and exit status are consistent across polls and tool calls.
    static process::Result runFeedScript(const std::string& runtime,
                                          const std::filesystem::path& entrypoint,
                                          std::map<std::string, std::string> extraEnv = {},
                                          int timeoutMs = 30000) {
        process::Spec spec;
        spec.shell = true;
        spec.command = runtimeCommand(runtime, entrypoint.string());
        spec.env = std::move(extraEnv);
        spec.timeoutMs = timeoutMs;
        spec.maxStdout = 1024 * 1024;
        spec.maxStderr = 64 * 1024;
        return process::run(spec);
    }

    // Feed poll view: returns trimmed stdout text. Empty string on timeout
    // (caller decides whether empty means "no data" or "error"). The child
    // sees the env additions but the parent process env is not mutated.
    static std::string runFeedPoll(const std::string& runtime,
                                    const std::filesystem::path& script,
                                    const std::string& callToolPath) {
        std::map<std::string, std::string> env;
        if (!callToolPath.empty())
            env["CALL_TOOL"] = callToolPath;
        process::Result pr = runFeedScript(runtime, script, std::move(env));
        if (pr.timedOut)
            return "";
        std::string out = pr.stdoutText;
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
            out.pop_back();
        return out;
    }

    // Feed tool view: passes params as FEED_TOOL_PARAMS env, parses stdout
    // as JSON when possible, otherwise returns it as raw `output`. Exit
    // status drives `success` and `error`. Timeout becomes a clean error
    // (no global setenv; no env leak between calls).
    static Json::Value runFeedTool(const std::string& toolName,
                                    const std::string& runtime,
                                    const std::filesystem::path& entrypoint,
                                    const Json::Value& params,
                                    int timeoutMs = 30000) {
        Json::StreamWriterBuilder w;
        w["indentation"] = "";
        std::map<std::string, std::string> env;
        env["FEED_TOOL_PARAMS"] = Json::writeString(w, params);

        process::Result pr = runFeedScript(runtime, entrypoint, std::move(env), timeoutMs);

        Json::Value result;
        if (pr.timedOut) {
            result["success"] = false;
            result["error"] = "feed tool timed out: " + toolName;
            return result;
        }

        std::string out = pr.stdoutText;
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
            out.pop_back();

        Json::CharReaderBuilder reader;
        std::string errs;
        std::istringstream ss(out);
        Json::Value parsed;
        if (Json::parseFromStream(reader, ss, &parsed, &errs)) {
            if (!parsed.isMember("success"))
                parsed["success"] = (pr.exitCode == 0);
            if (pr.exitCode != 0 && !parsed.isMember("error"))
                parsed["error"] = "tool exit code " + std::to_string(pr.exitCode);
            return parsed;
        }

        result["success"] = (pr.exitCode == 0);
        result["output"] = out;
        if (pr.stdoutTruncated)
            result["stdout_truncated"] = true;
        if (pr.exitCode != 0)
            result["error"] = "tool exit code " + std::to_string(pr.exitCode);
        return result;
    }

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

    static bool ensureBuilt(const std::string& command, const std::string& cwd,
                            const std::string& output) {
        if (command.empty())
            return true;
        if (!output.empty() && std::filesystem::exists(output))
            return true;
        std::string cmd = command;
        if (!cwd.empty())
            cmd = "cd " + shellEscape(cwd) + " && " + cmd;
        FILE* p = popen((cmd + " 2>&1").c_str(), "r");
        if (!p)
            return false;
        char buf[1024];
        while (fgets(buf, sizeof(buf), p)) {
        }
        int rc = pclose(p);
        return rc == 0 && (output.empty() || std::filesystem::exists(output));
    }

    static std::string runScriptWithEnvStatic(const std::string& runtime, const std::string& script,
                                              const std::string& callToolPath) {
        return runFeedPoll(runtime, std::filesystem::path(script), callToolPath);
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
