// src/tools/builtins/common.hpp — shared helpers for native builtin tools
#pragma once

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>

#include "../../core/types.hpp"
#include "../../utils/process.hpp"

namespace cortex::mk3::tools::builtins {

inline std::string jsonStr(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

inline std::string jsonErr(const std::string& msg) {
    return jsonStr(ToolResult::fail(msg).toJson());
}

inline std::string jsonOk(const Json::Value& data) {
    Json::Value r;
    r["success"] = true;
    r["result"] = data;
    return jsonStr(r);
}

inline std::string shellEscape(const std::string& input) {
    std::string out(1, '\x27');
    for (char c : input) {
        if (c == '\x27') {
            out += "'\\";
            out += '\x27';
            out += '\x27';
        } else {
            out += c;
        }
    }
    out += '\x27';
    return out;
}

// Dir names list/tree/grep/squeezer never descend into (and list never shows
// as children of a parent). Operator listing `build/` itself still works —
// skip applies to *children* named these, not the requested root.
inline bool skipDirName(const std::string& name) {
    static const char* kSkip[] = {
        ".git",       "build",      "Build",     "cmake-build-debug",
        "cmake-build-release", "node_modules", ".cache", ".cortex",
        ".artifacts", "sessions",    "state",
        "__pycache__", ".tox",      "target",      "dist",
        ".venv",      "venv",       ".mypy_cache", ".pytest_cache",
        nullptr};
    for (int i = 0; kSkip[i]; ++i)
        if (name == kSkip[i]) return true;
    return false;
}

inline bool skipBinaryExt(const std::string& extRaw) {
    std::string ext = extRaw;
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* kBin[] = {
        ".o", ".a", ".so", ".dylib", ".dll", ".exe", ".bin",
        nullptr};
    for (int i = 0; kBin[i]; ++i)
        if (ext == kBin[i]) return true;
    return false;
}

inline int runCmd(const std::string& cmd, std::string& output, int timeoutSec = 30) {
    process::Spec spec;
    spec.shell = true;
    spec.command = cmd;
    spec.timeoutMs = std::max(1, timeoutSec) * 1000;
    spec.maxStdout = 512 * 1024;
    spec.maxStderr = 256 * 1024;
    process::Result pr = process::run(spec);
    output = pr.stdoutText + pr.stderrText;
    if (pr.timedOut)
        return 124;
    return pr.exitCode;
}

}  // namespace cortex::mk3::tools::builtins
