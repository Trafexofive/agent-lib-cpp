// src/core/agent_tool_dispatch.cpp — tool dispatch, ask_tool, script execution
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "../feeds/feed_engine.hpp"
#include "../tools/dispatch.hpp"
#include "../utils/ansi.hpp"
#include "../utils/process.hpp"
#include "agent.hpp"
#include "dispatch.hpp"
#include "manifest_loader.hpp"

namespace cortex::mk3 {

Json::Value Agent::dispatchAskTool(const Json::Value& params) {
    if (askToolHandler_) {
        Json::Value result = askToolHandler_(params);
        if (!result.isObject()) {
            Json::Value wrapped;
            wrapped["success"] = true;
            wrapped["results"] = result;
            return wrapped;
        }
        if (!result.isMember("success"))
            result["success"] = true;
        return result;
    }

    auto fn = tools::ToolRegistry::instance().get("ask_tool");
    if (fn) {
        std::string raw = fn(params);
        Json::Value parsed;
        Json::CharReaderBuilder reader;
        std::string errs;
        std::istringstream ss(raw);
        if (Json::parseFromStream(reader, ss, &parsed, &errs))
            return parsed;
    }

    Json::Value out;
    out["success"] = false;
    out["error"] = "ask_tool requires an interactive ask handler";
    return out;
}

static bool parsedActionIsAskTool(const protocol::ParsedAction& action) {
    return action.type == protocol::ActionType::TOOL && action.name == "ask_tool";
}

static bool isConfigStagingDir(const std::string& dir) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(dir, ec);
    if (ec)
        p = fs::path(dir);
    return p.filename() == "staging" && !p.parent_path().empty() &&
           p.parent_path().filename() == "config";
}

