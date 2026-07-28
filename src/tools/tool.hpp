#pragma once
// =============================================================================
// agent-lib-MK3 — Tool Sovereign Class
// Single-responsibility: a Tool owns its definition, execution, validation,
// schema generation, and XML serialization. No more scattered ToolDef structs
// with logic in faraway dispatch code.
// =============================================================================

#include <json/json.h>

#include <chrono>
#include <cstdio>
#include "../utils/process.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#include "../core/types.hpp"

namespace cortex::mk3::tools {

// ── Execution callback ──
using ToolStreamCallback = std::function<void(const std::string& chunk, bool stderrStream)>;
using ToolCallback = std::function<std::string(const Json::Value&)>;
using StreamingToolCallback = std::function<std::string(const Json::Value&, ToolStreamCallback)>;

// ═══════════════════════════════════════════════════════════════════════════
// Tool — sovereign class for one tool definition and execution
// ═══════════════════════════════════════════════════════════════════════════
class Tool {
   public:
    // ── Constructors ──

    /// Default constructor — creates an invalid tool
    Tool() = default;

    /// Construct from ToolDef + native C++ callback
    Tool(const ToolDef& def, ToolCallback cb) : def_(def), cb_(std::move(cb)) {
    }

    /// Construct from ToolDef + stream-aware native C++ callback
    Tool(const ToolDef& def, StreamingToolCallback cb) : def_(def), streamingCb_(std::move(cb)) {
    }

    /// Construct from ToolDef + script execution
    Tool(const ToolDef& def, const std::string& scriptPath, const std::string& runtime)
        : def_(def), scriptPath_(scriptPath), scriptRuntime_(runtime) {
        def_.isNative = false;
        def_.scriptPath = scriptPath;
        def_.scriptRuntime = runtime;
    }

    /// Construct from ToolDef only (for script tools defined elsewhere)
    explicit Tool(const ToolDef& def) : def_(def) {
    }

    // ── Accessors ──

    const std::string& name() const noexcept {
        return def_.name;
    }
    const std::string& description() const noexcept {
        return def_.description;
    }
    const std::vector<ToolParam>& params() const noexcept {
        return def_.params;
    }
    bool isNative() const noexcept {
        return def_.isNative;
    }
    bool isScript() const noexcept {
        return !def_.isNative;
    }
    const std::string& scriptPath() const noexcept {
        return def_.scriptPath;
    }
    const std::string& scriptRuntime() const noexcept {
        return def_.scriptRuntime;
    }
    const std::string& buildCommand() const noexcept {
        return def_.buildCommand;
    }
    const std::string& buildCwd() const noexcept {
        return def_.buildCwd;
    }
    const std::string& buildOutput() const noexcept {
        return def_.buildOutput;
    }
    const std::string& inputType() const noexcept {
        return def_.inputType;
    }
    const std::string& textParam() const noexcept {
        return def_.textParam;
    }

    /// Full definition (for introspection / serialization)
    const ToolDef& definition() const noexcept {
        return def_;
    }

    /// Whether the tool has been fully initialized
    bool isValid() const noexcept {
        return !def_.name.empty() && (cb_ != nullptr || !def_.scriptPath.empty());
    }

    /// Whether this tool has a registered callback (ready to execute)
    bool isExecutable() const noexcept {
        return isValid();
    }

    // ── Execution ──

    /// Execute the tool with given arguments. Returns JSON string for protocol compat.
    std::string execute(const Json::Value& args, ToolStreamCallback stream = {}) const {
        if (def_.isNative) {
            return executeNative(args, std::move(stream));
        }
        return executeScript(args, std::move(stream));
    }

    /// Execute and return a structured result
    ToolResult executeResult(const Json::Value& args) const {
        std::string out = execute(args);
        // Try to parse as JSON result
        Json::Value parsed;
        Json::CharReaderBuilder r;
        std::string errs;
        std::istringstream ss(out);
        if (Json::parseFromStream(r, ss, &parsed, &errs)) {
            ToolResult res;
            res.success = parsed.get("success", false).asBool();
            res.output = parsed.get("output", "").asString();
            res.error = parsed.get("error", "").asString();
            if (parsed.isMember("result"))
                res.data = parsed["result"];
            if (parsed.isMember("data"))
                res.data = parsed["data"];
            return res;
        }
        // Non-JSON output — treat as success with output text
        return ToolResult::ok(out);
    }

    // ── Schema generation ──

    /// OpenAI function-calling schema
    Json::Value toOpenAISchema() const {
        return def_.toOpenAISchema();
    }

    /// XML protocol description
    std::string toXml() const {
        return def_.toXml();
    }

    // ── Validation ──

    /// Validate that required params are present
    std::string validateParams(const Json::Value& args) const {
        for (const auto& param : def_.params) {
            if (!param.required)
                continue;
            if (!args.isMember(param.name) || args[param.name].isNull()) {
                return "Missing required parameter: " + param.name;
            }
        }
        return {};  // empty = valid
    }

