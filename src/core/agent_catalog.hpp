#pragma once
// =============================================================================
// agent-lib-MK3 — Global agent / manifest catalog
// Global surface is manifests/ only (agents live under manifests/agents/).
// =============================================================================

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits.h>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "mini_yaml.hpp"
#include "types.hpp"

namespace cortex {
namespace mk3 {
namespace catalog {

namespace fs = std::filesystem;

// One imported/owned surface entry (tool, feed, relic, sub-agent, workflow, context file).
struct OwnedItem {
    std::string name;     // bare name or relative path as declared
    std::string kind;     // tool | feed | relic | agent | workflow | harness | system | persona | env
    std::string resolved; // absolute path when known
    bool exists = false;  // path resolved and present, or builtin name recognized
    bool isPath = false;  // declared as path import
    bool isBuiltin = false;
};

struct AgentEntry {
    std::string name;
    std::string version;
    std::string summary;
    std::string manifestPath;  // absolute agent.yml
    std::string source;        // env | config | share | project | cwd | binary | override
    std::string root;          // manifests/agents directory that produced this entry
    std::string manifestsRoot; // parent manifests/ tree (if known)
    std::string provider;
    std::string model;
    std::vector<OwnedItem> owned;
};

inline std::string expandHome(const std::string& path) {
    if (path.empty() || path[0] != '~')
        return path;
    const char* home = std::getenv("HOME");
    if (!home)
        return path;
    if (path.size() == 1)
        return std::string(home);
    if (path[1] == '/')
        return std::string(home) + path.substr(1);
    return path;
}

inline std::string absPath(const fs::path& p) {
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    if (ec)
        return p.string();
    fs::path c = fs::weakly_canonical(a, ec);
    return ec ? a.string() : c.string();
}

inline bool isPathImport(const std::string& s) {
    if (s.empty())
        return false;
    if (s.find('/') != std::string::npos)
        return true;
    if (s.rfind("./", 0) == 0 || s.rfind("../", 0) == 0)
        return true;
    if (s.size() > 4 && (s.compare(s.size() - 4, 4, ".yml") == 0 ||
                         s.compare(s.size() - 5, 5, ".yaml") == 0))
        return true;
    return false;
}

// Walk up from a file or directory until a manifests/ root is found.
// Accepts: manifests/, parent-of-manifests/, agents/, agent.yml, tool.yml, etc.
inline fs::path resolveManifestsRoot(const fs::path& start) {
    std::error_code ec;
    if (start.empty())
        return {};
    fs::path p = start;
    if (fs::is_regular_file(p, ec))
        p = p.parent_path();
    if (!fs::exists(p, ec))
        return {};
    for (int i = 0; i < 12; ++i) {
        if (p.filename() == "manifests" && fs::is_directory(p, ec))
            return p;
        if (fs::is_directory(p / "manifests", ec))
            return p / "manifests";
        // .../manifests/agents/<name> or .../manifests/built-in/tools/<name>
        if (p.filename() == "agents" || p.filename() == "built-in" ||
            p.filename() == "workflows" || p.filename() == "tools" ||
            p.filename() == "feeds" || p.filename() == "harness") {
            fs::path parent = p.parent_path();
            if (parent.filename() == "manifests" && fs::is_directory(parent, ec))
                return parent;
            if (parent.filename() == "built-in") {
                fs::path gp = parent.parent_path();
                if (gp.filename() == "manifests" && fs::is_directory(gp, ec))
                    return gp;
            }
        }
        if (!p.has_parent_path() || p == p.root_path())
            break;
        p = p.parent_path();
    }
    return {};
}

inline bool manifestsRootHasContent(const fs::path& mroot) {
    std::error_code ec;
    if (!fs::is_directory(mroot, ec))
        return false;
    // Require at least one known PROD subdir so empty placeholder dirs
    // (e.g. stray ~/repos/manifests) do not become the only "root".
    for (const char* sub : {"agents", "built-in", "workflows", "harness"}) {
        if (fs::is_directory(mroot / sub, ec))
            return true;
    }
    return false;
}

// Only manifests/ trees are global. Roots returned are .../manifests directories.
// Never pass an agent.yml path as a dead-end — resolveManifestsRoot walks up.
inline std::vector<std::pair<std::string, std::string>> manifestsSearchRoots(
    const std::string& manifestDirOverride = "") {
    std::vector<std::pair<std::string, std::string>> roots;  // {manifestsPath, source}
    std::set<std::string> seen;

    auto addManifests = [&](const std::string& raw, const std::string& source,
                            bool requireContent = true) {
        if (raw.empty())
            return;
        std::string expanded = expandHome(raw);
        std::error_code ec;
        if (!fs::exists(expanded, ec))
            return;
        fs::path m = resolveManifestsRoot(expanded);
        if (m.empty() || !fs::is_directory(m, ec))
            return;
        if (requireContent && !manifestsRootHasContent(m))
            return;
        std::string key = absPath(m);
        if (!seen.insert(key).second)
            return;
        roots.push_back({key, source});
    };

    // 0. Explicit override FIRST (CLI --manifest-dir, or agent.yml → walk-up).
    //    requireContent=false so a freshly seeded manifests/ still counts.
    if (!manifestDirOverride.empty())
        addManifests(manifestDirOverride, "override", /*requireContent=*/false);

    // 1. CORTEX_HOME
    if (const char* ch = std::getenv("CORTEX_HOME")) {
        addManifests(std::string(ch) + "/manifests", "env");
        addManifests(ch, "env");
    }

    // 2. XDG config / data
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"))
        addManifests(std::string(xdg) + "/cortex/manifests", "config");
    else
        addManifests("~/.config/cortex/manifests", "config");

    if (const char* xdgData = std::getenv("XDG_DATA_HOME"))
        addManifests(std::string(xdgData) + "/cortex/manifests", "share");
    else
        addManifests("~/.local/share/cortex/manifests", "share");

    // 3. CWD + walk-up (skip empty placeholder manifests dirs)
    addManifests("./manifests", "cwd");
    {
        std::error_code ec;
        fs::path cur = fs::current_path(ec);
        for (int i = 0; i < 10 && !ec; ++i) {
            addManifests((cur / "manifests").string(), "project");
            if (!cur.has_parent_path() || cur == cur.root_path())
                break;
            cur = cur.parent_path();
        }
    }

    // 4. Binary-adjacent (dev binary in repo root, or PREFIX/share)
    {
        char buf[PATH_MAX];
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            fs::path exe = fs::path(buf).parent_path();
            addManifests((exe / "manifests").string(), "binary");
            addManifests(exe.string(), "binary");  // walk-up from binary dir
            addManifests((exe / ".." / "share" / "cortex" / "manifests").string(), "binary");
            addManifests((exe / ".." / "share" / "cortex").string(), "binary");
            // walk a few parents from the binary (repo layouts)
            fs::path cur = exe;
            for (int i = 0; i < 6; ++i) {
                addManifests((cur / "manifests").string(), "binary");
                if (!cur.has_parent_path() || cur == cur.root_path())
                    break;
                cur = cur.parent_path();
            }
        }
    }

