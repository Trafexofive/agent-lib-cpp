// src/tools/builtins/list.cpp — list native builtin
#include "builtins.hpp"
#include "common.hpp"

#include <fnmatch.h>

#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace cortex::mk3::tools::builtins {

static bool typeAllowed(const fs::directory_entry& e, const std::string& type) {
    if (type == "file")
        return e.is_regular_file();
    if (type == "dir")
        return e.is_directory();
    return true;
}

static bool nameAllowed(const fs::path& path, const std::string& pattern) {
    if (pattern.empty())
        return true;
    return fnmatch(pattern.c_str(), path.filename().string().c_str(), 0) == 0;
}

static void appendEntry(std::ostringstream& out, const fs::directory_entry& e) {
    std::error_code ec;
    if (e.is_directory(ec)) {
        out << "dir  " << e.path().string() << "/\n";
        return;
    }
    if (e.is_regular_file(ec)) {
        auto size = e.file_size(ec);
        out << "file " << e.path().string();
        if (!ec)
            out << "  " << size << "B";
        out << "\n";
        return;
    }
    out << "other " << e.path().string() << "\n";
}

std::string list(const Json::Value& p) {
    std::string path = p.get("path", ".").asString();
    std::string pattern = p.get("pattern", "").asString();
    bool recursive = p.get("recursive", false).asBool();
    std::string type = p.get("type", "all").asString();

    std::error_code ec;
    fs::path root(path);
    if (!fs::exists(root, ec))
        return jsonErr("path not found: " + path);

    std::ostringstream out;
    int count = 0;
    constexpr int kLimit = 200;
    auto maybeAppend = [&](const fs::directory_entry& e) {
        if (count >= kLimit)
            return;
        if (!typeAllowed(e, type) || !nameAllowed(e.path(), pattern))
            return;
        appendEntry(out, e);
        ++count;
    };

    if (fs::is_regular_file(root, ec)) {
        maybeAppend(fs::directory_entry(root));
    } else if (recursive) {
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end && count < kLimit; it.increment(ec)) {
            maybeAppend(*it);
        }
    } else {
        for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end && count < kLimit; it.increment(ec)) {
            maybeAppend(*it);
        }
    }

    Json::Value r;
    r["success"] = true;
    r["count"] = count;
    r["truncated"] = count >= kLimit;
    r["results"] = out.str();
    return jsonStr(r);
}

}  // namespace cortex::mk3::tools::builtins
