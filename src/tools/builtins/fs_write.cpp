// src/tools/builtins/fs_write.cpp — fs_write native builtin
#include "builtins.hpp"
#include "common.hpp"

#include <fstream>

namespace cortex::mk3::tools::builtins {

std::string fs_write(const Json::Value& p) {
    std::string path = p.get("path", "").asString();
    std::string content = p.get("content", "").asString();
    if (path.empty())
        return jsonErr("path is required");
    bool append = p.get("append", false).asBool();
    auto lastSlash = path.rfind('/');
    if (lastSlash != std::string::npos) {
        std::string unused;
        runCmd("mkdir -p " + shellEscape(path.substr(0, lastSlash)), unused, 5);
    }
    std::ofstream f(path, append ? std::ios::app : std::ios::out);
    if (!f)
        return jsonErr("failed to write: " + path);
    f << content;
    f.close();
    Json::Value r;
    r["success"] = true;
    r["path"] = path;
    r["bytes_written"] = static_cast<Json::UInt64>(content.size());
    r["output"] = std::string(append ? "appended " : "wrote ") + std::to_string(content.size()) +
                  " bytes to " + path;
    return jsonStr(r);
}

}  // namespace cortex::mk3::tools::builtins
