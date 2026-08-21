// src/tools/builtins/grep.cpp — grep native builtin
#include "grep.hpp"
#include "common.hpp"
#include "src/core/run_control.hpp"  // g_running — cancel mid-walk

#include <fnmatch.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace cortex::mk3::tools::builtins {

static bool globAllowed(const fs::path& path, const std::string& glob) {
    if (glob.empty() || glob == "*")
        return true;
    return fnmatch(glob.c_str(), path.filename().string().c_str(), 0) == 0;
}

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

static bool lineMatches(const std::string& line, const std::string& pattern, const std::regex& re,
                        bool literal, bool ignoreCase) {
    if (!literal)
        return std::regex_search(line, re);
    if (ignoreCase)
        return lower(line).find(lower(pattern)) != std::string::npos;
    return line.find(pattern) != std::string::npos;
}

static void appendMatch(Json::Value& matchesJson, std::ostringstream& out, const fs::path& file,
                        int lineNo, const std::string& line, bool contextLine,
                        const std::function<void(const std::string&, bool)>& stream) {
    Json::Value m;
    m["path"] = file.string();
    m["line"] = lineNo;
    m["text"] = line;
    m["context"] = contextLine;
    matchesJson.append(m);
    std::ostringstream rendered;
    rendered << file.string() << (contextLine ? "-" : ":") << lineNo << (contextLine ? "-" : ":")
             << line << "\n";
    out << rendered.str();
    if (stream)
        stream(rendered.str(), false);
}

static void grepFile(const fs::path& file, const std::string& pattern, const std::regex& re,
                     bool literal, bool ignoreCase, int context, int maxMatches,
                     Json::Value& matchesJson, std::ostringstream& out,
                     const std::function<void(const std::string&, bool)>& stream) {
    if ((int)matchesJson.size() >= maxMatches)
        return;
    std::ifstream in(file);
    if (!in)
        return;
    std::deque<std::pair<int, std::string>> before;
    int after = 0;
    int lineNo = 0;
    std::string line;
    while (std::getline(in, line) && (int)matchesJson.size() < maxMatches) {
        ++lineNo;
        bool hit = lineMatches(line, pattern, re, literal, ignoreCase);
        if (hit) {
            for (const auto& [n, ctxLine] : before)
                appendMatch(matchesJson, out, file, n, ctxLine, true, stream);
            before.clear();
            appendMatch(matchesJson, out, file, lineNo, line, false, stream);
            after = context;
            continue;
        }
        if (after > 0) {
            appendMatch(matchesJson, out, file, lineNo, line, true, stream);
            --after;
            continue;
        }
        if (context > 0) {
            before.emplace_back(lineNo, line);
            while ((int)before.size() > context)
                before.pop_front();
        }
    }
}

std::string grep(const Json::Value& p) {
    return grepStreaming(p, {});
}

// Dir names we never descend into — recursive grep of a C++ monorepo otherwise
// walks build/ + .git and burns a wall minute while the footer clock looks like
// "list is slow" (list was 0ms; grep was still open).
static bool skipDirName(const std::string& name) {
    static const char* kSkip[] = {
        ".git",       "build",      "Build",     "cmake-build-debug",
        "cmake-build-release", "node_modules", ".cache", ".cortex",
        "sessions",    "state",      "__pycache__", ".tox",
        "target",      "dist",       ".venv",     "venv",
        ".mypy_cache", ".pytest_cache", nullptr};
    for (int i = 0; kSkip[i]; ++i)
        if (name == kSkip[i]) return true;
    return false;
}

static bool looksBinaryOrHuge(const fs::path& file, std::error_code& ec) {
    auto sz = fs::file_size(file, ec);
    if (ec) return true;
    // Skip multi-MB objects / archives; text tools shouldn't open them.
    if (sz > 2 * 1024 * 1024) return true;
    std::string ext = file.extension().string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* kBin[] = {
        ".o", ".a", ".so", ".dylib", ".dll", ".exe", ".bin",
        ".png", ".jpg", ".jpeg", ".gif", ".webp", ".ico",
        ".zip", ".gz", ".tgz", ".7z", ".rar", ".pdf",
        ".wasm", ".pyc", ".class", nullptr};
    for (int i = 0; kBin[i]; ++i)
        if (ext == kBin[i]) return true;
    return false;
}

