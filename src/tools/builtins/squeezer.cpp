// src/tools/builtins/squeezer.cpp — native functional map (no Python spawn)
// Ports manifests/tools/squeezer/src/main.py: signatures/imports, drop bodies.
#include "squeezer.hpp"
#include "common.hpp"
#include "src/core/run_control.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace cortex::mk3::tools::builtins {

static std::string lowerExt(std::string e) {
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

static const char* langOf(const fs::path& p) {
    const std::string e = lowerExt(p.extension().string());
    if (e == ".py") return "python";
    if (e == ".c" || e == ".h") return "c";
    if (e == ".cpp" || e == ".hpp" || e == ".cc" || e == ".cxx" || e == ".hh")
        return "cpp";
    if (e == ".js" || e == ".jsx" || e == ".mjs") return "javascript";
    if (e == ".ts" || e == ".tsx") return "typescript";
    if (e == ".go") return "go";
    return nullptr;
}

static bool looksBinaryOrHuge(const fs::path& file, std::error_code& ec) {
    auto sz = fs::file_size(file, ec);
    if (ec) return true;
    if (sz > 2 * 1024 * 1024) return true;
    return false;
}

static void trimInPlace(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
}

static void squeezePython(const std::string& src, Json::Value& imports,
                          Json::Value& signatures) {
    std::istringstream in(src);
    std::string line;
    std::string classIndent;
    while (std::getline(in, line)) {
        std::string t = line;
        trimInPlace(t);
        if (t.rfind("import ", 0) == 0 || t.rfind("from ", 0) == 0) {
            if (t.find(" import ") != std::string::npos || t.rfind("import ", 0) == 0)
                imports.append(t);
            continue;
        }
        size_t indent = 0;
        while (indent < line.size() &&
               (line[indent] == ' ' || line[indent] == '\t'))
            ++indent;
        bool isClass = t.rfind("class ", 0) == 0;
        bool isDef = t.rfind("def ", 0) == 0 || t.rfind("async def ", 0) == 0;
        if (isClass) {
            classIndent = line.substr(0, indent);
            size_t cut = t.find(':');
            signatures.append(cut == std::string::npos ? t : t.substr(0, cut));
            continue;
        }
        if (isDef) {
            size_t cut = t.find(':');
            std::string sig = cut == std::string::npos ? t : t.substr(0, cut);
            if (indent > classIndent.size() && !classIndent.empty())
                signatures.append(std::string("    ") + sig);
            else
                signatures.append(sig);
        }
        if (indent == 0) classIndent.clear();
    }
}

static bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':';
}

static void squeezeCFamily(const std::string& src, Json::Value& imports,
                           Json::Value& signatures) {
    static const std::regex inc(R"(^\s*#include\s+[<"]([^>"]+)[>"])",
                                std::regex::ECMAScript);
    static const std::regex cls(R"(^\s*(?:class|struct)\s+(\w+))",
                                std::regex::ECMAScript);
    std::istringstream in(src);
    std::string line;
    std::string acc;
    int paren = 0;
    auto flushDecl = [&]() {
        std::string sig = acc;
        acc.clear();
        paren = 0;
        trimInPlace(sig);
        while (!sig.empty() && (sig.back() == '{' || sig.back() == ';' ||
                                std::isspace(static_cast<unsigned char>(sig.back()))))
            sig.pop_back();
        trimInPlace(sig);
        size_t lp = sig.find('(');
        if (lp == std::string::npos || lp == 0) return;
        size_t i = lp;
        while (i > 0 && std::isspace(static_cast<unsigned char>(sig[i - 1]))) --i;
        size_t endName = i;
        while (i > 0 && isIdentChar(sig[i - 1])) --i;
        std::string name = sig.substr(i, endName - i);
        if (name == "if" || name == "while" || name == "for" || name == "switch" ||
            name == "return" || name == "catch")
            return;
        if (name.size() < 2) return;
        signatures.append(sig);
    };
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, inc)) imports.append(m[1].str());
        if (std::regex_search(line, m, cls))
            signatures.append(m[0].str());
        std::string t = line;
        trimInPlace(t);
        if (t.rfind("//", 0) == 0) continue;
        for (char c : line) {
            if (c == '(') {
                ++paren;
                acc.push_back(c);
            } else if (c == ')') {
                if (paren > 0) --paren;
                acc.push_back(c);
            } else if ((c == '{' || c == ';') && paren == 0) {
                if (acc.find('(') != std::string::npos) flushDecl();
                else acc.clear();
            } else {
                acc.push_back(c);
            }
        }
        if (!acc.empty() && acc.back() != ' ') acc.push_back(' ');
        if (acc.size() > 4000) acc.clear();
    }
}

