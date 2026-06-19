// src/tools/builtins/exec.cpp — exec native builtin
#include "builtins.hpp"
#include "common.hpp"

namespace cortex::mk3::tools::builtins {

std::string exec(const Json::Value& p) {
    std::string cmd = p.get("command", p.get("cmd", p.get("input", "").asString()).asString()).asString();
    if (cmd.empty())
        return jsonErr("command is required");
    std::string cwd = p.get("cwd", "").asString();
    int timeout = p.get("timeout", 30).asInt();
    std::string fullCmd = cwd.empty() ? cmd : "cd " + shellEscape(cwd) + " && " + cmd;
    std::string out;
    int rc = runCmd(fullCmd, out, timeout);
    Json::Value r;
    r["success"] = true;
    r["exit_code"] = rc;
    r["output"] = out;
    return jsonStr(r);
}

}  // namespace cortex::mk3::tools::builtins
