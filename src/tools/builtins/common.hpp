// src/tools/builtins/common.hpp — shared helpers for native builtin tools
#pragma once

#include <sys/wait.h>

#include <cstdio>
#include <sstream>
#include <string>

#include "../../core/types.hpp"

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

inline int runCmd(const std::string& cmd, std::string& output, int timeoutSec = 30) {
    std::string fullCmd = "timeout " + std::to_string(timeoutSec) + " " + cmd + " 2>&1";
    FILE* pipe = popen(fullCmd.c_str(), "r");
    if (!pipe)
        return -1;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe))
        output += buf;
    int rc = pclose(pipe);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

}  // namespace cortex::mk3::tools::builtins
