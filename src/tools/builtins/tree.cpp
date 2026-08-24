// src/tools/builtins/tree.cpp — native directory map (no Python spawn)
// Ports manifests/tools/tree/src/main.py.
#include "tree.hpp"
#include "common.hpp"
#include "src/core/run_control.hpp"

#include <fnmatch.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace cortex::mk3::tools::builtins {

static bool globMatch(const std::string& name, const std::string& rel,
                      const std::vector<std::string>& patterns) {
    for (const auto& pat : patterns) {
        if (pat.empty()) continue;
        if (fnmatch(pat.c_str(), name.c_str(), 0) == 0) return true;
        if (fnmatch(pat.c_str(), rel.c_str(), 0) == 0) return true;
    }
    return false;
}

static std::vector<std::string> loadGitignore(const fs::path& root) {
    std::vector<std::string> out;
    std::ifstream in(root / ".gitignore");
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            line.pop_back();
        size_t i = 0;
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= line.size() || line[i] == '#') continue;
        std::string pat = line.substr(i);
        while (!pat.empty() && pat.back() == '/') pat.pop_back();
        if (!pat.empty()) out.push_back(pat);
    }
    return out;
}

struct WalkStats {
    int dirs = 0;
    int files = 0;
    uint64_t totalSize = 0;
    int entries = 0;
    bool truncated = false;
    bool cancelled = false;
};

struct Node {
    std::string name;
    bool isDir = false;
    uint64_t size = 0;
    std::string error;
    bool depthCut = false;
    std::vector<Node> children;
};

static Node walk(const fs::path& dir, const fs::path& root, int depth, int maxDepth,
                 bool includeHidden, const std::vector<std::string>& exclude,
                 const std::vector<std::string>& extensions, int maxEntries,
                 WalkStats& st) {
    Node node;
    node.name = dir.filename().string();
    if (node.name.empty()) node.name = dir.string();
    node.isDir = true;
    if (!g_running) {
        st.cancelled = true;
        return node;
    }
    if (depth > maxDepth) {
        node.depthCut = true;
        return node;
    }
    std::error_code ec;
    std::vector<fs::directory_entry> kids;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        kids.push_back(*it);
    }
    std::sort(kids.begin(), kids.end(), [](const fs::directory_entry& a,
                                           const fs::directory_entry& b) {
        std::error_code e1, e2;
        bool af = a.is_regular_file(e1);
        bool bf = b.is_regular_file(e2);
        if (af != bf) return !af && bf;  // dirs first
        return a.path().filename().string() < b.path().filename().string();
    });

    for (const auto& child : kids) {
        if (!g_running) {
            st.cancelled = true;
            break;
        }
        if (st.entries >= maxEntries) {
            st.truncated = true;
            break;
        }
        const std::string name = child.path().filename().string();
        if (!includeHidden && !name.empty() && name[0] == '.') continue;
        if (skipDirName(name)) continue;
        std::error_code rec;
        std::string rel = fs::relative(child.path(), root, rec).string();
        if (rec) rel = name;
        if (globMatch(name, rel, exclude)) continue;

        if (child.is_directory(ec)) {
            ++st.dirs;
            ++st.entries;
            node.children.push_back(walk(child.path(), root, depth + 1, maxDepth,
                                         includeHidden, exclude, extensions, maxEntries, st));
        } else if (child.is_regular_file(ec)) {
            if (!extensions.empty()) {
                std::string ext = child.path().extension().string();
                if (!ext.empty() && ext[0] == '.') ext.erase(ext.begin());
                bool ok = false;
                for (const auto& e : extensions)
                    if (e == ext) {
                        ok = true;
                        break;
                    }
                if (!ok) continue;
            }
            ++st.files;
            ++st.entries;
            Node f;
            f.name = name;
            f.isDir = false;
            std::error_code se;
            f.size = child.file_size(se);
            if (se) f.size = 0;
            st.totalSize += f.size;
            node.children.push_back(std::move(f));
        }
    }
    return node;
}

