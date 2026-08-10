// sandbox_policy_test.cpp — validates SandboxPolicy without LLM calls
#include "../src/sandbox/policy.hpp"

#include <cassert>
#include <iostream>

using namespace cortex::mk3;
using namespace cortex::mk3::sandbox;

int main() {
    int failures = 0;
    auto check = [&](bool cond, const char* test) {
        if (!cond) {
            std::cerr << "FAIL: " << test << "\n";
            failures++;
        } else {
            std::cout << "  OK: " << test << "\n";
        }
    };

    std::cout << "═══ SandboxPolicy Tests ═══\n\n";

    // ── Test 1: Disabled policy allows everything ──
    {
        SandboxPolicy p;
        check(p.validate("exec", R"({"command":"rm -rf /"})").empty(),
              "disabled policy allows exec");
        check(p.validate("fs_write", R"({"path":"/etc/passwd"})").empty(),
              "disabled policy allows fs_write");
        check(p.validate("web_fetch", R"({"url":"https://evil.com"})").empty(),
              "disabled policy allows web_fetch");
    }

    // ── Test 2: Harness sandbox — exec whitelist ──
    {
        SandboxPolicy p = makeHarnessSandbox("/workspace");
        check(p.validate("exec", R"({"command":"rm -rf /"})").empty(),
              "harness sandbox allows 'rm'");
        check(p.validate("exec", R"({"command":"ls -la"})").empty(), "harness sandbox allows 'ls'");
        check(p.validate("exec", R"({"command":"grep foo *.cpp"})").empty(),
              "harness sandbox allows 'grep'");
        check(p.validate("exec", R"({"command":"cat /etc/passwd"})").empty(),
              "harness sandbox allows 'cat'");
        check(p.validate("exec", R"({"command":"make"})").empty(), "harness sandbox allows 'make'");
        check(!p.validate("exec", R"({"command":"nmap localhost"})").empty(),
              "harness sandbox blocks unknown cmd");
    }

    // ── Test 3: Harness sandbox — fs_write path restriction ──
    {
        SandboxPolicy p = makeHarnessSandbox("/workspace");
        check(!p.validate("fs_write", R"({"path":"/etc/hosts"})").empty(),
              "harness sandbox blocks fs_write to /etc/hosts");
        check(!p.validate("fs_write", R"({"path":"/tmp/outside"})").empty(),
              "harness sandbox blocks fs_write outside workspace");
        check(p.validate("fs_write", R"({"path":"/workspace/output.txt"})").empty(),
              "harness sandbox allows fs_write in workspace");
        check(p.validate("fs_write", R"({"path":"output.txt"})").empty(),
              "harness sandbox allows fs_write relative path");
    }

    // ── Test 4: Read-only sandbox — no fs_write at all ──
    {
        SandboxPolicy p = makeReadOnlySandbox("/workspace");
        check(!p.validate("fs_write", R"({"path":"/workspace/safe.txt"})").empty(),
              "RO sandbox blocks fs_write even in workspace");
        check(!p.validate("exec", R"({"command":"dd if=/dev/zero of=/dev/sda"})").empty(),
              "RO sandbox blocks dd (not in whitelist)");
        check(p.validate("exec", R"({"command":"ls"})").empty(),
              "RO sandbox allows read-only exec");
        check(p.validate("fs_read", R"({"path":"/workspace/src/main.cpp"})").empty(),
              "RO sandbox allows fs_read in workspace");
        check(!p.validate("fs_read", R"({"path":"/etc/shadow"})").empty(),
              "RO sandbox blocks fs_read outside workspace");
    }

    // ── Test 5: Read-only sandbox — restricted fs_read ──
    {
        SandboxPolicy p = makeReadOnlySandbox("/workspace");
        check(p.validate("fs_read", R"({"path":"src/main.cpp"})").empty(),
              "RO sandbox allows relative fs_read");
        check(p.validate("grep", R"({"pattern":"TODO"})").empty(),
              "RO sandbox allows grep (no path → ok)");
        check(p.validate("list", R"({"path":"."})").empty(), "RO sandbox allows list");
        check(p.validate("context_pin", R"({"path":"src/main.cpp"})").empty(),
              "RO sandbox allows context_pin (relative)");
    }

    // ── Test 6: Manifest-style multi-host / multi-path / wildcard ──
    {
        SandboxPolicy p;
        p.enabled = true;
        p.workspace = "/workspace";
        p.allowedCommands = {"python3", "bash", "*"};
        p.allowedPaths = {"/data/shared"};
        p.allowedHosts = {"api.github.com", "example.com"};

        check(p.validate("exec", R"({"command":"anything"})").empty(),
              "wildcard allowed_commands permits any exec");
        check(p.validate("fs_read", R"({"path":"/data/shared/x"})").empty(),
              "allowed_paths permits extra root");
        check(!p.validate("fs_read", R"({"path":"/etc/passwd"})").empty(),
              "extra root does not open entire fs");
        check(p.validate("web_fetch", R"({"url":"https://api.github.com/repos"})").empty(),
              "allowed host passes");
        check(p.validate("web_fetch", R"({"url":"https://foo.example.com/x"})").empty(),
              "suffix host match passes");
        check(!p.validate("web_fetch", R"({"url":"https://evil.com"})").empty(),
              "disallowed host blocked");
    }

    // ── Test 7: empty allowed_hosts blocks web_fetch ──
    {
        SandboxPolicy p;
        p.enabled = true;
        p.workspace = "/workspace";
        p.allowedHosts = {};
        check(!p.validate("web_fetch", R"({"url":"https://example.com"})").empty(),
              "empty allowed_hosts blocks web_fetch");
    }

    // ── Test 8: bind RO blocks write; rewrite guest→host ──
    {
        SandboxPolicy p;
        p.enabled = true;
        p.workspace = "/workspace";
        p.allowedCommands = {"cat"};
        SandboxBind b;
        b.host = "/home/op/ctx";
        b.guest = "/home/ctx";
        b.readOnly = true;
        p.binds.push_back(b);

        check(p.validate("fs_read", R"({"path":"/home/ctx/a.txt"})").empty(),
              "bind guest readable");
        check(!p.validate("fs_write", R"({"path":"/home/ctx/a.txt"})").empty(),
              "RO bind blocks fs_write on guest");
        check(!p.validate("fs_write", R"({"path":"/home/op/ctx/a.txt"})").empty(),
              "RO bind blocks fs_write on host path");

        auto rewritten = p.rewritePath("fs_read", R"({"path":"/home/ctx/a.txt"})");
        check(rewritten.find("/home/op/ctx/a.txt") != std::string::npos,
              "rewritePath maps guest→host");
    }

    // ── Test 9: makePolicyFromConfig ──
    {
        AgentConfig cfg;
        cfg.sandboxConfigured = true;
        cfg.sandboxMode = "process";
        cfg.sandboxCommandsSet = true;
        cfg.sandboxAllowedCommands = {"ls", "cat"};
        cfg.sandboxHostsSet = true;
        cfg.sandboxAllowedHosts = {};
        cfg.sandboxReadonly = false;
        SandboxBind b;
        b.host = "/tmp/data";
        b.guest = "/workspace/data";
        b.readOnly = false;
        cfg.sandboxBinds.push_back(b);

        auto p = makePolicyFromConfig(cfg, "/workspace");
        check(p.enabled, "makePolicyFromConfig enables when configured");
        check(p.allowedCommands.size() == 2, "commands carried");
        check(p.binds.size() == 1, "binds carried");
        check(p.validate("exec", R"({"command":"ls"})").empty(), "cfg commands allow ls");
        check(!p.validate("exec", R"({"command":"rm"})").empty(), "cfg commands block rm");
        check(!p.validate("web_fetch", R"({"url":"https://x.com"})").empty(),
              "empty hosts from cfg blocks fetch");
        check(p.validate("fs_write", R"({"path":"/workspace/data/f"})").empty(),
              "RW bind allows write under guest");
    }

    // ── Test 10: CLI merge ──
    {
        SandboxPolicy base;  // disabled
        auto m = mergeCliSandbox(base, true, true, "/workspace");
        check(m.enabled && m.readOnly, "CLI --sandbox-ro enables RO preset");

        AgentConfig cfg;
        cfg.sandboxConfigured = true;
        cfg.sandboxCommandsSet = true;
        cfg.sandboxAllowedCommands = {"ls"};
        auto p = makePolicyFromConfig(cfg, "/ws");
        auto m2 = mergeCliSandbox(p, true, true, "/ws");
        check(m2.enabled && m2.readOnly, "CLI RO overlays manifest policy");
        check(m2.allowedCommands.size() == 1, "manifest commands preserved under CLI RO");
    }

    std::cout << "\n═══ " << (failures ? "FAILED" : "ALL PASSED") << " (" << failures
              << " failures) ═══\n";
    return failures ? 1 : 0;
}
