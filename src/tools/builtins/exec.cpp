// src/tools/builtins/exec.cpp — production-grade exec native builtin
#include "exec.hpp"
#include "common.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <vector>

#include "../../utils/process.hpp"

namespace fs = std::filesystem;

namespace cortex::mk3::tools::builtins {

static std::vector<std::string> parseArgv(const Json::Value& p) {
    std::vector<std::string> argv;
    if (!p.isMember("argv") || !p["argv"].isArray())
        return argv;
    for (const auto& v : p["argv"])
        argv.push_back(v.asString());
    return argv;
}

static std::map<std::string, std::string> parseEnv(const Json::Value& p) {
    std::map<std::string, std::string> env;
    if (!p.isMember("env") || !p["env"].isObject())
        return env;
    for (const auto& k : p["env"].getMemberNames())
        env[k] = p["env"][k].asString();
    return env;
}

static Json::Value processResultJson(const process::Result& cap) {
    Json::Value r;
    r["success"] = cap.success();
    r["exit_code"] = cap.exitCode;
    r["signal"] = cap.termSignal;
    r["timed_out"] = cap.timedOut;
    r["elapsed_ms"] = static_cast<Json::Int64>(cap.elapsedMs);
    r["stdout_truncated"] = cap.stdoutTruncated;
    r["stderr_truncated"] = cap.stderrTruncated;
    r["truncated"] = cap.stdoutTruncated || cap.stderrTruncated;
    r["stdout_bytes"] = static_cast<Json::UInt64>(cap.stdoutText.size());
    r["stderr_bytes"] = static_cast<Json::UInt64>(cap.stderrText.size());
    r["bytes"] = static_cast<Json::UInt64>(cap.stdoutText.size() + cap.stderrText.size());
    r["stdout"] = cap.stdoutText;
    r["stderr"] = cap.stderrText;
    r["output"] = cap.stdoutText + cap.stderrText;
    if (cap.timedOut)
        r["error"] = "timed out";
    else if (cap.exitCode != 0)
        r["error"] = "exit code " + std::to_string(cap.exitCode);
    return r;
}

std::string exec(const Json::Value& p) {
    return execStreaming(p, {});
}

std::string execStreaming(const Json::Value& p,
                          const std::function<void(const std::string&, bool)>& stream) {
    process::Spec spec;
    spec.argv = parseArgv(p);
    bool hasArgv = !spec.argv.empty();
    spec.command = p.get("command", p.get("cmd", p.get("input", "").asString()).asString()).asString();
    spec.shell = p.get("shell", !hasArgv).asBool();
    if (spec.shell && spec.command.empty())
        return jsonErr("command is required when shell=true");
    if (!spec.shell && spec.argv.empty())
        return jsonErr("argv is required when shell=false");

    spec.cwd = p.get("cwd", "").asString();
    if (!spec.cwd.empty()) {
        std::error_code ec;
        if (!fs::is_directory(spec.cwd, ec))
            return jsonErr("cwd is not a directory: " + spec.cwd);
    }
    spec.stdinText = p.get("stdin", "").asString();
    spec.env = parseEnv(p);
    spec.clearEnv = p.get("clear_env", false).asBool();
    int timeoutSec = p.get("timeout", 30).asInt();
    spec.timeoutMs = p.isMember("timeout_ms") ? p["timeout_ms"].asInt() : timeoutSec * 1000;
    spec.timeoutMs = std::clamp(spec.timeoutMs, 1, 600000);
    int maxBytes = std::clamp(p.get("max_bytes", 512 * 1024).asInt(), 1, 10 * 1024 * 1024);
    spec.maxStdout = static_cast<size_t>(
        std::clamp(p.get("max_stdout", maxBytes).asInt(), 1, 10 * 1024 * 1024));
    spec.maxStderr = static_cast<size_t>(
        std::clamp(p.get("max_stderr", maxBytes).asInt(), 1, 10 * 1024 * 1024));
    if (stream) {
        spec.onOutput = [&](const char* data, size_t size, bool stderrStream) {
            if (size > 0)
                stream(std::string(data, size), stderrStream);
        };
    }

    return jsonStr(processResultJson(process::run(spec)));
}

}  // namespace cortex::mk3::tools::builtins
