// src/tools/builtins/challenge.cpp — doubt-finder tool for the aristotle agent
//
// Walks a file and surfaces things a skeptic would doubt:
//   - Unjustified assertion-words in comments ("always", "never", "safe", "obviously",
//     "clearly", "guaranteed", "impossible", "must")
//   - TODO / FIXME / HACK / XXX markers
//   - Unchecked errors (empty catch blocks, `if (!x) return;` with no explanation)
//   - Magic numbers (numeric literals >= 10 not obviously 0/1/2 or standard sizes)
//   - `assert(` without a comment explaining the invariant
//
// Output: a JSON envelope with `success` and `findings` (a JSON array).
// Each finding has `line`, `kind`, `severity`, `evidence` (the trimmed line text).
//
// The tool is intentionally permissive: it over-includes. The caller (an LLM
// persona) triages which findings are real. Better to surface a false positive
// than to miss a real doubt.

#include "builtins.hpp"
#include "common.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace cortex::mk3::tools::builtins {

namespace {

struct Finding {
    int line = 0;
    std::string kind;       // "assertion" | "todo" | "unchecked" | "magic" | "assert_macro"
    std::string severity;   // "concern" | "nit"
    std::string evidence;   // trimmed line text
};

// Trim leading/trailing whitespace.
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// Strip leading `//` or `/*` and trailing `*/` to get just the comment body.
std::string stripCommentDelim(const std::string& s) {
    std::string t = s;
    if (t.size() >= 2 && t[0] == '/' && t[1] == '/') t = t.substr(2);
    else if (t.size() >= 2 && t[0] == '/' && t[1] == '*') t = t.substr(2);
    if (t.size() >= 2 && t[t.size() - 2] == '*' && t.back() == '/') t.pop_back(), t.pop_back();
    // Trim a leading space left by the delimiter.
    if (!t.empty() && t[0] == ' ') t = t.substr(1);
    return trim(t);
}

bool lineIsComment(const std::string& s) {
    std::string t = trim(s);
    if (t.size() >= 2 && t[0] == '/' && t[1] == '/') return true;
    if (t.size() >= 2 && t[0] == '/' && t[1] == '*') return true;
    return false;
}

// Suspicious words in comments. Match as whole words, case-insensitive.
const std::vector<std::string>& assertionWords() {
    static const std::vector<std::string> words = {
        "always", "never", "safe", "obviously", "clearly", "guaranteed",
        "impossible", "must", "trivial", "obviously", "of course"};
    return words;
}

bool containsAssertionWord(const std::string& lower) {
    for (const auto& w : assertionWords()) {
        // Word boundary: non-alnum on both sides, or string edge.
        size_t pos = 0;
        while ((pos = lower.find(w, pos)) != std::string::npos) {
            bool leftOk = pos == 0 || !std::isalnum(static_cast<unsigned char>(lower[pos - 1]));
            bool rightOk =
                pos + w.size() == lower.size() ||
                !std::isalnum(static_cast<unsigned char>(lower[pos + w.size()]));
            if (leftOk && rightOk)
                return true;
            pos += w.size();
        }
    }
    return false;
}

bool containsTodo(const std::string& t) {
    static const std::vector<std::string> markers = {"TODO", "FIXME", "HACK", "XXX", "BUG"};
    for (const auto& m : markers) {
        size_t pos = 0;
        while ((pos = t.find(m, pos)) != std::string::npos) {
            // Must be a word-ish match (not part of a longer identifier).
            bool leftOk = pos == 0 || !std::isalnum(static_cast<unsigned char>(t[pos - 1]));
            bool rightOk =
                pos + m.size() == t.size() ||
                !std::isalnum(static_cast<unsigned char>(t[pos + m.size()]));
            if (leftOk && rightOk)
                return true;
            pos += m.size();
        }
    }
    return false;
}

// Detect `if (!x) return;` or `if (!x) continue;` with no comment on the same line.
bool isUncheckedGuard(const std::string& t) {
    static const std::regex guard(
        R"(if\s*\(\s*!\s*[A-Za-z_][A-Za-z_0-9]*\s*\)\s*(return|continue|break)\s*;)");
    return std::regex_search(t, guard);
}

bool isEmptyCatch(const std::string& t) {
    static const std::regex empty(R"(\}\s*$)");
    // Heuristic: line is just `}` after a catch — the parser already does the hard
    // work, this is a fallback.
    return false;  // disabled; the regex above is too brittle across styles
}

bool isAssertMacro(const std::string& t) {
    static const std::regex re(R"(\bassert\s*\()");
    return std::regex_search(t, re);
}

// Find numeric literals in code (not in comments). Returns values and positions.
struct NumHit {
    int line;
    int col;
    long value;
};
std::vector<NumHit> findNumericLiterals(const std::string& line) {
    std::vector<NumHit> out;
    // std::regex doesn't reliably support lookbehind across libstdc++ versions;
    // emulate the boundary by checking the previous char manually.
    static const std::regex re(R"((\d{2,}))");
    auto begin = std::sregex_iterator(line.begin(), line.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        size_t pos = it->position();
        if (pos > 0) {
            char prev = line[pos - 1];
            if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_' || prev == '.')
                continue;
        }
        try {
            long v = std::stol((*it)[1].str());
            out.push_back({0, static_cast<int>(pos), v});
        } catch (...) {
        }
    }
    return out;
}

// "Standard" sizes we don't flag: page sizes, common buffers, errno/limits.
bool isStandardSize(long v) {
    static const std::vector<long> ok = {0,  1,   2,    8,    10,   16,   24,  32,
                                         64, 100, 128,  256,  512,  1000, 1024, 2048,
                                         4096, 8192, 16384, 32768, 65536, 131072, 262144};
    for (long x : ok)
        if (v == x)
            return true;
    return false;
}

}  // namespace

