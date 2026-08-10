#pragma once
// =============================================================================
// agent-lib-MK3 — Sandbox Policy
// Runtime tool filtering driven by agent.yml `sandbox:` (and CLI --sandbox).
// Gates exec / fs / network tools, enforces multi-root allowed paths, per-bind
// read-only, and guest→host path rewrite for process-mode binds.
// =============================================================================

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "../core/types.hpp"

namespace cortex::mk3::sandbox {

namespace fs = std::filesystem;

// ── Policy configuration ─────────────────────────────────────────────

struct SandboxPolicy {
    bool enabled = false;
    bool readOnly = false;  // global: no fs_write at all
    std::string workspace;  // primary allowed root (CWD / container workdir)
    std::string network = "out";  // none | out | full (informational for process)

    std::vector<std::string> allowedCommands;  // empty + enabled → exec blocked; "*" → all
    std::vector<std::string> allowedPaths;     // extra roots beyond workspace + binds
    std::vector<std::string> allowedHosts;  // empty + enabled → web_fetch blocked; "*" → all
    std::vector<SandboxBind> binds;         // live mounts; guest rewritten → host

    // Legacy single-host field kept for callers of makeHarnessSandbox.
    std::string allowedApiHost;

    // ── Validate a tool call before execution ────────────────────────
    // Returns "" if allowed, or an error message string if blocked.
    std::string validate(const std::string& toolName, const std::string& paramsJson) const {
        if (!enabled)
            return "";

        if (toolName == "exec") {
            if (allowedCommands.empty()) {
                return "sandbox: exec blocked (no commands whitelisted)";
            }
            if (!commandAllowedAll()) {
                std::string cmd = extractCommand(paramsJson);
                if (!cmd.empty()) {
                    std::string prog = programName(cmd);
                    if (!isCommandAllowed(prog, cmd)) {
                        return "sandbox: exec '" + prog + "' not in whitelist";
                    }
                }
            }
        }

        if (toolName == "fs_write") {
            auto path = extractPath(paramsJson);
            if (readOnly) {
                return "sandbox: fs_write blocked (read-only mode)";
            }
            if (!path.empty()) {
                if (isUnderReadOnlyBind(path)) {
                    return "sandbox: fs_write path '" + path + "' is on a read-only bind";
                }
                if (!isAllowedPath(path)) {
                    return "sandbox: fs_write path '" + path + "' outside workspace";
                }
            }
        }

        if (toolName == "fs_read" || toolName == "list" || toolName == "grep") {
            auto path = extractPath(paramsJson);
            // grep/list default path is "."; empty extract is fine.
            if (!path.empty() && !isAllowedPath(path)) {
                return "sandbox: " + toolName + " path '" + path + "' outside workspace";
            }
        }

        // SB07 — context_pin/peek read arbitrary files, must be sandboxed.
        if (toolName == "context_pin" || toolName == "context_peek") {
            auto path = extractPath(paramsJson);
            if (!path.empty() && !isAllowedPath(path)) {
                return "sandbox: " + toolName + " path '" + path + "' outside workspace";
            }
        }

        if (toolName == "web_fetch") {
            std::string url = jsonStringField(paramsJson, "url");
            if (url.empty())
                url = jsonStringField(paramsJson, "endpoint");
            if (!url.empty() && !isHostAllowed(url)) {
                return "sandbox: web_fetch to '" + url + "' blocked (host not in allowed_hosts)";
            }
        }

        // DeepSearchStack tools stay local-stack by default.
        if (toolName.rfind("dss_", 0) == 0) {
            std::string baseUrl = jsonStringField(paramsJson, "base_url");
            if (!baseUrl.empty() && !isLocalhostUrl(baseUrl) && !isHostAllowed(baseUrl)) {
                return "sandbox: dss tool base_url '" + baseUrl +
                       "' blocked; use localhost or allow the host";
            }
            if (toolName == "dss_ingest" && !jsonBoolField(paramsJson, "allow_mutation")) {
                return "sandbox: dss_ingest is mutating; pass allow_mutation=true or disable "
                       "sandbox";
            }
        }

        return "";
    }

    // Rewrite guest bind paths → host paths in tool params JSON (process mode).
    // Docker/chroot already present the guest path natively via mounts.
    std::string rewritePath(const std::string& toolName, const std::string& paramsJson) const {
        if (!enabled || binds.empty())
            return paramsJson;

        if (toolName != "fs_read" && toolName != "fs_write" && toolName != "list" &&
            toolName != "grep" && toolName != "context_pin" && toolName != "context_peek" &&
            toolName != "context_unpin") {
            return paramsJson;
        }

        auto path = extractPath(paramsJson);
        if (path.empty())
            return paramsJson;

        std::string rewritten = resolveToHost(path);
        if (rewritten == path)
            return paramsJson;

        return replaceJsonStringField(paramsJson, pathFieldKey(paramsJson), rewritten);
    }

