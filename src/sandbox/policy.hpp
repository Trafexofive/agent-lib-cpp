#pragma once
// =============================================================================
// agent-lib-MK3 — Sandbox Policy
// Runtime tool filtering. When --sandbox is active, all tool calls pass
// through this policy. Blocks destructive operations, restricts filesystem
// access to the workspace, and prevents network calls outside the provider.
// =============================================================================

#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <iostream>

namespace cortex::mk3::sandbox {

namespace fs = std::filesystem;

// ── Policy configuration ─────────────────────────────────────────────

struct SandboxPolicy {
    bool enabled = false;
    bool readOnly = false;          // No fs_write at all
    std::string workspace;          // Allowed root for reads/writes
    std::string allowedApiHost;     // Only this host for web_fetch
    std::vector<std::string> allowedCommands;  // Whitelist for exec (empty = block all)

    // ── Validate a tool call before execution ────────────────────────
    // Returns "" if allowed, or an error message string if blocked.
    std::string validate(const std::string& toolName,
                         const std::string& paramsJson) const
    {
        if (!enabled) return "";

        // Block exec unless whitelisted
        if (toolName == "exec") {
            if (allowedCommands.empty()) {
                return "sandbox: exec blocked (no commands whitelisted)";
            }
            // Check if command matches any whitelist prefix
            // (Simple check — we extract the command name from params)
            auto cmdPos = paramsJson.find("\"command\":");
            if (cmdPos == std::string::npos) {
                cmdPos = paramsJson.find("\"cmd\":");
            }
            if (cmdPos != std::string::npos) {
                auto start = paramsJson.find('"', cmdPos + 10) + 1;
                auto end = paramsJson.find('"', start);
                std::string cmd = paramsJson.substr(start, end - start);
                // Extract just the program name (before first space)
                auto space = cmd.find(' ');
                std::string prog = (space != std::string::npos) ? cmd.substr(0, space) : cmd;
                bool allowed = false;
                for (auto& a : allowedCommands) {
                    if (prog == a || cmd.rfind(a, 0) == 0) { allowed = true; break; }
                }
                if (!allowed) {
                    return "sandbox: exec '" + prog + "' not in whitelist";
                }
            }
        }

        // Block/redirect fs_write
        if (toolName == "fs_write") {
            if (readOnly) {
                return "sandbox: fs_write blocked (read-only mode)";
            }
            // Validate path is within workspace
            auto path = extractPath(paramsJson);
            if (!path.empty() && !isWithinWorkspace(path)) {
                return "sandbox: fs_write path '" + path + "' outside workspace";
            }
        }

        // Restrict fs_read to workspace
        if (toolName == "fs_read") {
            auto path = extractPath(paramsJson);
            if (!path.empty() && !isWithinWorkspace(path)) {
                return "sandbox: fs_read path '" + path + "' outside workspace";
            }
        }

        // SB07 — context_pin/peek/unpin read arbitrary files, must be sandboxed.
        // unpin doesn't touch the disk so we only validate pin and peek paths.
        if (toolName == "context_pin" || toolName == "context_peek") {
            auto path = extractPath(paramsJson);
            if (!path.empty() && !isWithinWorkspace(path)) {
                return "sandbox: " + toolName + " path '" + path +
                       "' outside workspace";
            }
        }

        // Restrict web_fetch to allowed API host only
        if (toolName == "web_fetch") {
            auto urlPos = paramsJson.find("\"url\":");
            if (urlPos == std::string::npos) {
                urlPos = paramsJson.find("\"endpoint\":");
            }
            if (urlPos != std::string::npos) {
                auto start = paramsJson.find('"', urlPos + 6) + 1;
                auto end = paramsJson.find('"', start);
                std::string url = paramsJson.substr(start, end - start);
                if (!allowedApiHost.empty() &&
                    url.find(allowedApiHost) == std::string::npos) {
                    return "sandbox: web_fetch to '" + url + "' blocked (only " +
                           allowedApiHost + " allowed)";
                }
            }
        }

        return "";  // Allowed
    }

    // ── Rewrite a tool call's path to be workspace-relative ──────────
    std::string rewritePath(const std::string& toolName,
                            const std::string& paramsJson) const
    {
        if (!enabled || workspace.empty()) return paramsJson;

        if (toolName == "fs_read" || toolName == "fs_write" ||
            toolName == "context_pin" || toolName == "context_peek" ||
            toolName == "context_unpin") {
            auto path = extractPath(paramsJson);
            if (!path.empty() && path[0] != '/') {
                // Relative paths are resolved against workspace
                // Already OK — they'll be relative to CWD which is workspace
                return paramsJson;
            }
        }

        return paramsJson;
    }

private:
    std::string extractPath(const std::string& json) const {
        auto pos = json.find("\"path\":");
        if (pos == std::string::npos) {
            pos = json.find("\"file\":");
        }
        if (pos != std::string::npos) {
            auto start = json.find('"', pos + 7) + 1;
            auto end = json.find('"', start);
            return json.substr(start, end - start);
        }
        return "";
    }

    bool isWithinWorkspace(const std::string& path) const {
        if (workspace.empty()) return true;
        // Relative paths are trusted to the OS-level sandbox (Docker sets
        // CWD = workspace inside the container). This keeps dev-mode ergonomics
        // intact while still defending against absolute-path escapes.
        if (path.empty() || path[0] != '/') return true;

        // SB02 — canonicalise the absolute path before prefix-comparing so
        // `workspace/../outside` is correctly rejected. lexically_normal handles
        // `..` resolution without requiring the path to exist on disk.
        std::error_code ec;
        fs::path candidate = fs::path(path).lexically_normal();
        fs::path root = fs::path(workspace);
        if (root.is_relative()) root = fs::current_path(ec) / root;
        root = root.lexically_normal();

        // Compare component-wise so `/a/b` isn't treated as a prefix of `/a/bb`.
        auto rIt = root.begin(), rEnd = root.end();
        auto cIt = candidate.begin(), cEnd = candidate.end();
        for (; rIt != rEnd; ++rIt, ++cIt) {
            if (cIt == cEnd) return false;
            if (*cIt != *rIt) return false;
        }
        return true;
    }
};

// ── Preset sandbox configs ───────────────────────────────────────────

inline SandboxPolicy makeHarnessSandbox(const std::string& workspacePath) {
    SandboxPolicy p;
    p.enabled = true;
    p.readOnly = false;  // Allow writes for test artifacts
    p.workspace = workspacePath;
    p.allowedApiHost = "";  // Allow provider API (not enforced at tool level)
    p.allowedCommands = {
        "ls", "cat", "head", "tail", "grep", "find", "wc",
        "echo", "date", "whoami", "pwd", "uname", "env",
        "mkdir", "touch", "cp", "mv", "rm",  // Allowed in workspace
        "make", "g++", "gcc", "python3", "node",
    };
    return p;
}

inline SandboxPolicy makeReadOnlySandbox(const std::string& workspacePath) {
    SandboxPolicy p = makeHarnessSandbox(workspacePath);
    p.readOnly = true;
    // Remove write-capable commands
    p.allowedCommands = {
        "ls", "cat", "head", "tail", "grep", "find", "wc",
        "echo", "date", "whoami", "pwd", "uname", "env",
    };
    return p;
}

} // namespace cortex::mk3::sandbox