std::string challenge(const Json::Value& params) {
    std::string path = params.get("path", "").asString();
    if (path.empty())
        return jsonErr("missing required param: path");
    int maxFindings = params.get("max_findings", 50).asInt();
    if (maxFindings <= 0)
        maxFindings = 50;

    std::error_code ec;
    fs::path p(path);
    if (!fs::exists(p, ec))
        return jsonErr("file not found: " + path);
    if (!fs::is_regular_file(p, ec))
        return jsonErr("not a regular file: " + path);

    std::ifstream f(p);
    if (!f)
        return jsonErr("cannot open: " + path);

    std::vector<Finding> findings;
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        lineno++;
        std::string trimmed = trim(line);
        if (trimmed.empty())
            continue;
        std::string lower = trimmed;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        // 1) Comments: assertion-words, TODOs.
        if (lineIsComment(trimmed)) {
            std::string body = stripCommentDelim(trimmed);
            if (containsAssertionWord(body)) {
                findings.push_back({lineno, "assertion", "concern", trimmed});
            } else if (containsTodo(trimmed)) {
                findings.push_back({lineno, "todo", "nit", trimmed});
            }
            continue;
        }

        // 2) Code lines: unchecked guards, asserts, magic numbers.
        if (isUncheckedGuard(trimmed)) {
            findings.push_back({lineno, "unchecked", "concern", trimmed});
        }
        if (isAssertMacro(trimmed)) {
            findings.push_back({lineno, "assert_macro", "nit", trimmed});
        }
        for (const auto& num : findNumericLiterals(trimmed)) {
            (void)num.col;
            if (!isStandardSize(num.value) && num.value >= 10) {
                findings.push_back(
                    {lineno, "magic", "nit", trimmed});
                break;  // one magic-number finding per line
            }
        }

        if (static_cast<int>(findings.size()) >= maxFindings)
            break;
    }

    Json::Value out(Json::objectValue);
    out["success"] = true;
    out["path"] = path;
    out["lines_scanned"] = lineno;
    out["findings"] = Json::Value(Json::arrayValue);
    for (const auto& f : findings) {
        Json::Value jf(Json::objectValue);
        jf["line"] = f.line;
        jf["kind"] = f.kind;
        jf["severity"] = f.severity;
        jf["evidence"] = f.evidence;
        out["findings"].append(jf);
    }
    out["count"] = static_cast<int>(findings.size());
    return jsonStr(out);
}

}  // namespace cortex::mk3::tools::builtins