static void squeezeJsTs(const std::string& src, Json::Value& imports,
                        Json::Value& signatures) {
    static const std::regex fromImp(
        R"(^\s*import\s+.*?from\s+['"]([^'"]+)['"])", std::regex::ECMAScript);
    static const std::regex req(R"(require\(['"]([^'"]+)['"]\))",
                                std::regex::ECMAScript);
    static const std::regex fn(
        R"(^\s*(?:export\s+)?(?:async\s+)?function\s+\w+\s*\([^)]*\))",
        std::regex::ECMAScript);
    static const std::regex arrow(
        R"(^\s*(?:export\s+)?const\s+\w+\s*=\s*(?:async\s+)?\([^)]*\)\s*=>)",
        std::regex::ECMAScript);
    static const std::regex cls(R"(^\s*(?:export\s+)?class\s+(\w+))",
                                std::regex::ECMAScript);
    static const std::regex iface(R"(^\s*(?:export\s+)?interface\s+\w+[^\{]*)",
                                  std::regex::ECMAScript);
    static const std::regex typ(R"(^\s*(?:export\s+)?type\s+\w+\s*=)",
                                std::regex::ECMAScript);
    std::istringstream in(src);
    std::string line;
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, fromImp)) imports.append(m[1].str());
        if (std::regex_search(line, m, req)) imports.append(m[1].str());
        if (std::regex_search(line, m, fn)) {
            std::string s = m[0].str();
            trimInPlace(s);
            signatures.append(s);
        }
        if (std::regex_search(line, m, arrow)) {
            std::string s = m[0].str();
            trimInPlace(s);
            signatures.append(s);
        }
        if (std::regex_search(line, m, cls))
            signatures.append(std::string("class ") + m[1].str());
        if (std::regex_search(line, m, iface)) {
            std::string s = m[0].str();
            trimInPlace(s);
            signatures.append(s);
        }
        if (std::regex_search(line, m, typ)) {
            std::string s = m[0].str();
            trimInPlace(s);
            signatures.append(s);
        }
    }
}

