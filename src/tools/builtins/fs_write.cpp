// src/tools/builtins/fs_write.cpp — fs_write native builtin
#include "fs_write.hpp"
#include "common.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace cortex::mk3::tools::builtins {

static std::string uniqueTempPath(const fs::path& target) {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream name;
    name << "." << target.filename().string() << ".tmp." << now;
    fs::path dir = target.has_parent_path() ? target.parent_path() : fs::path(".");
    return (dir / name.str()).string();
}

static bool readFile(const fs::path& path, std::string& content, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "failed to read existing file: " + path.string();
        return false;
    }
    content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

static std::vector<std::string> splitLines(const std::string& content) {
    std::vector<std::string> lines;
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line))
        lines.push_back(line);
    if (!content.empty() && content.back() == '\n')
        lines.push_back("");
    return lines;
}

static std::string joinLines(const std::vector<std::string>& lines) {
    std::ostringstream out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i + 1 == lines.size() && lines[i].empty())
            break;
        out << lines[i];
        if (i + 1 < lines.size())
            out << "\n";
    }
    return out.str();
}

static std::string diffSummary(const fs::path& path, const std::string& before,
                               const std::string& after) {
    auto oldLines = splitLines(before);
    auto newLines = splitLines(after);
    size_t prefix = 0;
    while (prefix < oldLines.size() && prefix < newLines.size() && oldLines[prefix] == newLines[prefix])
        ++prefix;
    size_t oldSuffix = oldLines.size();
    size_t newSuffix = newLines.size();
    while (oldSuffix > prefix && newSuffix > prefix && oldLines[oldSuffix - 1] == newLines[newSuffix - 1]) {
        --oldSuffix;
        --newSuffix;
    }

    std::ostringstream out;
    out << "--- " << path.string() << "\n";
    out << "+++ " << path.string() << "\n";
    out << "@@ -" << (prefix + 1) << "," << (oldSuffix - prefix) << " +" << (prefix + 1)
        << "," << (newSuffix - prefix) << " @@\n";
    constexpr size_t kMaxDiffLines = 80;
    size_t emitted = 0;
    for (size_t i = prefix; i < oldSuffix && emitted < kMaxDiffLines; ++i, ++emitted)
        if (!(i + 1 == oldLines.size() && oldLines[i].empty()))
            out << "- " << (i + 1) << " | " << oldLines[i] << "\n";
    for (size_t i = prefix; i < newSuffix && emitted < kMaxDiffLines; ++i, ++emitted)
        if (!(i + 1 == newLines.size() && newLines[i].empty()))
            out << "+ " << (i + 1) << " | " << newLines[i] << "\n";
    if (emitted >= kMaxDiffLines)
        out << "... diff truncated ...\n";
    return out.str();
}

static Json::Value writeResult(const fs::path& target, const std::string& before,
                               const std::string& after, const std::string& op) {
    Json::Value r;
    r["success"] = true;
    r["path"] = target.string();
    r["op"] = op;
    r["changed"] = before != after;
    r["bytes_written"] = static_cast<Json::UInt64>(after.size());
    r["diff"] = diffSummary(target, before, after);
    r["output"] = op + " " + std::to_string(after.size()) + " bytes to " + target.string();
    return r;
}

static std::string replaceFileAtomically(const fs::path& target, const std::string& before,
                                         const std::string& after, const std::string& op,
                                         const std::function<void(const std::string&, bool)>& stream) {
    if (before == after) {
        Json::Value unchanged = writeResult(target, before, after, op);
        if (stream)
            stream(unchanged["output"].asString() + " (unchanged)\n", false);
        return jsonStr(unchanged);
    }
    std::string tmp = uniqueTempPath(target);
    {
        std::ofstream out(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out)
            return jsonErr("failed to open temp file for write: " + tmp);
        out.write(after.data(), static_cast<std::streamsize>(after.size()));
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
    if (stream)
        stream(op + " " + std::to_string(after.size()) + " bytes to " + target.string() + "\n", false);
    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return jsonErr("failed to atomically replace " + target.string() + " — " + ec.message());
    }
    Json::Value wr = writeResult(target, before, after, op);
    if (stream)
        stream(wr["diff"].asString(), false);
    return jsonStr(wr);
}

static std::string contentParam(const Json::Value& p) {
    if (p.isMember("content"))
        return p["content"].asString();
    if (p.isMember("new_text"))
        return p["new_text"].asString();
    return "";
}

static bool ensureLineRange(int start, int end, size_t lineCount, std::string& err) {
    if (start < 1 || end < start) {
        err = "invalid line range";
        return false;
    }
    if ((size_t)end > lineCount) {
        err = "line range exceeds file length";
        return false;
    }
    return true;
}

