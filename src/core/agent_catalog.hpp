#pragma once
// =============================================================================
// agent-lib-MK3 — Global agent / manifest catalog
// Global surface is manifests/ only (agents live under manifests/agents/).
// =============================================================================

#include <algorithm>
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

// Only manifests/ trees are global. Roots returned are .../manifests directories.
inline std::vector<std::pair<std::string, std::string>> manifestsSearchRoots(
    const std::string& manifestDirOverride = "") {
    std::vector<std::pair<std::string, std::string>> roots;  // {manifestsPath, source}
    std::set<std::string> seen;

    auto addManifests = [&](const std::string& raw, const std::string& source) {
        if (raw.empty())
            return;
        std::string expanded = expandHome(raw);
        std::error_code ec;
        if (!fs::exists(expanded, ec))
            return;
        fs::path p(expanded);
        // Accept either .../manifests or a parent that contains manifests/
        fs::path m = p;
        if (p.filename() != "manifests") {
            if (fs::is_directory(p / "manifests", ec))
                m = p / "manifests";
            else if (p.filename() == "agents" && p.parent_path().filename() == "manifests")
                m = p.parent_path();
            else
                return;  // not a manifests tree
        }
        if (!fs::is_directory(m, ec))
            return;
        std::string key = absPath(m);
        if (!seen.insert(key).second)
            return;
        roots.push_back({key, source});
    };

    // 1. CORTEX_HOME → $CORTEX_HOME/manifests
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

    // 3. --manifest-dir: must resolve to a manifests tree (or parent of one)
    if (!manifestDirOverride.empty())
        addManifests(manifestDirOverride, "override");

    // 4. CWD
    addManifests("./manifests", "cwd");

    // 5. Walk up from CWD for repo layouts
    {
        std::error_code ec;
        fs::path cur = fs::current_path(ec);
        for (int i = 0; i < 8 && !ec; ++i) {
            addManifests((cur / "manifests").string(), "project");
            if (!cur.has_parent_path() || cur == cur.root_path())
                break;
            cur = cur.parent_path();
        }
    }

    // 6. Binary-adjacent (dev binary in repo root, or PREFIX/share/cortex/manifests)
    {
        char buf[PATH_MAX];
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            fs::path exe = fs::path(buf).parent_path();
            addManifests((exe / "manifests").string(), "binary");
            addManifests((exe / ".." / "share" / "cortex" / "manifests").string(), "binary");
            addManifests((exe / ".." / "share" / "cortex").string(), "binary");
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
        if (k == "harness" || k == "system" || k == "persona")
            return "context";
        if (k == "env")
            return "env";
        return k;
    };

    // Merge context kinds into one group for display order
    std::vector<std::pair<std::string, std::vector<const OwnedItem*>>> sections;
    {
        std::vector<const OwnedItem*> ctx;
        for (const char* k : {"harness", "system", "persona"}) {
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