    return roots;
}

// Agent catalogs are always manifests/agents under a manifests root.
inline std::vector<std::pair<std::string, std::string>> agentSearchRoots(
    const std::string& manifestDirOverride = "") {
    std::vector<std::pair<std::string, std::string>> roots;
    std::set<std::string> seen;
    for (const auto& [mroot, source] : manifestsSearchRoots(manifestDirOverride)) {
        fs::path agents = fs::path(mroot) / "agents";
        std::error_code ec;
        if (!fs::is_directory(agents, ec))
            continue;
        std::string key = absPath(agents);
        if (!seen.insert(key).second)
            continue;
        roots.push_back({key, source});
    }
    return roots;
}

inline std::vector<std::string> sharedSearchRoots(const std::string& manifestDirOverride = "") {
    std::vector<std::string> roots;
    for (const auto& [mroot, source] : manifestsSearchRoots(manifestDirOverride)) {
        (void)source;
        roots.push_back(mroot);
    }
    return roots;
}

inline std::string findShared(const std::string& relative,
                              const std::string& manifestDirOverride = "") {
    // relative e.g. "harness/default.md" or "manifests/harness/default.md"
    for (const auto& root : sharedSearchRoots(manifestDirOverride)) {
        fs::path cand = fs::path(root) / relative;
        std::error_code ec;
        if (fs::is_regular_file(cand, ec))
            return absPath(cand);
        if (relative.rfind("manifests/", 0) == 0) {
            cand = fs::path(root) / relative.substr(std::string("manifests/").size());
            if (fs::is_regular_file(cand, ec))
                return absPath(cand);
        }
    }
    return {};
}

// Known built-in tool names (stdlib) — shown as ◆ builtin in the tree.
inline bool isBuiltinToolName(const std::string& name) {
    static const std::set<std::string> k = {
        "exec",        "list",         "grep",         "fs_read",      "fs_write",
        "json",        "web_fetch",    "sleep",        "artifact",     "ask_tool",
        "context_pin", "context_peek", "context_unpin"};
    return k.count(name) > 0;
}

inline OwnedItem makeOwned(const std::string& raw, const std::string& kind,
                           const fs::path& agentDir, const fs::path& manifestsRoot) {
    OwnedItem item;
    item.name = raw;
    item.kind = kind;
    item.isPath = isPathImport(raw);
    std::error_code ec;

    if (item.isPath) {
        fs::path p = fs::path(raw);
        if (!p.is_absolute())
            p = agentDir / p;
        if (fs::exists(p, ec)) {
            item.resolved = absPath(p);
            item.exists = true;
        } else {
            item.resolved = p.string();
            item.exists = false;
        }
        return item;
    }

    // Bare name resolution by kind
    if (kind == "tool") {
        item.isBuiltin = isBuiltinToolName(raw);
        // Prefer built-in manifest, then tools/<name>
        std::vector<fs::path> cands = {
            manifestsRoot / "built-in" / "tools" / raw / "tool.yml",
            manifestsRoot / "tools" / raw / "tool.yml",
            manifestsRoot / "tools" / (raw + ".yml"),
        };
        for (const auto& c : cands) {
            if (fs::is_regular_file(c, ec)) {
                item.resolved = absPath(c);
                item.exists = true;
                return item;
            }
        }
        item.exists = item.isBuiltin;  // builtin without yml still "ok"
        return item;
    }
    if (kind == "feed") {
        std::vector<fs::path> cands = {
            manifestsRoot / "built-in" / "feeds" / raw / "feed.yml",
            manifestsRoot / "feeds" / raw / "feed.yml",
            manifestsRoot / "feeds" / (raw + ".yml"),
        };
        for (const auto& c : cands) {
            if (fs::is_regular_file(c, ec)) {
                item.resolved = absPath(c);
                item.exists = true;
                return item;
            }
        }
        return item;
    }
    if (kind == "relic") {
        std::vector<fs::path> cands = {
            manifestsRoot / "relics" / raw / "relic.yml",
            manifestsRoot / "relics" / (raw + ".yml"),
        };
        for (const auto& c : cands) {
            if (fs::is_regular_file(c, ec)) {
                item.resolved = absPath(c);
                item.exists = true;
                return item;
            }
        }
        return item;
    }
    if (kind == "agent") {
        std::vector<fs::path> cands = {
            manifestsRoot / "agents" / raw / "agent.yml",
            manifestsRoot / "agents" / (raw + ".yml"),
        };
        for (const auto& c : cands) {
            if (fs::is_regular_file(c, ec)) {
                item.resolved = absPath(c);
                item.exists = true;
                return item;
            }
        }
        return item;
    }
    if (kind == "workflow") {
        std::vector<fs::path> cands = {
            manifestsRoot / "workflows" / (raw + ".yml"),
            manifestsRoot / "workflows" / raw / "workflow.yml",
        };
        for (const auto& c : cands) {
            if (fs::is_regular_file(c, ec)) {
                item.resolved = absPath(c);
                item.exists = true;
                return item;
            }
        }
        return item;
    }
    return item;
}

inline void parseOwnership(AgentEntry& e) {
    e.owned.clear();
    std::ifstream in(e.manifestPath);
    if (!in)
        return;
    std::ostringstream ss;
    ss << in.rdbuf();
    auto root = ManifestYaml::parse(ss.str());

    fs::path agentDir = fs::path(e.manifestPath).parent_path();
    fs::path manifestsRoot = e.manifestsRoot.empty() ? agentDir.parent_path().parent_path()
                                                     : fs::path(e.manifestsRoot);
    // Prefer real manifests/ parent if agent lives under manifests/agents/<name>
    if (agentDir.parent_path().filename() == "agents" &&
        agentDir.parent_path().parent_path().filename() == "manifests") {
        manifestsRoot = agentDir.parent_path().parent_path();
        e.manifestsRoot = absPath(manifestsRoot);
    }

    auto* engine = ManifestYaml::find(root, "cognitive_engine");
    if (engine) {
        auto* primary = ManifestYaml::find(*engine, "primary");
        if (primary) {
            e.provider = ManifestYaml::get(*primary, "provider");
            e.model = ManifestYaml::get(*primary, "model");
        }
    }

    auto* context = ManifestYaml::find(root, "context");
    auto addCtx = [&](const char* key, const char* kind) {
        if (!context)
            return;
        std::string rel = ManifestYaml::get(*context, key);
        if (rel.empty())
            return;
        OwnedItem item;
        item.name = rel;
        item.kind = kind;
        item.isPath = true;
        fs::path p = agentDir / rel;
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) {
            item.resolved = absPath(p);
            item.exists = true;
        } else {
            item.resolved = p.string();
            item.exists = false;
        }
        e.owned.push_back(item);
    };
    addCtx("harness", "harness");
    addCtx("system", "system");
    addCtx("persona", "persona");
    addCtx("user", "user");
    addCtx("user_context", "user");
    addCtx("operator", "user");