static std::string shellEscapeArg(const std::string& input) {
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

static std::vector<std::string> runtimeArgv(const std::string& runtime, const std::string& entrypoint,
                                            const std::string& inputFile) {
    std::string rt = runtime.empty() ? "python3" : runtime;
    if (rt == "process" || rt == "binary" || rt == "exec" || rt == "direct")
        return {entrypoint, inputFile};
    if (rt == "python")
        rt = "python3";
    return {rt, entrypoint, inputFile};
}

static Json::Value ensureToolBuilt(const tools::Tool& tool) {
    Json::Value ok;
    ok["success"] = true;
    if (tool.buildCommand().empty())
        return ok;
    if (!tool.buildOutput().empty() && fs::exists(tool.buildOutput()))
        return ok;

    std::string cmd = tool.buildCommand();
    if (!tool.buildCwd().empty())
        cmd = "cd " + shellEscapeArg(tool.buildCwd()) + " && " + cmd;
    auto start = std::chrono::steady_clock::now();
    FILE* p = popen((cmd + " 2>&1").c_str(), "r");
    std::string output;
    if (p) {
        char buf[4096];
        while (fgets(buf, sizeof(buf), p))
            output += buf;
    }
    int rc = p ? pclose(p) : -1;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    if (rc == 0 && (tool.buildOutput().empty() || fs::exists(tool.buildOutput())))
        return ok;

    Json::Value err;
    err["success"] = false;
    err["error"] = "build failed for tool: " + tool.name();
    err["exit_code"] = rc;
    err["ms"] = (Json::Int64)elapsed;
    err["output"] = output;
    return err;
}

Json::Value Agent::dispatchTool(const protocol::ParsedAction& action) {
    protocol::ParsedAction normalized = action;
    auto toolIt = tools_.find(action.name);
    if (action.type == protocol::ActionType::TOOL && toolIt != tools_.end() &&
        !action.content.empty()) {
        if (!normalized.params.isObject())
            normalized.params = Json::Value(Json::objectValue);
        std::string textParam = toolIt->second.textParam();
        if (textParam.empty() && toolIt->second.inputType() == "text")
            textParam = "input";
        if (!textParam.empty() && !normalized.params.isMember(textParam)) {
            normalized.params[textParam] = action.content;
        }
    }

    // ── Ask tool: interactive dialog bridge. This must run before sandbox/tool
    //    availability checks so the TUI can own terminal input instead of the
    //    agent thread blocking on stdin.
    if (parsedActionIsAskTool(normalized)) {
        return dispatchAskTool(normalized.params);
    }

    // ── Sandbox validation (BT04, SB07) — runs FIRST so meta-tools and
    //    context_pin/peek/unpin can't bypass the policy.
    if (sandboxPolicy_.enabled) {
        Json::StreamWriterBuilder w;
        w["indentation"] = "";
        std::string paramsStr = Json::writeString(w, normalized.params);
        std::string blockReason = sandboxPolicy_.validate(normalized.name, paramsStr);
        if (!blockReason.empty()) {
            Json::Value err;
            err["success"] = false;
            err["error"] = blockReason;
            return err;
        }
    }

    // Manifest opt-in gate: tool implementations may exist in the backend
    // registry, but the active agent can only call tools present in tools_.
    if (normalized.type == protocol::ActionType::TOOL && !tools_.count(normalized.name)) {
        Json::Value err;
        err["success"] = false;
        err["error"] =
            "tool not available: " + normalized.name + " (not imported by active manifest)";
        return err;
    }

    // ── Meta-tools: reload manifests, toggle builtins ──
    if (normalized.name == "disable_builtin" || normalized.name == "enable_builtin") {
        return toggleBuiltin(normalized.params, normalized.name == "enable_builtin");
    }
    if (normalized.name == "reload_manifests") {
        Json::Value r;
        bool backup = normalized.params.get("backup", false).asBool();
        r["loaded"] = reloadManifests(backup);
        r["success"] = true;
        return r;
    }

    // ── Meta-tools: context management (need Agent state, can't be in registry) ──
    if (normalized.name == "context_pin") {
        return contextPin(normalized.params.get("path", "").asString(),
                          normalized.params.get("force", false).asBool());
    }
    if (normalized.name == "context_peek") {
        return contextPeek(normalized.params.get("path", "").asString(),
                           normalized.params.get("cycles", 1).asInt(),
                           normalized.params.get("force", false).asBool());
    }
    if (normalized.name == "context_unpin") {
        return contextUnpin(normalized.params.get("path", "").asString());
    }

    // ── Relic dispatch ──
    if (normalized.type == protocol::ActionType::RELIC) {
        return dispatch::dispatchRelic(normalized);
    }

    // Script tools (path-imported, not native)
    auto it = tools_.find(normalized.name);
    if (it != tools_.end() && it->second.isScript() && !it->second.scriptPath().empty()) {
        return executeScriptTool(it->second, normalized.params);
    }

    // Native tools: prefer the agent-local Tool if it is executable (has a
    // callback). Manifest import sometimes grants a schema-only ToolDef
    // (isNative=true, no cb) when registry lookup failed at load time — in
    // that case fall through to tools::dispatch which hits ToolRegistry
    // (registerDefaults). tools_ still gates permission above.
    auto parseToolJson = [](const std::string& raw) -> Json::Value {
        Json::Value parsed;
        Json::CharReaderBuilder reader;
        std::string errs;
        std::istringstream ss(raw);
        if (Json::parseFromStream(reader, ss, &parsed, &errs))
            return parsed;
        Json::Value fallback;
        fallback["success"] = true;
        fallback["output"] = raw;
        return fallback;
    };

    if (it != tools_.end()) {
        // For natives without a local callback, do NOT call execute() — it
        // returns "No callback registered". Route through ToolRegistry.
        const tools::Tool& local = it->second;
        if (local.isNative() && !local.isValid()) {
            std::string raw = tools::dispatch(normalized.name, normalized.params);
            return parseToolJson(raw);
        }
        std::string raw = local.execute(normalized.params);
        // Recover if local execute still reports missing callback.
        if (raw.find("No callback registered for native tool") != std::string::npos) {
            raw = tools::dispatch(normalized.name, normalized.params);
        }
        return parseToolJson(raw);
    }

    // Not in tools_ map (should have been gated earlier) — last-chance registry.
    if (tools::ToolRegistry::instance().has(normalized.name)) {
        return parseToolJson(tools::dispatch(normalized.name, normalized.params));
    }

    Json::Value err;
    err["success"] = false;
    err["error"] = "tool not available: " + normalized.name;
    return err;
}

Json::Value Agent::executeScriptTool(const tools::Tool& tool, const Json::Value& params) {
    // Synchronous execution — blocks but returns actual output.
    // Contract for manifest script tools (tool.yml runtime + entrypoint):
    //   - params JSON on stdin (primary)
    //   - same JSON also as argv[1] file path (compat)
    //   - stdout should be a JSON object when possible
    std::string toolName = tool.name();
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    std::string paramsJson = Json::writeString(wb, params);
    std::string blockReason = sandboxPolicy_.validate(toolName, paramsJson);
    if (!blockReason.empty()) {
        Json::Value err;
        err["success"] = false;
        err["error"] = blockReason;
        return err;
    }

    Json::Value build = ensureToolBuilt(tool);
    if (!build.get("success", false).asBool())
        return build;

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path tmpFile = fs::temp_directory_path() /
                       ("cortex-tool-" + toolName + "-" + std::to_string(now) + ".json");
    {
        std::ofstream tf(tmpFile);
        tf << paramsJson;
    }

    bool allowAnsi = true;
    auto ansiIt = env_.find("__TOOL_ANSI__");
    if (ansiIt != env_.end() && (ansiIt->second == "false" || ansiIt->second == "0" ||
                                 ansiIt->second == "no" || ansiIt->second == "never"))
        allowAnsi = false;

    // Per-tool timeout from tool.yml, else agent actionTimeoutSec.
    int timeoutSec = tool.timeoutSec() > 0 ? tool.timeoutSec() : config_.actionTimeoutSec;
    int timeoutMs = std::max(1, timeoutSec) * 1000;

    process::Spec spec;
    spec.shell = false;
    // argv: runtime entrypoint [paramsFile] — scripts may ignore file and use stdin.
    spec.argv = runtimeArgv(tool.scriptRuntime(), tool.scriptPath(), tmpFile.string());
    spec.stdinText = paramsJson;  // primary contract for bash tools that `cat`
    spec.timeoutMs = timeoutMs;
    spec.maxStdout = 1024 * 1024;
    spec.maxStderr = 256 * 1024;
    spec.env = allowAnsi ? std::map<std::string, std::string>{{"FORCE_COLOR", "1"},
                                                              {"CLICOLOR_FORCE", "1"},
                                                              {"TERM", "xterm-256color"}}
                         : std::map<std::string, std::string>{{"NO_COLOR", "1"}, {"TERM", "dumb"}};

    process::Result pr = process::run(spec);
    std::error_code ignored;
    fs::remove(tmpFile, ignored);

    // Prefer structured JSON from stdout (coder tools print a single object).
    {
        std::string out = pr.stdoutText;
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
            out.pop_back();
        Json::Value parsed;
        Json::CharReaderBuilder reader;
        std::string errs;
        std::istringstream ss(out);
        if (!out.empty() && Json::parseFromStream(reader, ss, &parsed, &errs) &&
            parsed.isObject()) {
            if (!parsed.isMember("success"))
                parsed["success"] = pr.success() && (pr.exitCode == 0);
            if (pr.timedOut) {
                parsed["success"] = false;
                if (!parsed.isMember("error"))
                    parsed["error"] = "timed out";
            } else if (pr.exitCode != 0 && !parsed.isMember("error")) {
                parsed["error"] = "exit code " + std::to_string(pr.exitCode);
            }
            parsed["exit_code"] = pr.exitCode;
            parsed["ms"] = (Json::Int64)pr.elapsedMs;
            if (pr.stdoutTruncated)
                parsed["stdout_truncated"] = true;
            if (!pr.stderrText.empty() && !parsed.isMember("stderr"))
                parsed["stderr"] = pr.stderrText;
            return parsed;
        }
    }

    Json::Value r;
    r["success"] = pr.success() && pr.exitCode == 0 && !pr.timedOut;
    r["exit"] = pr.exitCode;
    r["exit_code"] = pr.exitCode;
    r["signal"] = pr.termSignal;
    r["timed_out"] = pr.timedOut;
    r["ms"] = (Json::Int64)pr.elapsedMs;
    r["stdout"] = pr.stdoutText;
    r["stderr"] = pr.stderrText;
    r["output"] = pr.stdoutText;
    if (!pr.stderrText.empty() && pr.stdoutText.empty())
        r["output"] = pr.stderrText;
    r["stdout_truncated"] = pr.stdoutTruncated;
    r["stderr_truncated"] = pr.stderrTruncated;
    r["truncated"] = pr.stdoutTruncated || pr.stderrTruncated;
    if (pr.timedOut)
        r["error"] = "timed out";
    else if (pr.exitCode != 0)
        r["error"] = "exit code " + std::to_string(pr.exitCode);
    return r;
}

}  // namespace cortex::mk3