static void renderAscii(const Node& n, const std::string& prefix, bool isLast,
                        bool isRoot, std::ostringstream& out) {
    if (isRoot) {
        out << n.name << "\n";
        for (size_t i = 0; i < n.children.size(); ++i)
            renderAscii(n.children[i], "", i + 1 == n.children.size(), false, out);
        return;
    }
    out << prefix << (isLast ? "└── " : "├── ") << n.name;
    if (!n.isDir)
        out << " (" << n.size << "B)";
    else if (n.depthCut)
        out << " [depth limit]";
    else if (!n.error.empty())
        out << " [" << n.error << "]";
    out << "\n";
    if (n.isDir) {
        std::string ext = prefix + (isLast ? "    " : "│   ");
        for (size_t i = 0; i < n.children.size(); ++i)
            renderAscii(n.children[i], ext, i + 1 == n.children.size(), false, out);
    }
}

static Json::Value nodeJson(const Node& n) {
    Json::Value j;
    j["name"] = n.name;
    j["type"] = n.isDir ? "dir" : "file";
    if (!n.isDir) j["size"] = static_cast<Json::UInt64>(n.size);
    if (n.depthCut) j["truncated"] = "max_depth";
    if (!n.error.empty()) j["error"] = n.error;
    if (n.isDir) {
        Json::Value kids(Json::arrayValue);
        for (const auto& c : n.children) kids.append(nodeJson(c));
        j["children"] = kids;
    }
    return j;
}

std::string tree(const Json::Value& p) {
    return treeStreaming(p, {});
}

std::string treeStreaming(const Json::Value& p,
                          const std::function<void(const std::string&, bool)>& stream) {
    std::string path = p.get("path", ".").asString();
    if (path.empty()) path = ".";
    int maxDepth = std::clamp(p.get("max_depth", 6).asInt(), 0, 32);
    bool includeHidden = p.get("include_hidden", false).asBool();
    bool respectGit = p.get("gitignore", true).asBool();
    int maxEntries = std::clamp(p.get("max_entries", 2000).asInt(), 1, 20000);
    std::string fmt = p.get("format", "tree").asString();

    std::vector<std::string> extensions;
    if (p.isMember("extensions") && p["extensions"].isArray()) {
        for (const auto& v : p["extensions"]) {
            if (!v.isString()) continue;
            std::string e = v.asString();
            if (!e.empty() && e[0] == '.') e.erase(e.begin());
            if (!e.empty()) extensions.push_back(e);
        }
    }
    std::vector<std::string> exclude = {
        "*.pyc", ".DS_Store",
    };
    if (p.isMember("exclude") && p["exclude"].isArray()) {
        for (const auto& v : p["exclude"])
            if (v.isString()) exclude.push_back(v.asString());
    }

    std::error_code ec;
    fs::path root(path);
    if (!fs::exists(root, ec))
        return jsonErr("path not found: " + path);
    if (!fs::is_directory(root, ec))
        return jsonErr("not a directory: " + path);

    if (respectGit) {
        auto gi = loadGitignore(root);
        exclude.insert(exclude.end(), gi.begin(), gi.end());
    }

    WalkStats st;
    Node n = walk(root, root, 0, maxDepth, includeHidden, exclude, extensions, maxEntries, st);

    Json::Value r;
    r["success"] = !st.cancelled;
    if (st.cancelled) r["error"] = "tree cancelled (operator stop)";
    r["root"] = fs::weakly_canonical(root, ec).string();
    if (r["root"].asString().empty()) r["root"] = root.string();
    Json::Value stats;
    stats["dirs"] = st.dirs;
    stats["files"] = st.files;
    stats["total_size"] = static_cast<Json::UInt64>(st.totalSize);
    r["stats"] = stats;
    r["truncated"] = st.truncated || n.depthCut;

    if (fmt == "json") {
        r["tree"] = nodeJson(n);
        Json::StreamWriterBuilder w;
        w["indentation"] = "";
        r["results"] = Json::writeString(w, r["tree"]);
        r["output"] = r["results"];
    } else {
        std::ostringstream ascii;
        renderAscii(n, "", true, true, ascii);
        std::string s = ascii.str();
        r["tree"] = s;
        r["results"] = s;
        r["output"] = s;
        if (stream) stream(s, false);
    }
    return jsonStr(r);
}

}  // namespace cortex::mk3::tools::builtins
