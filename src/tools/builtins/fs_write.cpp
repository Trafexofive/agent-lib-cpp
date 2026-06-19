// src/tools/builtins/fs_write.cpp — fs_write native builtin
#include "builtins.hpp"
#include "common.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace cortex::mk3::tools::builtins {

std::string fs_write(const Json::Value& p) {
    std::string path = p.get("path", "").asString();
    std::string content = p.get("content", "").asString();
    if (path.empty())
        return jsonErr("path is required");
    bool append = p.get("append", false).asBool();

    std::error_code ec;
    fs::path target(path);
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
        if (ec)
            return jsonErr("failed to create parent directory: " + target.parent_path().string() +
                           " — " + ec.message());
    }

    std::ofstream f(path, append ? std::ios::app : std::ios::out);
    if (!f)
        return jsonErr("failed to write: " + path);
    f << content;
    f.close();
    if (!f)
        return jsonErr("failed while writing: " + path);

    Json::Value r;
    r["success"] = true;
    r["path"] = path;
    r["bytes_written"] = static_cast<Json::UInt64>(content.size());
    r["output"] = std::string(append ? "appended " : "wrote ") + std::to_string(content.size()) +
                  " bytes to " + path;
    return jsonStr(r);
}

}  // namespace cortex::mk3::tools::builtins
