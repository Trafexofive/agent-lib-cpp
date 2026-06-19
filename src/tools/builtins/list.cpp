// src/tools/builtins/list.cpp — list native builtin
#include "builtins.hpp"
#include "common.hpp"

#include <fnmatch.h>

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace cortex::mk3::tools::builtins {

static bool typeAllowed(const fs::directory_entry& e, const std::string& type) {
    std::error_code ec;
    if (type == "file")
        return e.is_regular_file(ec);
    if (type == "dir" || type == "directory")
        return e.is_directory(ec);
    if (type == "symlink")
        return e.is_symlink(ec);
    return type == "all" || type.empty();
}

static bool nameAllowed(const fs::path& path, const std::string& pattern) {
    if (pattern.empty() || pattern == "*")
        return true;
    return fnmatch(pattern.c_str(), path.filename().string().c_str(), 0) == 0;
}

static std::string entryType(const fs::directory_entry& e) {
    std::error_code ec;
    if (e.is_directory(ec))
        return "dir";
    if (e.is_regular_file(ec))
        return "file";
    if (e.is_symlink(ec))
        return "symlink";
    return "other";
}

static Json::Value entryJson(const fs::directory_entry& e, const fs::path& root, bool relative) {
    std::error_code ec;
    fs::path shown = relative ? fs::relative(e.path(), root, ec) : e.path();
    if (ec)
        shown = e.path();
    Json::Value item;
    item["path"] = shown.string();
    item["name"] = e.path().filename().string();
    item["type"] = entryType(e);
    if (e.is_regular_file(ec)) {
        ec.clear();
        auto size = e.file_size(ec);
        if (!ec)
            item["size"] = static_cast<Json::UInt64>(size);
    }
    return item;
}

static void appendTextEntry(std::ostringstream& out, const Json::Value& item) {
    out << item["type"].asString();
    if (item["type"].asString().size() < 4)
        out << " ";
    out << " " << item["path"].asString();
    if (item.isMember("size"))
        out << "  " << item["size"].asUInt64() << "B";
    out << "\n";
}

std::string list(const Json::Value& p) {
    if (p.isMember("path") && !p["path"].isString())
        return jsonErr("path must be a string");
    std::string path = p.get("path", ".").asString();
    std::string pattern = p.get("pattern", p.get("glob", "*").asString()).asString();
    bool recursive = p.get("recursive", false).asBool();
    bool relative = p.get("relative", true).asBool();
    std::string type = p.get("type", "all").asString();
    int maxEntries = std::clamp(p.get("max_entries", 200).asInt(), 1, 5000);

    std::error_code ec;
    fs::path root(path);
    if (!fs::exists(root, ec))
        return jsonErr("path not found: " + path);

    Json::Value entries(Json::arrayValue);
    std::ostringstream out;
    bool truncated = false;
    auto maybeAppend = [&](const fs::directory_entry& e) {
        if ((int)entries.size() >= maxEntries) {
            truncated = true;
            return;
        }
        if (!typeAllowed(e, type) || !nameAllowed(e.path(), pattern))
            return;
        Json::Value item = entryJson(e, root, relative);
        appendTextEntry(out, item);
        entries.append(item);
    };

    if (fs::is_regular_file(root, ec)) {
        maybeAppend(fs::directory_entry(root));
    } else if (fs::is_directory(root, ec)) {
        if (recursive) {
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                maybeAppend(*it);
                if (truncated)
                    break;
            }
        } else {
            for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                maybeAppend(*it);
                if (truncated)
                    break;
            }
        }
    } else {
        return jsonErr("unsupported path type: " + root.string());
    }

    Json::Value r;
    r["success"] = true;
    r["path"] = root.string();
    r["count"] = static_cast<Json::UInt64>(entries.size());
    r["truncated"] = truncated;
    r["entries"] = entries;
    r["results"] = out.str();
    r["output"] = out.str();
    return jsonStr(r);
}

}  // namespace cortex::mk3::tools::builtins
