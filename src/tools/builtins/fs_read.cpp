// src/tools/builtins/fs_read.cpp — fs_read native builtin
#include "builtins.hpp"
#include "common.hpp"

#include <fnmatch.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace cortex::mk3::tools::builtins {

static bool globAllowed(const fs::path& path, const std::string& glob) {
    if (glob.empty() || glob == "*")
        return true;
    return fnmatch(glob.c_str(), path.filename().string().c_str(), 0) == 0;
}

static std::vector<std::string> readLines(const fs::path& path, std::string& err) {
    std::ifstream f(path);
    if (!f) {
        err = "failed to read file: " + path.string();
        return {};
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line))
        lines.push_back(line);
    return lines;
}

static std::string formatLineBlock(const fs::path& path, const std::vector<std::string>& lines,
                                   int startLine, int limit, bool lineNumbers, int& emitted,
                                   bool& truncated) {
    std::ostringstream out;
    int total = static_cast<int>(lines.size());
    int start = std::max(1, startLine);
    int end = limit > 0 ? std::min(total, start + limit - 1) : total;
    int width = std::max(1, static_cast<int>(std::to_string(std::max(total, end)).size()));
    for (int lineNo = start; lineNo <= end; ++lineNo) {
        if (lineNo < 1 || lineNo > total)
            continue;
        if (lineNumbers)
            out << std::setw(width) << lineNo << " | ";
        out << lines[lineNo - 1] << "\n";
        ++emitted;
    }
    truncated = limit > 0 && end < total;
    (void)path;
    return out.str();
}

static std::string readFileResult(const fs::path& path, int offset, int limit, bool lineNumbers) {
    std::string err;
    auto lines = readLines(path, err);
    if (!err.empty())
        return jsonErr(err);
    int emitted = 0;
    bool truncated = false;
    std::string content = formatLineBlock(path, lines, offset, limit, lineNumbers, emitted, truncated);
    Json::Value r;
    r["success"] = true;
    r["kind"] = "file";
    r["path"] = path.string();
    r["content"] = content;
    r["line_numbers"] = lineNumbers;
    r["lines"] = static_cast<Json::UInt64>(lines.size());
    r["start_line"] = std::max(1, offset);
    r["lines_emitted"] = emitted;
    r["truncated"] = truncated;
    return jsonStr(r);
}

static void appendTreeEntry(std::ostringstream& out, const fs::directory_entry& e, const fs::path& root) {
    std::error_code ec;
    auto rel = fs::relative(e.path(), root, ec);
    std::string shown = ec ? e.path().string() : rel.string();
    if (e.is_directory(ec)) {
        out << "dir  " << shown << "/\n";
    } else if (e.is_regular_file(ec)) {
        auto size = e.file_size(ec);
        out << "file " << shown;
        if (!ec)
            out << "  " << size << "B";
        out << "\n";
    } else {
        out << "other " << shown << "\n";
    }
}

static std::string readDirectoryResult(const fs::path& root, const Json::Value& p) {
    bool recursive = p.get("recursive", false).asBool();
    bool includeContent = p.get("include_content", false).asBool();
    bool lineNumbers = p.get("line_numbers", true).asBool();
    std::string glob = p.get("glob", p.get("pattern", "*").asString()).asString();
    int maxEntries = std::max(1, p.get("max_entries", 200).asInt());
    int maxFiles = std::max(1, p.get("max_files", 25).asInt());
    int perFileLimit = std::max(1, p.get("per_file_limit", 120).asInt());

    std::error_code ec;
    std::ostringstream tree;
    Json::Value files(Json::arrayValue);
    int entries = 0;
    int filesRead = 0;
    bool truncated = false;

    auto visit = [&](const fs::directory_entry& e) {
        if (entries >= maxEntries) {
            truncated = true;
            return;
        }
        appendTreeEntry(tree, e, root);
        ++entries;
        if (!includeContent || filesRead >= maxFiles || !e.is_regular_file(ec) ||
            !globAllowed(e.path(), glob))
            return;
        std::string err;
        auto lines = readLines(e.path(), err);
        if (!err.empty())
            return;
        int emitted = 0;
        bool fileTruncated = false;
        std::string block = formatLineBlock(e.path(), lines, 1, perFileLimit, lineNumbers, emitted,
                                            fileTruncated);
        Json::Value item;
        item["path"] = e.path().string();
        item["lines"] = static_cast<Json::UInt64>(lines.size());
        item["lines_emitted"] = emitted;
        item["truncated"] = fileTruncated;
        item["content"] = block;
        files.append(item);
        ++filesRead;
    };

    if (recursive) {
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            visit(*it);
            if (entries >= maxEntries)
                break;
        }
    } else {
        for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            visit(*it);
            if (entries >= maxEntries)
                break;
        }
    }

    std::ostringstream content;
    content << tree.str();
    if (includeContent) {
        for (const auto& file : files) {
            content << "\n--- " << file["path"].asString() << " ---\n";
            content << file["content"].asString();
        }
    }

    Json::Value r;
    r["success"] = true;
    r["kind"] = "directory";
    r["path"] = root.string();
    r["content"] = content.str();
    r["tree"] = tree.str();
    r["files"] = files;
    r["entries"] = entries;
    r["files_read"] = filesRead;
    r["truncated"] = truncated;
    r["line_numbers"] = lineNumbers;
    return jsonStr(r);
}

std::string fs_read(const Json::Value& p) {
    if (!p.isMember("path") || !p["path"].isString())
        return jsonErr("path is required");
    fs::path path(p["path"].asString());
    if (path.empty())
        return jsonErr("path is required");
    int offset = p.get("offset", p.get("start_line", 1).asInt()).asInt();
    int limit = p.get("limit", 0).asInt();
    bool lineNumbers = p.get("line_numbers", true).asBool();

    std::error_code ec;
    if (!fs::exists(path, ec))
        return jsonErr("path not found: " + path.string());
    if (fs::is_directory(path, ec))
        return readDirectoryResult(path, p);
    if (fs::is_regular_file(path, ec))
        return readFileResult(path, offset, limit, lineNumbers);
    return jsonErr("unsupported path type: " + path.string());
}

}  // namespace cortex::mk3::tools::builtins