std::string fs_write(const Json::Value& p) {
    return fsWriteStreaming(p, {});
}

std::string fsWriteStreaming(const Json::Value& p,
                             const std::function<void(const std::string&, bool)>& stream) {
    if (!p.isMember("path") || !p["path"].isString())
        return jsonErr("path is required");
    if (p.isMember("content") && !p["content"].isString())
        return jsonErr("content must be a string");
    if (p.isMember("new_text") && !p["new_text"].isString())
        return jsonErr("new_text must be a string");
    if (p.isMember("old_text") && !p["old_text"].isString())
        return jsonErr("old_text must be a string");

    fs::path target(p["path"].asString());
    if (target.empty())
        return jsonErr("path is required");
    std::string op = p.get("op", "").asString();
    if (op.empty())
        op = p.get("append", false).asBool() ? "append" : "write";

    std::error_code ec;
    fs::path parent = target.has_parent_path() ? target.parent_path() : fs::path(".");
    fs::create_directories(parent, ec);
    if (ec)
        return jsonErr("failed to create parent directory: " + parent.string() + " — " + ec.message());
    if (!fs::is_directory(parent, ec))
        return jsonErr("parent is not a directory: " + parent.string());

    bool exists = fs::exists(target, ec);
    bool create = p.get("create", op == "write" || op == "append" || op == "prepend").asBool();
    if (!exists && !create)
        return jsonErr("file does not exist: " + target.string());
    if (exists && fs::is_directory(target, ec))
        return jsonErr("target is a directory: " + target.string());

    std::string before;
    if (exists) {
        std::string err;
        if (!readFile(target, before, err))
            return jsonErr(err);
    }

    std::string after = before;
    std::string content = contentParam(p);

    if (op == "write") {
        after = content;
    } else if (op == "append") {
        after += content;
    } else if (op == "prepend") {
        after = content + after;
    } else if (op == "replace_text") {
        std::string oldText = p.get("old_text", "").asString();
        std::string newText = content;
        if (oldText.empty())
            return jsonErr("old_text is required for replace_text");
        bool all = p.get("all", false).asBool();
        size_t first = before.find(oldText);
        if (first == std::string::npos)
            return jsonErr("old_text not found");
        if (!all && before.find(oldText, first + oldText.size()) != std::string::npos)
            return jsonErr("old_text is not unique (set all:true or use replace_range)");
        after.clear();
        size_t pos = 0;
        int replacements = 0;
        while (true) {
            size_t hit = before.find(oldText, pos);
            if (hit == std::string::npos) {
                after += before.substr(pos);
                break;
            }
            after += before.substr(pos, hit - pos);
            after += newText;
            pos = hit + oldText.size();
            ++replacements;
            if (!all) {
                after += before.substr(pos);
                break;
            }
        }
        (void)replacements;
    } else if (op == "replace_range" || op == "delete_range" || op == "insert_before" ||
               op == "insert_after") {
        auto lines = splitLines(before);
        if (!lines.empty() && lines.back().empty())
            lines.pop_back();
        int start = p.get("start_line", p.get("line", 0).asInt()).asInt();
        int end = p.get("end_line", start).asInt();
        std::string err;
        if (op == "insert_before" || op == "insert_after") {
            if (start < 1 || (size_t)start > lines.size() + 1)
                return jsonErr("line is outside insert bounds");
            auto insertLines = splitLines(content);
            if (!insertLines.empty() && insertLines.back().empty())
                insertLines.pop_back();
            size_t pos = op == "insert_before" ? (size_t)start - 1 : (size_t)start;
            lines.insert(lines.begin() + std::min(pos, lines.size()), insertLines.begin(), insertLines.end());
        } else {
            if (!ensureLineRange(start, end, lines.size(), err))
                return jsonErr(err);
            auto b = lines.begin() + (start - 1);
            auto e = lines.begin() + end;
            if (op == "delete_range") {
                lines.erase(b, e);
            } else {
                auto newLines = splitLines(content);
                if (!newLines.empty() && newLines.back().empty())
                    newLines.pop_back();
                lines.erase(b, e);
                lines.insert(lines.begin() + (start - 1), newLines.begin(), newLines.end());
            }
        }
        after = joinLines(lines);
        if (!after.empty())
            after += "\n";
    } else {
        return jsonErr("unknown op: " + op + " (write|append|prepend|replace_text|replace_range|delete_range|insert_before|insert_after)");
    }

    return replaceFileAtomically(target, before, after, op, stream);
}

}  // namespace cortex::mk3::tools::builtins
