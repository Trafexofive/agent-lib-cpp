// src/tools/builtins/fs_read.cpp — fs_read native builtin
#include "builtins.hpp"
#include "common.hpp"

#include <fstream>

namespace cortex::mk3::tools::builtins {

std::string fs_read(const Json::Value& p) {
    std::string path = p.get("path", "").asString();
    if (path.empty())
        return jsonErr("path is required");
    int offset = p.get("offset", 1).asInt();
    int limit = p.get("limit", 0).asInt();
    int startLine = offset <= 0 ? 1 : offset;
    std::ifstream f(path);
    if (!f)
        return jsonErr("file not found: " + path);
    std::string content, line;
    int lineNo = 0, total = 0, emitted = 0;
    while (std::getline(f, line)) {
        ++lineNo;
        ++total;
        if (lineNo < startLine)
            continue;
        if (limit > 0 && emitted >= limit)
            continue;
        content += line + "\n";
        ++emitted;
    }
    Json::Value r;
    r["success"] = true;
    r["content"] = content;
    r["lines"] = total;
    r["start_line"] = startLine;
    r["truncated"] = (limit > 0 && total >= startLine + limit);
    return jsonStr(r);
}

}  // namespace cortex::mk3::tools::builtins
