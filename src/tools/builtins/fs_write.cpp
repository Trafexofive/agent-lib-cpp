// src/tools/builtins/fs_write.cpp — fs_write native builtin
#include "builtins.hpp"
#include "common.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace cortex::mk3::tools::builtins {

static std::string uniqueTempPath(const fs::path& target) {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream name;
    name << "." << target.filename().string() << ".tmp." << now;
    fs::path dir = target.has_parent_path() ? target.parent_path() : fs::path(".");
    return (dir / name.str()).string();
}

static Json::Value writeResult(const fs::path& target, const std::string& content,
                               const std::string& mode) {
    Json::Value r;
    r["success"] = true;
    r["path"] = target.string();
    r["mode"] = mode;
    r["bytes_written"] = static_cast<Json::UInt64>(content.size());
    r["output"] = mode + " " + std::to_string(content.size()) + " bytes to " + target.string();
    return r;
}

static std::string appendFile(const fs::path& target, const std::string& content) {
    std::ofstream out(target, std::ios::app | std::ios::binary);
    if (!out)
        return jsonErr("failed to open for append: " + target.string());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    if (!out)
        return jsonErr("failed while appending: " + target.string());
    out.close();
    if (!out)
        return jsonErr("failed to close after append: " + target.string());
    return jsonStr(writeResult(target, content, "appended"));
}

static std::string replaceFileAtomically(const fs::path& target, const std::string& content) {
    std::string tmp = uniqueTempPath(target);
    {
        std::ofstream out(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out)
            return jsonErr("failed to open temp file for write: " + tmp);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) {
            std::error_code ignored;
            fs::remove(tmp, ignored);
            return jsonErr("failed while writing temp file: " + tmp);
        }
        out.close();
        if (!out) {
            std::error_code ignored;
            fs::remove(tmp, ignored);
            return jsonErr("failed to close temp file: " + tmp);
        }
    }

    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return jsonErr("failed to atomically replace " + target.string() + " — " + ec.message());
    }
    return jsonStr(writeResult(target, content, "wrote"));
}

std::string fs_write(const Json::Value& p) {
    if (!p.isMember("path") || !p["path"].isString())
        return jsonErr("path is required");
    if (p.isMember("content") && !p["content"].isString())
        return jsonErr("content must be a string");

    fs::path target(p["path"].asString());
    if (target.empty())
        return jsonErr("path is required");
    std::string content = p.get("content", "").asString();
    bool append = p.get("append", false).asBool();

    std::error_code ec;
    fs::path parent = target.has_parent_path() ? target.parent_path() : fs::path(".");
    fs::create_directories(parent, ec);
    if (ec)
        return jsonErr("failed to create parent directory: " + parent.string() + " — " +
                       ec.message());
    if (!fs::is_directory(parent, ec))
        return jsonErr("parent is not a directory: " + parent.string());

    return append ? appendFile(target, content) : replaceFileAtomically(target, content);
}

}  // namespace cortex::mk3::tools::builtins