    auto* importNode = ManifestYaml::find(root, "import");
    if (!importNode)
        return;

    auto loadList = [&](const char* key, const char* kind) {
        auto names = ManifestYaml::getList(*importNode, key);
        for (auto name : names) {
            if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
                name = name.substr(1, name.size() - 2);
            if (name.empty() || name[0] == '#')
                continue;
            e.owned.push_back(makeOwned(name, kind, agentDir, manifestsRoot));
        }
    };
    loadList("tools", "tool");
    loadList("feeds", "feed");
    loadList("relics", "relic");
    loadList("agents", "agent");
    loadList("workflows", "workflow");
    loadList("env", "env");
}

// ASCII ownership tree for TUI / list output.
inline std::vector<std::string> formatOwnershipTree(const AgentEntry& e, bool color = false) {
    auto dim = [&](const std::string& s) -> std::string {
        return color ? ("\033[2m" + s + "\033[0m") : s;
    };
    auto green = [&](const std::string& s) -> std::string {
        return color ? ("\033[32m" + s + "\033[0m") : s;
    };
    auto red = [&](const std::string& s) -> std::string {
        return color ? ("\033[31m" + s + "\033[0m") : s;
    };
    auto cyan = [&](const std::string& s) -> std::string {
        return color ? ("\033[36m" + s + "\033[0m") : s;
    };
    auto bold = [&](const std::string& s) -> std::string {
        return color ? ("\033[1m" + s + "\033[0m") : s;
    };

    std::vector<std::string> lines;
    std::string title = e.name + "  v" + e.version;
    if (!e.provider.empty() || !e.model.empty())
        title += "  ·  " + e.provider + (e.model.empty() ? "" : ("/" + e.model));
    lines.push_back(bold(title));
    if (!e.summary.empty())
        lines.push_back(dim("  " + e.summary));
    lines.push_back(dim("  " + e.manifestPath));

    // Group owned items
    std::map<std::string, std::vector<const OwnedItem*>> groups;
    for (const auto& it : e.owned)
        groups[it.kind].push_back(&it);

    auto kindLabel = [](const std::string& k) -> std::string {
        if (k == "tool")
            return "tools";
        if (k == "feed")
            return "feeds";
        if (k == "relic")
            return "relics";
        if (k == "agent")
            return "sub-agents";
        if (k == "workflow")
            return "workflows";
        if (k == "harness" || k == "system" || k == "persona" || k == "user")
            return "context";
        if (k == "env")
            return "env";
        return k;
    };

    // Merge context kinds into one group for display order
    std::vector<std::pair<std::string, std::vector<const OwnedItem*>>> sections;
    {
        std::vector<const OwnedItem*> ctx;
        for (const char* k : {"harness", "system", "persona", "user"}) {
            auto it = groups.find(k);
            if (it != groups.end())
                ctx.insert(ctx.end(), it->second.begin(), it->second.end());
        }
        if (!ctx.empty())
            sections.push_back({"context", ctx});
        for (const char* k : {"tool", "feed", "relic", "agent", "workflow", "env"}) {
            auto it = groups.find(k);
            if (it != groups.end() && !it->second.empty())
                sections.push_back({kindLabel(k), it->second});
        }
    }

    if (sections.empty()) {
        lines.push_back("  └── " + dim("(no imports declared)"));
        return lines;
    }

    for (size_t si = 0; si < sections.size(); ++si) {
        bool lastSec = (si + 1 == sections.size());
        const auto& secName = sections[si].first;
        const auto& items = sections[si].second;
        std::string branch = lastSec ? "└── " : "├── ";
        lines.push_back(branch + cyan(secName) + dim(" (" + std::to_string(items.size()) + ")"));

        for (size_t ii = 0; ii < items.size(); ++ii) {
            bool lastItem = (ii + 1 == items.size());
            std::string pad = lastSec ? "    " : "│   ";
            std::string ibranch = lastItem ? "└── " : "├── ";
            const OwnedItem* it = items[ii];

            std::string mark;
            if (it->isBuiltin && !it->isPath)
                mark = dim(" ◆");
            else if (it->exists)
                mark = green(" ✓");
            else if (it->isPath || !it->resolved.empty())
                mark = red(" ✗");
            else
                mark = dim(" ·");

            std::string line = pad + ibranch + it->name + mark;
            if (it->isBuiltin && !it->resolved.empty())
                line += dim("  builtin");
            else if (it->isBuiltin)
                line += dim("  builtin");
            lines.push_back(line);

            // Optional second line: resolved path (short)
            if (it->isPath && !it->resolved.empty()) {
                std::string rp = it->resolved;
                // Prefer path relative to manifests root when possible
                if (!e.manifestsRoot.empty() && rp.rfind(e.manifestsRoot, 0) == 0) {
                    rp = "manifests" + rp.substr(e.manifestsRoot.size());
                }
                if (rp.size() > 64)
                    rp = "…" + rp.substr(rp.size() - 63);
                lines.push_back(pad + (lastItem ? "    " : "│   ") + dim(rp));
            }
        }
    }
    return lines;
}

inline AgentEntry readAgentMeta(const fs::path& manifestPath, const std::string& source,
                                const std::string& agentsRoot) {
    AgentEntry e;
    e.manifestPath = absPath(manifestPath);
    e.source = source;
    e.root = agentsRoot;
    // agentsRoot is .../manifests/agents → manifests root is parent
    fs::path ar(agentsRoot);
    if (ar.filename() == "agents")
        e.manifestsRoot = absPath(ar.parent_path());
    e.name = manifestPath.parent_path().filename().string();
    if (e.name.empty() || e.name == "." || e.name == "/")
        e.name = manifestPath.stem().string();

    std::ifstream in(manifestPath);
    if (!in)
        return e;
    std::ostringstream ss;
    ss << in.rdbuf();
    auto rootNode = ManifestYaml::parse(ss.str());
    auto* kind = ManifestYaml::find(rootNode, "kind");
    if (kind && kind->value != "Agent") {
        e.name.clear();
        return e;
    }
    std::string n = ManifestYaml::get(rootNode, "name");
    if (!n.empty())
        e.name = n;
    e.version = ManifestYaml::get(rootNode, "version", "1.0");
    e.summary = ManifestYaml::get(rootNode, "summary");
    parseOwnership(e);
    return e;
}

inline void scanRoot(const std::string& agentsRoot, const std::string& source,
                     std::map<std::string, AgentEntry>& byName) {
    std::error_code ec;
    if (!fs::is_directory(agentsRoot, ec))
        return;

    for (auto it = fs::directory_iterator(agentsRoot, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        if (!it->is_directory()) {
            if (it->path().extension() == ".yml" || it->path().extension() == ".yaml") {
                auto e = readAgentMeta(it->path(), source, agentsRoot);
                if (e.name.empty())
                    continue;
                if (!byName.count(e.name))
                    byName[e.name] = std::move(e);
            }
            continue;
        }
        fs::path agentYml = it->path() / "agent.yml";
        if (!fs::is_regular_file(agentYml, ec))
            agentYml = it->path() / "agent.yaml";
        if (!fs::is_regular_file(agentYml, ec))
            continue;
        auto e = readAgentMeta(agentYml, source, agentsRoot);
        if (e.name.empty())
            continue;
        if (!byName.count(e.name))
            byName[e.name] = std::move(e);
    }
}

inline std::vector<AgentEntry> discoverAgents(const std::string& manifestDirOverride = "") {
    std::map<std::string, AgentEntry> byName;
    for (const auto& [root, source] : agentSearchRoots(manifestDirOverride))
        scanRoot(root, source, byName);

    std::vector<AgentEntry> out;
    out.reserve(byName.size());
    for (auto& kv : byName)
        out.push_back(std::move(kv.second));
    std::sort(out.begin(), out.end(),
              [](const AgentEntry& a, const AgentEntry& b) { return a.name < b.name; });
    return out;
}

inline bool looksLikePath(const std::string& s) {
    return isPathImport(s);
}

// Resolve name-or-path to an absolute agent.yml path. Empty on failure.
inline std::string resolveAgent(const std::string& nameOrPath,
                                const std::string& manifestDirOverride = "",
                                std::string* err = nullptr) {
    if (nameOrPath.empty()) {
        if (err)
            *err = "empty agent name";
        return {};
    }

    if (looksLikePath(nameOrPath)) {
        fs::path p = expandHome(nameOrPath);
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            if (fs::is_regular_file(p / "agent.yml", ec))
                return absPath(p / "agent.yml");
            if (fs::is_regular_file(p / "agent.yaml", ec))
                return absPath(p / "agent.yaml");
            if (err)
                *err = "directory has no agent.yml: " + p.string();
            return {};
        }
        if (fs::is_regular_file(p, ec))
            return absPath(p);
        if (err)
            *err = "manifest path not found: " + p.string();
        return {};
    }

    for (const auto& [root, source] : agentSearchRoots(manifestDirOverride)) {
        (void)source;
        fs::path dir = fs::path(root) / nameOrPath;
        std::error_code ec;
        if (fs::is_regular_file(dir / "agent.yml", ec))
            return absPath(dir / "agent.yml");
        if (fs::is_regular_file(dir / "agent.yaml", ec))
            return absPath(dir / "agent.yaml");
        if (fs::is_regular_file(fs::path(root) / (nameOrPath + ".yml"), ec))
            return absPath(fs::path(root) / (nameOrPath + ".yml"));
        if (fs::is_regular_file(fs::path(root) / (nameOrPath + ".yaml"), ec))
            return absPath(fs::path(root) / (nameOrPath + ".yaml"));
    }

    if (err)
        *err = "agent not found under manifests/agents: " + nameOrPath +
               " (try `cortex-mk3 -m` or `cortex-mk3 list --agents`)";
    return {};
}

inline void fixDefaultPromptPaths(AgentConfig& cfg, const std::string& manifestDirOverride = "") {
    auto ensure = [&](std::string& path, const char* relA, const char* relB) {
        std::error_code ec;
        if (!path.empty() && fs::is_regular_file(path, ec))
            return;
        std::string found = findShared(relA, manifestDirOverride);
        if (found.empty())
            found = findShared(relB, manifestDirOverride);
        if (!found.empty())
            path = found;
    };
    ensure(cfg.harnessPath, "harness/default.md", "manifests/harness/default.md");
    ensure(cfg.systemPromptPath, "system/default.md", "manifests/system/default.md");
    ensure(cfg.personaPath, "persona/default.md", "manifests/persona/default.md");
}

// Resolve harness CLI/config value:
//   small|medium|big|default  → manifests/harness/<name>.md via catalog roots
//   path / existing file      → absolute path
// Empty string left empty (caller keeps manifest default).
// ── Full manifests tree discovery (hub / dashboard) ───────────────────
// Recursive under manifests/: agents, tools, feeds, relics, workflows,
// skills, harness, prompts. config/ is intentionally NOT scanned here
// (DEV/MVP only — pass as override if needed).
struct ManifestEntry {
    std::string kind;          // agent|tool|feed|relic|workflow|skill|harness|prompt|other
    std::string category;      // display category (often == kind, or nested path group)
    std::string name;
    std::string version;
    std::string summary;
    std::string description;   // longer body blurb (tool PE, skill prose, …)
    std::string path;          // absolute path to primary file
    std::string relPath;       // path relative to manifests root
    std::string source;        // cwd|project|env|...
    std::string manifestsRoot;
    std::string provider;      // agent only
    std::string model;         // agent only
    std::string runtime;       // tool/feed/relic runtime hint
    std::string entrypoint;    // tool/feed entrypoint
    std::vector<std::string> tags;
    std::vector<std::string> endpoints;  // relic endpoints when known
    std::vector<std::string> extraMeta;  // "key: value" lines for card (skill license, …)
    bool launchable = false;   // currently: agents only
    bool nested = false;       // specialist under another agent
    bool builtin = false;      // under built-in/
};

inline bool shouldSkipManifestDir(const fs::path& p) {
    std::string name = p.filename().string();
    if (name.empty() || name[0] == '.') return true;
    if (name == "archive" || name == "versions" || name == "_session" ||
        name == "node_modules" || name == "build")
        return true;
    return false;
}

inline std::string relToRoot(const fs::path& file, const fs::path& root) {
    std::string f = absPath(file);
    std::string r = absPath(root);
    if (f.rfind(r, 0) == 0) {
        std::string rel = f.substr(r.size());
        if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
        return rel.empty() ? f : rel;
    }
    return f;
}

inline void pushUnique(std::map<std::string, ManifestEntry>& byKey, ManifestEntry e) {
    if (e.name.empty() && e.path.empty()) return;
    std::string key = e.kind + ":" + (e.relPath.empty() ? e.path : e.relPath);
    if (!byKey.count(key))
        byKey[key] = std::move(e);
}

inline ManifestEntry readYamlManifestMeta(const fs::path& path, const std::string& kindHint,
                                          const std::string& source,
                                          const fs::path& manifestsRoot) {
    ManifestEntry e;
    e.path = absPath(path);
    e.source = source;
    e.manifestsRoot = absPath(manifestsRoot);
    e.relPath = relToRoot(path, manifestsRoot);
    e.kind = kindHint;
    e.name = path.parent_path().filename().string();
    if (e.name.empty() || e.name == "." || e.name == "/")
        e.name = path.stem().string();

    std::ifstream in(path);
    if (!in) return e;
    std::ostringstream ss;
    ss << in.rdbuf();
    auto root = ManifestYaml::parse(ss.str());

    std::string kindField = ManifestYaml::get(root, "kind");
    if (!kindField.empty()) {
        // Normalize Kind: Agent|Tool|Feed|Relic|Workflow
        std::string k = kindField;
        for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (k == "agent" || k == "tool" || k == "feed" || k == "relic" || k == "workflow")
            e.kind = k;
    }
    std::string n = ManifestYaml::get(root, "name");
    if (!n.empty()) e.name = n;
    e.version = ManifestYaml::get(root, "version", "");
    e.summary = ManifestYaml::get(root, "summary");
    e.description = ManifestYaml::get(root, "description");
    if (e.summary.empty()) e.summary = e.description;
    // Cap description for card paint (full body lives on kind pages).
    if (e.description.size() > 480)
        e.description = e.description.substr(0, 477) + "…";

    // Runtime surface (tool / feed / relic implementation blocks or top-level).
    e.runtime = ManifestYaml::get(root, "runtime");
    e.entrypoint = ManifestYaml::get(root, "entrypoint");
    auto* impl = ManifestYaml::find(root, "implementation");
    if (impl) {
        if (e.runtime.empty()) e.runtime = ManifestYaml::get(*impl, "runtime");
        if (e.entrypoint.empty()) e.entrypoint = ManifestYaml::get(*impl, "entrypoint");
    }
    // Relic endpoints: list of strings or {name/url} maps.
    if (e.kind == "relic" || kindHint == "relic") {
        auto* eps = ManifestYaml::find(root, "endpoints");
        if (eps) {
            for (const auto& child : eps->children) {
                std::string ep = child.value;
                if (ep.empty()) ep = ManifestYaml::get(child, "name");
                if (ep.empty()) ep = ManifestYaml::get(child, "url");
                if (ep.empty() && child.key == "name") ep = child.value;
                if (!ep.empty()) e.endpoints.push_back(ep);
            }
            // Scalar list items sometimes sit as key-only children
            if (e.endpoints.empty()) {
                for (const auto& child : eps->children) {
                    if (!child.key.empty() && child.value.empty())
                        e.endpoints.push_back(child.key);
                    else if (!child.key.empty() && !child.value.empty() &&
                             child.key != "name" && child.key != "url")
                        e.endpoints.push_back(child.key + ": " + child.value);
                }
            }
        }
    }

    // Explicit tags + category from YAML when present.
    e.tags = ManifestYaml::getList(root, "tags");
    // Flow-style: tags: [a, b, c]  (mini_yaml may leave as scalar value)
    if (e.tags.empty()) {
        auto* tn = ManifestYaml::find(root, "tags");
        if (tn && !tn->value.empty() && tn->value.front() == '[') {
            std::string inner = tn->value.substr(1);
            if (!inner.empty() && inner.back() == ']') inner.pop_back();
            std::istringstream iss(inner);
            std::string tok;
            while (std::getline(iss, tok, ',')) {
                // trim
                size_t a = tok.find_first_not_of(" \t\"'");
                size_t b = tok.find_last_not_of(" \t\"'");
                if (a == std::string::npos) continue;
                e.tags.push_back(tok.substr(a, b - a + 1));
            }
        }
    }
    e.category = ManifestYaml::get(root, "category");

    // Path-derived taxonomy (always available for hub grouping).
    e.builtin = e.relPath.rfind("built-in/", 0) == 0;
    if (e.kind == "agent") {
        // nested if path looks like agents/<parent>/agents/<child>
        e.nested = e.relPath.find("/agents/") != std::string::npos &&
                   e.relPath.rfind("agents/", 0) == 0 &&
                   std::count(e.relPath.begin(), e.relPath.end(), '/') >= 3;
        e.launchable = !e.nested;  // top-level agents only for -m bare name
        // Actually nested can still be launched by path; mark launchable always for agents.
        e.launchable = true;
        auto* engine = ManifestYaml::find(root, "cognitive_engine");
        if (engine) {
            auto* primary = ManifestYaml::find(*engine, "primary");
            if (primary) {
                e.provider = ManifestYaml::get(*primary, "provider");
                e.model = ManifestYaml::get(*primary, "model");
            }
        }
    }

    if (e.category.empty()) {
        if (e.kind == "agent" && e.nested)
            e.category = "specialist";
        else if (e.builtin)
            e.category = std::string("builtin-") + e.kind;
        else
            e.category = e.kind;
    }

    auto pushTag = [&](const std::string& t) {
        if (t.empty()) return;
        if (std::find(e.tags.begin(), e.tags.end(), t) == e.tags.end()) e.tags.push_back(t);
    };
    pushTag(e.kind);
    pushTag(e.category);
    if (e.builtin) pushTag("builtin");
    if (e.nested) pushTag("nested");
    if (e.launchable && e.kind == "agent" && !e.nested) pushTag("launchable");
    if (!e.provider.empty()) pushTag(e.provider);
    // Parent folder as soft tag for nested agents (coder/reader → parent:coder)
    if (e.nested) {
        // agents/<parent>/agents/<child>/agent.yml
        auto parts = e.relPath;
        // extract first segment after agents/
        if (parts.rfind("agents/", 0) == 0) {
            auto rest = parts.substr(7);
            auto slash = rest.find('/');
            if (slash != std::string::npos) pushTag(std::string("parent:") + rest.substr(0, slash));
        }
    }

    return e;
}

// SKILL.md — YAML frontmatter between --- fences (name, description, …).
// Body after the second --- becomes description fallback / prose blurb.
inline ManifestEntry readSkillManifestMeta(const fs::path& path, const std::string& source,
                                           const fs::path& manifestsRoot) {
    ManifestEntry e;
    e.kind = "skill";
    e.category = "skill";
    e.name = path.parent_path().filename().string();
    e.path = absPath(path);
    e.source = source;
    e.manifestsRoot = absPath(manifestsRoot);
    e.relPath = relToRoot(path, manifestsRoot);
    e.tags = {"skill", "policy"};

    std::ifstream in(path);
    if (!in) {
        e.summary = "skill module";
        return e;
    }
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (all.empty()) {
        e.summary = "skill module";
        return e;
    }

    // Normalize newlines lightly
    auto startsWithFence = [](const std::string& s, size_t pos) {
        return pos < s.size() && s.compare(pos, 3, "---") == 0;
    };
    size_t pos = 0;
    // Skip UTF-8 BOM
    if (all.size() >= 3 && static_cast<unsigned char>(all[0]) == 0xEF)
        pos = 3;
    // Optional leading whitespace/newlines before first ---
    while (pos < all.size() && (all[pos] == ' ' || all[pos] == '\t' || all[pos] == '\r' ||
                                all[pos] == '\n'))
        ++pos;

    std::string front;
    std::string body;
    if (startsWithFence(all, pos)) {
        size_t fmStart = pos + 3;
        if (fmStart < all.size() && all[fmStart] == '\r') ++fmStart;
        if (fmStart < all.size() && all[fmStart] == '\n') ++fmStart;
        size_t fmEnd = all.find("\n---", fmStart);
        if (fmEnd == std::string::npos) fmEnd = all.find("\r\n---", fmStart);
        if (fmEnd != std::string::npos) {
            front = all.substr(fmStart, fmEnd - fmStart);
            size_t bodyStart = all.find('\n', fmEnd + 1);
            if (bodyStart != std::string::npos) {
                ++bodyStart;
                if (bodyStart < all.size() && all[bodyStart] == '\n') ++bodyStart;
                body = all.substr(bodyStart);
            }
        } else {
            body = all;
        }
    } else {
        body = all;
    }

    if (!front.empty()) {
        auto root = ManifestYaml::parse(front);
        std::string n = ManifestYaml::get(root, "name");
        if (!n.empty()) e.name = n;
        e.version = ManifestYaml::get(root, "version");
        e.summary = ManifestYaml::get(root, "description");
        if (e.summary.empty()) e.summary = ManifestYaml::get(root, "summary");
        e.description = e.summary;
        // Common skill frontmatter keys → extraMeta + tags
        auto addMeta = [&](const char* key) {
            std::string v = ManifestYaml::get(root, key);
            if (!v.empty()) e.extraMeta.push_back(std::string(key) + ": " + v);
        };
        addMeta("license");
        addMeta("compatibility");
        addMeta("metadata");
        addMeta("author");
        addMeta("homepage");
        auto tags = ManifestYaml::getList(root, "tags");
        for (const auto& t : tags) {
            if (std::find(e.tags.begin(), e.tags.end(), t) == e.tags.end()) e.tags.push_back(t);
        }
        // allowed-tools / disable-model-invocation as flags
        std::string allowed = ManifestYaml::get(root, "allowed-tools");
        if (allowed.empty()) allowed = ManifestYaml::get(root, "allowed_tools");
        if (!allowed.empty()) e.extraMeta.push_back("allowed-tools: " + allowed);
        std::string disable = ManifestYaml::get(root, "disable-model-invocation");
        if (disable.empty()) disable = ManifestYaml::get(root, "disable_model_invocation");
        if (!disable.empty()) e.extraMeta.push_back("disable-model-invocation: " + disable);
    }

    if (e.summary.empty() && !body.empty()) {
        // First non-empty, non-heading line of body as summary
        std::istringstream iss(body);
        std::string line;
        while (std::getline(iss, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            size_t a = line.find_first_not_of(" \t");
            if (a == std::string::npos) continue;
            line = line.substr(a);
            if (line.rfind("#", 0) == 0) continue;
            if (line.rfind("---", 0) == 0) continue;
            e.summary = line.size() > 160 ? line.substr(0, 157) + "…" : line;
            break;
        }
        if (e.summary.empty()) e.summary = "skill module";
    }
    if (e.description.empty() && !body.empty()) {
        e.description = body.size() > 480 ? body.substr(0, 477) + "…" : body;
    }
    if (e.summary.empty()) e.summary = "skill module";

    e.builtin = e.relPath.rfind("built-in/", 0) == 0 ||
                e.relPath.rfind("skills/", 0) == 0;
    if (e.relPath.find("/skills/") != std::string::npos ||
        e.relPath.rfind("agents/", 0) == 0)
        e.nested = e.relPath.find("/agents/") != std::string::npos;
    if (std::find(e.tags.begin(), e.tags.end(), e.name) == e.tags.end())
        e.tags.push_back(e.name);
    return e;
}

inline void scanManifestsTree(const fs::path& root, const std::string& source,
                              std::map<std::string, ManifestEntry>& byKey) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;

    // Recursive walk — skip archive/versions noise.
    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        if (it->is_directory(ec)) {
            if (shouldSkipManifestDir(p))
                it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;

        std::string fname = p.filename().string();
        std::string ext = p.extension().string();
        // Skip backups / old drafts
        if (fname.find(".old") != std::string::npos) continue;
        if (fname.find(".worktree") != std::string::npos) continue;

        if (fname == "agent.yml" || fname == "agent.yaml") {
            pushUnique(byKey, readYamlManifestMeta(p, "agent", source, root));
            continue;
        }
        if (fname == "tool.yml" || fname == "tool.yaml") {
            pushUnique(byKey, readYamlManifestMeta(p, "tool", source, root));
            continue;
        }
        if (fname == "feed.yml" || fname == "feed.yaml") {
            pushUnique(byKey, readYamlManifestMeta(p, "feed", source, root));
            continue;
        }
        if (fname == "relic.yml" || fname == "relic.yaml") {
            pushUnique(byKey, readYamlManifestMeta(p, "relic", source, root));
            continue;
        }
        if (fname == "workflow.yml" || fname == "workflow.yaml") {
            pushUnique(byKey, readYamlManifestMeta(p, "workflow", source, root));
            continue;
        }
        // Loose workflow yml under workflows/
        if ((ext == ".yml" || ext == ".yaml") &&
            p.parent_path().filename() == "workflows") {
            // skip pure specs that aren't runnable if name is workflow_spec — still list
            pushUnique(byKey, readYamlManifestMeta(p, "workflow", source, root));
            continue;
        }
        if (fname == "SKILL.md") {
            pushUnique(byKey, readSkillManifestMeta(p, source, root));
            continue;
        }
        // harness + prompts as first-class registry entries (md modules)
        if (ext == ".md" && p.parent_path().filename() == "harness" &&
            fname.find("README") == std::string::npos) {
            ManifestEntry e;
            e.kind = "harness";
            e.category = "harness";
            e.name = p.stem().string();
            e.path = absPath(p);
            e.source = source;
            e.manifestsRoot = absPath(root);
            e.relPath = relToRoot(p, root);
            e.summary = "harness profile";
            e.tags = {"harness", "profile", e.name};
            pushUnique(byKey, std::move(e));
            continue;
        }
        if (ext == ".md" && p.parent_path().filename() == "prompts" &&
            fname.find("README") == std::string::npos) {
            ManifestEntry e;
            e.kind = "prompt";
            e.category = "prompt";
            e.name = p.stem().string();
            e.path = absPath(p);
            e.source = source;
            e.manifestsRoot = absPath(root);
            e.relPath = relToRoot(p, root);
            e.summary = "prompt module";
            e.tags = {"prompt", "module", e.name};
            pushUnique(byKey, std::move(e));
            continue;
        }
    }
}

inline std::vector<ManifestEntry> discoverManifests(const std::string& manifestDirOverride = "") {
    std::map<std::string, ManifestEntry> byKey;
    for (const auto& [mroot, source] : manifestsSearchRoots(manifestDirOverride))
        scanManifestsTree(mroot, source, byKey);

    std::vector<ManifestEntry> out;
    out.reserve(byKey.size());
    for (auto& kv : byKey)
        out.push_back(std::move(kv.second));

    // Kind order then name — hub scannability.
    auto kindRank = [](const std::string& k) -> int {
        if (k == "agent") return 0;
        if (k == "workflow") return 1;
        if (k == "tool") return 2;
        if (k == "feed") return 3;
        if (k == "relic") return 4;
        if (k == "harness") return 5;
        if (k == "skill") return 6;
        if (k == "prompt") return 7;
        return 9;
    };
    std::sort(out.begin(), out.end(), [&](const ManifestEntry& a, const ManifestEntry& b) {
        int ra = kindRank(a.kind), rb = kindRank(b.kind);
        if (ra != rb) return ra < rb;
        if (a.name != b.name) return a.name < b.name;
        return a.relPath < b.relPath;
    });
    return out;
}

// Keep agent discovery, but recursive (nested sub-agents under agents/**).
inline std::vector<AgentEntry> discoverAgentsRecursive(const std::string& manifestDirOverride = "") {
    std::map<std::string, AgentEntry> byName;
    for (const auto& [mroot, source] : manifestsSearchRoots(manifestDirOverride)) {
        fs::path agentsRoot = fs::path(mroot) / "agents";
        std::error_code ec;
        if (!fs::is_directory(agentsRoot, ec)) continue;
        fs::recursive_directory_iterator it(
            agentsRoot, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec) && shouldSkipManifestDir(it->path())) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            auto fname = it->path().filename().string();
            if (fname != "agent.yml" && fname != "agent.yaml") continue;
            auto e = readAgentMeta(it->path(), source, agentsRoot.string());
            if (e.name.empty()) continue;
            // Nested agents: prefer unique key by rel path if name collides.
            std::string key = e.name;
            if (byName.count(key)) {
                // Disambiguate nested (e.g. reader under coder)
                key = e.name + "@" + relToRoot(it->path().parent_path(), agentsRoot);
                e.name = key;  // show path-qualified name in flat agent lists
            }
            if (!byName.count(key))
                byName[key] = std::move(e);
        }
    }
    std::vector<AgentEntry> out;
    out.reserve(byName.size());
    for (auto& kv : byName) out.push_back(std::move(kv.second));
    std::sort(out.begin(), out.end(),
              [](const AgentEntry& a, const AgentEntry& b) { return a.name < b.name; });
    return out;
}

