// sandbox_policy_test.cpp — validates SandboxPolicy without LLM calls
#include "../src/sandbox/policy.hpp"
#include <cassert>
#include <iostream>

using namespace cortex::mk3::sandbox;

int main() {
    int failures = 0;
    auto check = [&](bool cond, const char* test) {
        if (!cond) { std::cerr << "FAIL: " << test << "\n"; failures++; }
        else { std::cout << "  OK: " << test << "\n"; }
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
        check(p.validate("exec", R"({"command":"ls -la"})").empty(),
              "harness sandbox allows 'ls'");
        check(p.validate("exec", R"({"command":"grep foo *.cpp"})").empty(),
              "harness sandbox allows 'grep'");
        check(p.validate("exec", R"({"command":"cat /etc/passwd"})").empty(),
              "harness sandbox allows 'cat'");
        check(p.validate("exec", R"({"command":"make"})").empty(),
              "harness sandbox allows 'make'");
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
              "RO sandbox allows grep (no path filter)");
        check(p.validate("list", R"({"path":"."})").empty(),
              "RO sandbox allows list");
        check(p.validate("context_pin", R"({"path":"src/main.cpp"})").empty(),
              "RO sandbox allows context_pin (relative)");
    }

    std::cout << "\n═══ " << (failures ? "FAILED" : "ALL PASSED")
              << " (" << failures << " failures) ═══\n";
    return failures ? 1 : 0;
}