    // Map a guest (or relative) path to the host path it should touch.
    std::string resolveToHost(const std::string& path) const {
        if (path.empty())
            return path;

        fs::path cand = fs::path(path).lexically_normal();
        std::string bestGuest;
        std::string bestHost;
        for (const auto& b : binds) {
            if (b.guest.empty() || b.host.empty())
                continue;
            fs::path g = fs::path(b.guest).lexically_normal();
            if (!pathEqualsOrUnder(cand, g))
                continue;
            // Longest guest prefix wins.
            if (g.string().size() >= bestGuest.size()) {
                bestGuest = g.string();
                bestHost = b.host;
            }
        }
        if (bestGuest.empty())
            return path;

        fs::path g(bestGuest);
        fs::path h(bestHost);
        auto gIt = g.begin(), gEnd = g.end();
        auto cIt = cand.begin(), cEnd = cand.end();
        for (; gIt != gEnd && cIt != cEnd; ++gIt, ++cIt) {
            if (*gIt != *cIt)
                return path;
        }
        fs::path rest;
        for (; cIt != cEnd; ++cIt)
            rest /= *cIt;
        return (h / rest).lexically_normal().string();
    }

   private:
    bool commandAllowedAll() const {
        for (const auto& a : allowedCommands) {
            if (a == "*")
                return true;
        }
        return false;
    }

    bool hostAllowedAll() const {
        for (const auto& h : allowedHosts) {
            if (h == "*")
                return true;
        }
        if (!allowedApiHost.empty() && allowedApiHost == "*")
            return true;
        return false;
    }

    bool isCommandAllowed(const std::string& prog, const std::string& fullCmd) const {
        for (const auto& a : allowedCommands) {
            if (a == "*")
                return true;
            if (prog == a || fullCmd.rfind(a, 0) == 0)
                return true;
            // basename match for absolute whitelist entries
            fs::path ap(a);
            if (!ap.filename().empty() && ap.filename().string() == prog)
                return true;
        }
        return false;
    }

