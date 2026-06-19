// src/tools/builtins/list.cpp — list native builtin
#include "builtins.hpp"
#include "common.hpp"

#include <sstream>

namespace cortex::mk3::tools::builtins {

std::string list(const Json::Value& p) {
    std::string path = p.get("path", ".").asString();
    std::string pattern = p.get("pattern", "").asString();
    bool recursive = p.get("recursive", false).asBool();
    std::string type = p.get("type", "all").asString();
    std::string cmd = recursive
                          ? "find " + shellEscape(path) +
                                (type == "dir"    ? " -type d"
                                 : type == "file" ? " -type f"
                                                  : "") +
                                (pattern.empty() ? "" : " -name " + shellEscape(pattern)) +
                                " 2>/dev/null | head -200"
                          : "ls -la " + shellEscape(pattern.empty() ? path : path + "/" + pattern) +
                                " 2>/dev/null";
    std::string out;
    runCmd(cmd, out, 15);
    int count = 0;
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line))
        if (!line.empty())
            count++;
    Json::Value r;
    r["success"] = true;
    r["count"] = count;
    r["results"] = out;
    return jsonStr(r);
}

}  // namespace cortex::mk3::tools::builtins