    /// Tool metadata as JSON
    Json::Value toJson() const {
        Json::Value j;
        j["name"] = def_.name;
        j["description"] = def_.description;
        j["is_native"] = def_.isNative;
        j["input_type"] = def_.inputType;
        if (!def_.scriptRuntime.empty())
            j["runtime"] = def_.scriptRuntime;
        if (!def_.buildCommand.empty()) {
            j["build_command"] = def_.buildCommand;
            j["build_cwd"] = def_.buildCwd;
            j["build_output"] = def_.buildOutput;
        }
        Json::Value paramArray(Json::arrayValue);
        for (const auto& p : def_.params) {
            Json::Value pj;
            pj["name"] = p.name;
            pj["type"] = p.type;
            pj["description"] = p.description;
            pj["required"] = p.required;
            paramArray.append(pj);
        }
        j["params"] = paramArray;
        return j;
    }

   private:
    ToolDef def_;
    ToolCallback cb_;
    StreamingToolCallback streamingCb_;
    std::string scriptPath_;
    std::string scriptRuntime_;

    // ── Native execution ──
    std::string executeNative(const Json::Value& args, ToolStreamCallback stream) const {
        if (!cb_ && !streamingCb_) {
            return jsonError("No callback registered for native tool: " + def_.name);
        }
        try {
            if (streamingCb_)
                return streamingCb_(args, std::move(stream));
            return cb_(args);
        } catch (const std::exception& e) {
            return jsonError(std::string("Tool '") + def_.name + "' threw: " + e.what());
        } catch (...) {
            return jsonError(std::string("Tool '") + def_.name + "' threw unknown exception");
        }
    }

    // ── Script execution ──
    std::string executeScript(const Json::Value& args, ToolStreamCallback stream = {}) const {
        (void)stream;
        if (scriptPath_.empty()) {
            return jsonError("No script path for: " + def_.name);
        }

        std::string buildErr = ensureBuilt();
        if (!buildErr.empty())
            return jsonError(buildErr);

        std::string cmd = runtimeCommand(scriptRuntime_, scriptPath_);

        std::string tmpFile;
        if (def_.inputType == "text" && !def_.textParam.empty()) {
            std::string textInput = args.get(def_.textParam, "").asString();
            cmd += " " + shellEscape(textInput) + " 2>/dev/null";
        } else {
            Json::StreamWriterBuilder w;
            w["indentation"] = "";
            std::string jsonArgs = Json::writeString(w, args);
            tmpFile = tempInputPath();
            std::ofstream tf(tmpFile);
            tf << jsonArgs;
            tf.close();
            cmd += " " + shellEscape(tmpFile) + " 2>/dev/null";
        }

        std::string output = runShell(cmd);
        if (!tmpFile.empty())
            std::remove(tmpFile.c_str());
        if (output.empty()) {
            return jsonOk("{}");
        }
        return output;
    }

    // ── Helpers ──

    static std::string jsonError(const std::string& msg) {
        Json::Value r;
        r["success"] = false;
        r["error"] = msg;
        Json::StreamWriterBuilder w;
        w["indentation"] = "";
        return Json::writeString(w, r);
    }

    static std::string jsonOk(const std::string& data) {
        Json::Value r;
        r["success"] = true;
        r["result"] = data;
        Json::StreamWriterBuilder w;
        w["indentation"] = "";
        return Json::writeString(w, r);
    }

    std::string tempInputPath() const {
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        return (std::filesystem::temp_directory_path() /
                ("cortex-tool-" + def_.name + "-" + std::to_string(now) + ".json"))
            .string();
    }

    std::string ensureBuilt() const {
        if (def_.buildCommand.empty())
            return "";
        if (!def_.buildOutput.empty() && std::filesystem::exists(def_.buildOutput))
            return "";
        std::string cmd = def_.buildCommand;
        if (!def_.buildCwd.empty())
            cmd = "cd " + shellEscape(def_.buildCwd) + " && " + cmd;
        std::string out = runShell(cmd + " 2>&1");
        if (!def_.buildOutput.empty() && std::filesystem::exists(def_.buildOutput))
            return "";
        return "build failed for tool '" + def_.name + "': " + out;
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
            if (c == '\'') {
                out += "'\\";
                out += '\'';
                out += '\'';
            } else
                out += c;
        }
        out += '\'';
        return out;
    }

    // Shared bounded shell runner for tools. Routes through process::run so
    // every script execution gets per-call isolation, timeout, and output
    // caps. The legacy popen path had none of those.
    //
    // If the command contains `2>&1` we append stderr to the returned text
    // to match the old behavior (caller expected merged output). When the
    // command uses `2>/dev/null` (or no stderr redirection at all) we
    // return only stdout.
    static std::string runShell(const std::string& cmd) {
        process::Spec spec;
        spec.shell = true;
        spec.command = cmd;
        spec.timeoutMs = 30000;
        spec.maxStdout = 1024 * 1024;
        spec.maxStderr = 64 * 1024;
        process::Result pr = process::run(spec);

        std::string out = pr.stdoutText;
        if (cmd.find("2>&1") != std::string::npos)
            out += pr.stderrText;
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
            out.pop_back();
        return out;
    }
};

// ── Utility: build a ToolCallback from a function pointer ──
template <typename Fn>
inline ToolCallback makeToolCallback(Fn fn) {
    return [fn](const Json::Value& args) -> std::string { return fn(args).toJson(); };
}

}  // namespace cortex::mk3::tools