std::string grepStreaming(const Json::Value& p,
                          const std::function<void(const std::string&, bool)>& stream) {
    std::string pattern = p.get("pattern", "").asString();
    if (pattern.empty())
        return jsonErr("pattern is required");
    std::string path = p.get("path", ".").asString();
    std::string glob = p.get("glob", p.get("pattern_glob", "*").asString()).asString();
    int ctx = std::clamp(p.get("context", 0).asInt(), 0, 20);
    int maxMatches = std::clamp(p.get("max_matches", 100).asInt(), 1, 5000);
    // Cap files opened — unbounded recursive walk was the 60s hang.
    int maxFiles = std::clamp(p.get("max_files", 2000).asInt(), 1, 20000);
    bool literal = p.get("literal", false).asBool();
    bool ignoreCase = p.get("ignore_case", false).asBool();

    std::regex re;
    if (!literal) {
        try {
            auto flags = std::regex_constants::ECMAScript;
            if (ignoreCase)
                flags |= std::regex_constants::icase;
            re = std::regex(pattern, flags);
        } catch (const std::regex_error& e) {
            return jsonErr(std::string("invalid regex: ") + e.what());
        }
    }

    std::error_code ec;
    fs::path root(path);
    if (!fs::exists(root, ec))
        return jsonErr("path not found: " + path);

    Json::Value matchesJson(Json::arrayValue);
    std::ostringstream out;
    int filesOpened = 0;
    int filesSkipped = 0;
    bool filesCapped = false;
    bool cancelled = false;

    auto scanFile = [&](const fs::path& f) {
        if (!g_running) {
            cancelled = true;
            return;
        }
        if ((int)matchesJson.size() >= maxMatches) return;
        if (filesOpened >= maxFiles) {
            filesCapped = true;
            return;
        }
        if (!globAllowed(f, glob)) return;
        if (looksBinaryOrHuge(f, ec)) {
            ++filesSkipped;
            return;
        }
        ++filesOpened;
        grepFile(f, pattern, re, literal, ignoreCase, ctx, maxMatches, matchesJson, out,
                 stream);
    };

    if (fs::is_regular_file(root, ec)) {
        scanFile(root);
    } else if (fs::is_directory(root, ec)) {
        for (fs::recursive_directory_iterator it(
                 root, fs::directory_options::skip_permission_denied, ec),
             end;
             !ec && it != end && (int)matchesJson.size() < maxMatches && !filesCapped &&
             !cancelled;
             it.increment(ec)) {
            if (!g_running) {
                cancelled = true;
                break;
            }
            if (it->is_directory(ec)) {
                if (skipDirName(it->path().filename().string())) {
                    it.disable_recursion_pending();
                    ++filesSkipped;
                }
                continue;
            }
            if (it->is_regular_file(ec))
                scanFile(it->path());
        }
    } else {
        return jsonErr("unsupported path type: " + root.string());
    }

    Json::Value r;
    r["success"] = !cancelled;
    if (cancelled) {
        r["error"] = "grep cancelled (operator stop)";
        r["protocol_error"] = false;
    }
    r["path"] = root.string();
    r["pattern"] = pattern;
    r["matches"] = static_cast<Json::UInt64>(matchesJson.size());
    r["files_opened"] = filesOpened;
    r["files_skipped"] = filesSkipped;
    r["truncated"] =
        (int)matchesJson.size() >= maxMatches || filesCapped || cancelled;
    if (filesCapped)
        r["files_capped"] = true;
    r["items"] = matchesJson;
    r["results"] = out.str();
    std::ostringstream head;
    head << "matches=" << matchesJson.size() << " files=" << filesOpened;
    if (filesCapped) head << " (files cap)";
    if (cancelled) head << " (cancelled)";
    head << "\n" << out.str();
    r["output"] = head.str();
    return jsonStr(r);
}

}  // namespace cortex::mk3::tools::builtins