inline std::string resolveHarnessPath(const std::string& nameOrPath,
                                      const std::string& manifestDirOverride = "",
                                      std::string* err = nullptr) {
    if (nameOrPath.empty())
        return {};

    static const std::set<std::string> sizes = {"small", "medium", "big", "default"};
    std::string key = nameOrPath;
    // strip optional .md
    if (key.size() > 3 && key.compare(key.size() - 3, 3, ".md") == 0)
        key = key.substr(0, key.size() - 3);
    // strip optional harness/ prefix for bare sizes
    if (key.rfind("harness/", 0) == 0)
        key = key.substr(8);

    if (sizes.count(key)) {
        std::string found = findShared("harness/" + key + ".md", manifestDirOverride);
        if (found.empty())
            found = findShared("manifests/harness/" + key + ".md", manifestDirOverride);
        if (!found.empty())
            return found;
        if (err)
            *err = "harness size '" + key + "' not found under manifests/harness/";
        return {};
    }

    // Path form
    fs::path p = expandHome(nameOrPath);
    std::error_code ec;
    if (fs::is_regular_file(p, ec))
        return absPath(p);
    // Try relative to manifests roots
    std::string found = findShared(nameOrPath, manifestDirOverride);
    if (!found.empty())
        return found;
    if (err)
        *err = "harness not found: " + nameOrPath;
    return {};
}

}  // namespace catalog
}  // namespace mk3
}  // namespace cortex