static void squeezeGo(const std::string& src, Json::Value& imports,
                      Json::Value& signatures) {
    static const std::regex pkgImp(R"re(^\s*"([^"]+)")re", std::regex::ECMAScript);
    static const std::regex oneImp(R"re(^import\s+"([^"]+)")re", std::regex::ECMAScript);
    static const std::regex fn(R"(^(func\s+[\w.\*()\s]*?\w+\s*\([^)]*\)[^{]*)\{)",
                               std::regex::ECMAScript);
    static const std::regex typ(R"(^(type\s+\w+\s+(?:struct|interface)\s*\{))",
                                std::regex::ECMAScript);
    std::istringstream in(src);
    std::string line;
    bool inImport = false;
    while (std::getline(in, line)) {
        std::string t = line;
        trimInPlace(t);
        if (t.rfind("import (", 0) == 0) {
            inImport = true;
            continue;
        }
        if (inImport) {
            if (t == ")") {
                inImport = false;
                continue;
            }
            std::smatch m;
            if (std::regex_search(line, m, pkgImp)) imports.append(m[1].str());
            continue;
        }
        std::smatch m;
        if (std::regex_search(line, m, oneImp)) imports.append(m[1].str());
        if (std::regex_search(line, m, fn)) {
            std::string s = m[1].str();
            trimInPlace(s);
            signatures.append(s);
        }
        if (std::regex_search(line, m, typ)) {
            std::string s = m[1].str();
            while (!s.empty() && (s.back() == '{' ||
                                  std::isspace(static_cast<unsigned char>(s.back()))))
                s.pop_back();
            trimInPlace(s);
            signatures.append(s);
        }
    }
}

static Json::Value squeezeFile(const fs::path& path, const char* lang) {
    Json::Value rec;
    rec["path"] = path.string();
    rec["language"] = lang;
    std::ifstream in(path);
    if (!in) {
        rec["skipped_reason"] = "read error";
        return rec;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string src = buf.str();
    Json::Value imports(Json::arrayValue);
    Json::Value signatures(Json::arrayValue);
    if (std::string(lang) == "python")
        squeezePython(src, imports, signatures);
    else if (std::string(lang) == "c" || std::string(lang) == "cpp")
        squeezeCFamily(src, imports, signatures);
    else if (std::string(lang) == "javascript" || std::string(lang) == "typescript")
        squeezeJsTs(src, imports, signatures);
    else if (std::string(lang) == "go")
        squeezeGo(src, imports, signatures);
    rec["imports"] = imports;
    rec["signatures"] = signatures;
    rec["best_effort"] = true;
    return rec;
}

std::string squeezer(const Json::Value& p) {
    return squeezerStreaming(p, {});
}

std::string squeezerStreaming(const Json::Value& p,
                              const std::function<void(const std::string&, bool)>& stream) {
    if (!p.isMember("path") || !p["path"].isString() || p["path"].asString().empty())
        return jsonErr("path is required");
    const std::string path = p["path"].asString();
    const bool recursive = p.get("recursive", true).asBool();
    int maxFiles = std::clamp(p.get("max_files", 200).asInt(), 1, 2000);

    std::unordered_set<std::string> langFilter;
    if (p.isMember("languages") && p["languages"].isArray()) {
        for (const auto& v : p["languages"]) {
            if (v.isString()) langFilter.insert(v.asString());
        }
    }

    std::error_code ec;
    fs::path root(path);
    if (!fs::exists(root, ec))
        return jsonErr("path not found: " + path);

    Json::Value files(Json::arrayValue);
    int processed = 0;
    int skipped = 0;
    int totalSigs = 0;
    bool capped = false;
    bool cancelled = false;
    std::ostringstream text;

    auto acceptLang = [&](const char* lang) {
        if (!lang) return false;
        if (langFilter.empty()) return true;
        return langFilter.count(lang) > 0;
    };

    auto handleFile = [&](const fs::path& f, bool mustReport) {
        if (!g_running) {
            cancelled = true;
            return;
        }
        if (processed >= maxFiles) {
            capped = true;
            return;
        }
        const char* lang = langOf(f);
        if (!acceptLang(lang)) {
            if (!mustReport) return;  // dir walk: silent skip, same as Python collect_files
            ++skipped;
            Json::Value rec;
            rec["path"] = f.string();
            rec["skipped_reason"] =
                lang ? "language filtered" : ("unsupported extension: " + f.extension().string());
            files.append(rec);
            return;
        }
        if (looksBinaryOrHuge(f, ec)) {
            ++skipped;
            Json::Value rec;
            rec["path"] = f.string();
            rec["skipped_reason"] = "binary or too large";
            files.append(rec);
            return;
        }
        Json::Value rec = squeezeFile(f, lang);
        int n = rec.isMember("signatures") ? static_cast<int>(rec["signatures"].size()) : 0;
        totalSigs += n;
        ++processed;
        files.append(rec);
        std::ostringstream line;
        line << rec["language"].asString() << "  " << rec["path"].asString()
             << "  sigs=" << n << "\n";
        if (rec.isMember("signatures")) {
            for (const auto& s : rec["signatures"])
                line << "  " << s.asString() << "\n";
        }
        std::string rendered = line.str();
        text << rendered;
        if (stream) stream(rendered, false);
    };

    if (fs::is_regular_file(root, ec)) {
        handleFile(root, true);
    } else if (fs::is_directory(root, ec)) {
        if (recursive) {
            for (fs::recursive_directory_iterator it(
                     root, fs::directory_options::skip_permission_denied, ec),
                 end;
                 !ec && it != end && !capped && !cancelled; it.increment(ec)) {
                if (!g_running) {
                    cancelled = true;
                    break;
                }
                if (it->is_directory(ec)) {
                    if (skipDirName(it->path().filename().string()))
                        it.disable_recursion_pending();
                    continue;
                }
                if (it->is_regular_file(ec)) handleFile(it->path(), false);
            }
        } else {
            for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec),
                 end;
                 !ec && it != end && !capped && !cancelled; it.increment(ec)) {
                if (it->is_regular_file(ec)) handleFile(it->path(), false);
            }
        }
    } else {
        return jsonErr("unsupported path type: " + root.string());
    }

    Json::Value r;
    r["success"] = !cancelled;
    if (cancelled) r["error"] = "squeezer cancelled (operator stop)";
    r["root"] = fs::weakly_canonical(root, ec).string();
    if (r["root"].asString().empty()) r["root"] = root.string();
    r["files"] = files;
    Json::Value stats;
    stats["files_processed"] = processed;
    stats["files_skipped"] = skipped;
    stats["total_signatures"] = totalSigs;
    stats["truncated"] = capped;
    r["stats"] = stats;
    r["results"] = text.str();
    r["output"] = text.str();
    return jsonStr(r);
}

}  // namespace cortex::mk3::tools::builtins
