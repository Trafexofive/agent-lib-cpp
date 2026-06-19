// src/tools/builtins/grep.cpp — grep native builtin
#include "builtins.hpp"
#include "common.hpp"

#include <fnmatch.h>

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

static void grepFile(const fs::path& file, const std::regex& re, int context, int& matches,
                     std::ostringstream& out) {
    if (matches >= 100)
        return;
    std::ifstream in(file);
    if (!in)
        return;
    std::deque<std::pair<int, std::string>> before;
    int after = 0;
    int lineNo = 0;
    std::string line;
    while (std::getline(in, line) && matches < 100) {
        ++lineNo;
        bool hit = std::regex_search(line, re);
        if (hit) {
            for (const auto& [n, ctxLine] : before)
                out << file.string() << "-" << n << "-" << ctxLine << "\n";
            before.clear();
            out << file.string() << ":" << lineNo << ":" << line << "\n";
            ++matches;
            after = context;
            continue;
        }
        if (after > 0) {
            out << file.string() << "-" << lineNo << "-" << line << "\n";
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
    std::string pattern = p.get("pattern", "").asString();
    if (pattern.empty())
        return jsonErr("pattern is required");
    std::string path = p.get("path", ".").asString();
    std::string glob = p.get("glob", "*").asString();
    int ctx = std::max(0, p.get("context", 0).asInt());

    std::regex re;
    try {
        re = std::regex(pattern);
    } catch (const std::regex_error& e) {
        return jsonErr(std::string("invalid regex: ") + e.what());
    }

    std::error_code ec;
    fs::path root(path);
    if (!fs::exists(root, ec))
        return jsonErr("path not found: " + path);

    int matches = 0;
    std::ostringstream out;
    if (fs::is_regular_file(root, ec)) {
        if (globAllowed(root, glob))
            grepFile(root, re, ctx, matches, out);
    } else {
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end && matches < 100; it.increment(ec)) {
            if (it->is_regular_file(ec) && globAllowed(it->path(), glob))
                grepFile(it->path(), re, ctx, matches, out);
        }
    }

    Json::Value r;
    r["success"] = true;
    r["matches"] = matches;
    r["truncated"] = matches >= 100;
    r["results"] = out.str();
    r["output"] = "matches=" + std::to_string(matches) + "\n" + out.str();
    return jsonStr(r);
}

}  // namespace cortex::mk3::tools::builtins
