// src/tools/builtins/grep.cpp — grep native builtin
#include "builtins.hpp"
#include "common.hpp"

#include <sstream>

namespace cortex::mk3::tools::builtins {

std::string grep(const Json::Value& p) {
    std::string pattern = p.get("pattern", "").asString();
    if (pattern.empty())
        return jsonErr("pattern is required");
    std::string path = p.get("path", ".").asString();
    std::string glob = p.get("glob", "*").asString();
    int ctx = p.get("context", 0).asInt();
    std::string cmd = "grep -rn --include=" + shellEscape(glob) +
                      (ctx > 0 ? " -C " + std::to_string(ctx) : "") + " " + shellEscape(pattern) +
                      " " + shellEscape(path) + " 2>/dev/null | head -100";
    std::string out;
    runCmd(cmd, out, 30);
    int matches = 0;
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line))
        if (!line.empty())
            matches++;
    Json::Value r;
    r["success"] = true;
    r["matches"] = matches;
    r["results"] = out;
    r["output"] = "matches=" + std::to_string(matches) + "\n" + out;
    return jsonStr(r);
}

}  // namespace cortex::mk3::tools::builtins