    bool isHostAllowed(const std::string& url) const {
        if (hostAllowedAll())
            return true;

        // Legacy single host
        if (!allowedApiHost.empty() && url.find(allowedApiHost) != std::string::npos)
            return true;

        if (allowedHosts.empty())
            return false;

        std::string host = extractUrlHost(url);
        if (host.empty())
            return false;
        // lowercase
        std::string hostLower = host;
        std::transform(hostLower.begin(), hostLower.end(), hostLower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        for (auto h : allowedHosts) {
            std::transform(h.begin(), h.end(), h.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (h == "*" || hostLower == h)
                return true;
            // suffix match: allowed "example.com" matches "api.example.com"
            if (hostLower.size() > h.size() && hostLower[hostLower.size() - h.size() - 1] == '.' &&
                hostLower.compare(hostLower.size() - h.size(), h.size(), h) == 0)
                return true;
        }
        return false;
    }

    bool isUnderReadOnlyBind(const std::string& path) const {
        fs::path cand = fs::path(path).lexically_normal();
        // Also check resolved host form
        fs::path hostForm = fs::path(resolveToHost(path)).lexically_normal();

        for (const auto& b : binds) {
            if (!b.readOnly)
                continue;
            if (!b.guest.empty() && pathEqualsOrUnder(cand, fs::path(b.guest).lexically_normal()))
                return true;
            if (!b.host.empty() &&
                pathEqualsOrUnder(hostForm, fs::path(b.host).lexically_normal()))
                return true;
            if (!b.host.empty() && pathEqualsOrUnder(cand, fs::path(b.host).lexically_normal()))
                return true;
        }
        return false;
    }

    bool isAllowedPath(const std::string& path) const {
        if (path.empty())
            return true;

        // Relative paths stay within CWD/workspace by convention.
        if (path[0] != '/')
            return true;

        fs::path cand = fs::path(path).lexically_normal();
        fs::path hostForm = fs::path(resolveToHost(path)).lexically_normal();

        auto roots = collectRoots();
        for (const auto& rootStr : roots) {
            if (rootStr.empty())
                continue;
            fs::path root = fs::path(rootStr).lexically_normal();
            if (pathEqualsOrUnder(cand, root) || pathEqualsOrUnder(hostForm, root))
                return true;
        }
        return false;
    }

    std::vector<std::string> collectRoots() const {
        std::vector<std::string> roots;
        auto push = [&](const std::string& p) {
            if (p.empty())
                return;
            std::error_code ec;
            fs::path r = fs::path(p);
            if (r.is_relative())
                r = fs::current_path(ec) / r;
            roots.push_back(r.lexically_normal().string());
        };

        push(workspace);
        for (const auto& p : allowedPaths)
            push(p);
        for (const auto& b : binds) {
            push(b.host);
            // Absolute guest roots (container paths) count when tools use them pre-rewrite.
            if (!b.guest.empty() && b.guest[0] == '/')
                push(b.guest);
        }
        return roots;
    }

    static bool pathEqualsOrUnder(const fs::path& cand, const fs::path& root) {
        if (root.empty())
            return false;
        auto rIt = root.begin(), rEnd = root.end();
        auto cIt = cand.begin(), cEnd = cand.end();
        // Skip empty/"." components that lexically_normal can leave on some roots.
        auto skipNoise = [](fs::path::iterator& it, fs::path::iterator end) {
            while (it != end && (it->empty() || *it == "."))
                ++it;
        };
        skipNoise(rIt, rEnd);
        skipNoise(cIt, cEnd);
        for (; rIt != rEnd; ++rIt, ++cIt) {
            skipNoise(rIt, rEnd);
            skipNoise(cIt, cEnd);
            if (rIt == rEnd)
                break;
            if (cIt == cEnd)
                return false;
            if (*cIt != *rIt)
                return false;
        }
        return true;
    }

    static std::string jsonStringField(const std::string& json, const std::string& field) {
        std::string needle = "\"" + field + "\":";
        auto pos = json.find(needle);
        if (pos == std::string::npos)
            return "";
        auto start = json.find('"', pos + needle.size());
        if (start == std::string::npos)
            return "";
        ++start;
        std::string out;
        bool escape = false;
        for (size_t i = start; i < json.size(); ++i) {
            char c = json[i];
            if (escape) {
                out.push_back(c);
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                break;
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    static bool jsonBoolField(const std::string& json, const std::string& field) {
        std::string needle = "\"" + field + "\":";
        auto pos = json.find(needle);
        if (pos == std::string::npos)
            return false;
        size_t i = pos + needle.size();
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t'))
            ++i;
        return json.compare(i, 4, "true") == 0;
    }

    static bool isLocalhostUrl(const std::string& url) {
        return url.find("http://localhost") == 0 || url.find("http://127.0.0.1") == 0 ||
               url.find("https://localhost") == 0 || url.find("https://127.0.0.1") == 0 ||
               url.find("http://[::1]") == 0 || url.find("https://[::1]") == 0;
    }

    static std::string extractUrlHost(const std::string& url) {
        // scheme://host[:port]/path]
        auto scheme = url.find("://");
        if (scheme == std::string::npos)
            return "";
        size_t start = scheme + 3;
        if (start >= url.size())
            return "";
        if (url[start] == '[') {
            auto end = url.find(']', start);
            if (end == std::string::npos)
                return "";
            return url.substr(start + 1, end - start - 1);
        }
        size_t end = start;
        while (end < url.size() && url[end] != '/' && url[end] != ':' && url[end] != '?' &&
               url[end] != '#')
            ++end;
        return url.substr(start, end - start);
    }

    static std::string extractPath(const std::string& json) {
        auto path = jsonStringField(json, "path");
        if (!path.empty())
            return path;
        return jsonStringField(json, "file");
    }

    static std::string pathFieldKey(const std::string& json) {
        if (json.find("\"path\":") != std::string::npos)
            return "path";
        if (json.find("\"file\":") != std::string::npos)
            return "file";
        return "path";
    }

    static std::string extractCommand(const std::string& json) {
        auto cmd = jsonStringField(json, "command");
        if (!cmd.empty())
            return cmd;
        return jsonStringField(json, "cmd");
    }

    static std::string programName(const std::string& cmd) {
        // Skip env assignments: FOO=bar cmd
        std::string s = cmd;
        while (!s.empty()) {
            auto sp = s.find(' ');
            std::string tok = sp == std::string::npos ? s : s.substr(0, sp);
            if (tok.find('=') != std::string::npos && tok[0] != '/') {
                if (sp == std::string::npos)
                    return tok;
                s = s.substr(sp + 1);
                while (!s.empty() && s[0] == ' ')
                    s.erase(s.begin());
                continue;
            }
            // basename if path
            auto slash = tok.find_last_of('/');
            if (slash != std::string::npos)
                tok = tok.substr(slash + 1);
            return tok;
        }
        return cmd;
    }

    static std::string replaceJsonStringField(const std::string& json, const std::string& field,
                                              const std::string& newVal) {
        std::string needle = "\"" + field + "\":";
        auto pos = json.find(needle);
        if (pos == std::string::npos)
            return json;
        auto start = json.find('"', pos + needle.size());
        if (start == std::string::npos)
            return json;
        size_t end = start + 1;
        bool escape = false;
        for (; end < json.size(); ++end) {
            char c = json[end];
            if (escape) {
                escape = false;
                continue;
            }
            if (c == '\\') {
                escape = true;
                continue;
            }
            if (c == '"')
                break;
        }
        if (end >= json.size())
            return json;

        // Escape newVal for JSON
        std::string esc;
        for (char c : newVal) {
            if (c == '\\' || c == '"')
                esc.push_back('\\');
            esc.push_back(c);
        }
        return json.substr(0, start + 1) + esc + json.substr(end);
    }
};

// ── Build policy from AgentConfig (manifest-driven) ──────────────────

inline SandboxPolicy makePolicyFromConfig(const AgentConfig& cfg, const std::string& workspacePath) {
    SandboxPolicy p;
    if (!cfg.sandboxConfigured && cfg.sandboxMode == "process" && cfg.sandboxBinds.empty() &&
        cfg.sandboxFiles.empty()) {
        return p;  // disabled
    }

    p.enabled = true;
    p.readOnly = cfg.sandboxReadonly;
    p.workspace = workspacePath;
    p.network = cfg.sandboxNetwork.empty() ? "out" : cfg.sandboxNetwork;
    p.binds = cfg.sandboxBinds;

    if (cfg.sandboxCommandsSet) {
        p.allowedCommands = cfg.sandboxAllowedCommands;
    } else if (cfg.sandboxMode == "docker" || cfg.sandboxMode == "chroot") {
        // Isolated runtimes still need a usable default shell surface unless
        // the manifest opted into an empty whitelist.
        p.allowedCommands = {"ls",   "cat",  "head", "tail", "grep", "find", "wc",
                             "echo", "date", "pwd",  "env",  "mkdir", "cp",  "mv",
                             "rm",   "make", "g++",  "gcc",  "python3", "node", "bash"};
    } else {
        // process + sandbox: present without allowed_commands → block exec
        // (most restrictive design default).
        p.allowedCommands.clear();
    }

    if (cfg.sandboxPathsSet) {
        p.allowedPaths = cfg.sandboxAllowedPaths;
    }
    // Always allow bind hosts as path roots (already in policy via binds).

    if (cfg.sandboxHostsSet) {
        p.allowedHosts = cfg.sandboxAllowedHosts;
    } else {
        p.allowedHosts.clear();  // block web_fetch by default when sandbox configured
    }

    return p;
}

// ── Preset sandbox configs (CLI --sandbox / --sandbox-ro) ────────────

inline SandboxPolicy makeHarnessSandbox(const std::string& workspacePath) {
    SandboxPolicy p;
    p.enabled = true;
    p.readOnly = false;
    p.workspace = workspacePath;
    p.allowedHosts = {"*"};  // CLI harness preset is permissive on network
    p.allowedCommands = {
        "ls",   "cat",   "head", "tail",    "grep",  "find", "wc", "echo", "date", "whoami",
        "pwd",  "uname", "env",  "mkdir",   "touch", "cp",   "mv", "rm",
        "make", "g++",   "gcc",  "python3", "node",
    };
    return p;
}

inline SandboxPolicy makeReadOnlySandbox(const std::string& workspacePath) {
    SandboxPolicy p = makeHarnessSandbox(workspacePath);
    p.readOnly = true;
    p.allowedCommands = {
        "ls",   "cat",  "head",   "tail", "grep",  "find", "wc",
        "echo", "date", "whoami", "pwd",  "uname", "env",
    };
    return p;
}

// Merge CLI preset over manifest policy: CLI forces enable + optional RO.
inline SandboxPolicy mergeCliSandbox(SandboxPolicy base, bool cliSandbox, bool cliReadOnly,
                                     const std::string& workspacePath) {
    if (!cliSandbox)
        return base;
    if (!base.enabled) {
        base = cliReadOnly ? makeReadOnlySandbox(workspacePath) : makeHarnessSandbox(workspacePath);
        return base;
    }
    base.enabled = true;
    if (cliReadOnly)
        base.readOnly = true;
    if (base.workspace.empty())
        base.workspace = workspacePath;
    return base;
}

}  // namespace cortex::mk3::sandbox
