// =============================================================================
// agent-lib-MK3 — Sandbox + context_* integration tests
//
// Targets:
//   SB02 — isWithinWorkspace must canonicalise paths so `workspace/../outside`
//          is correctly rejected (the legacy naive prefix check passed it).
//   SB07 — context_pin/peek must go through sandbox validate() with the
//          workspace-path check applied.
//   BT04 — sandbox validation runs BEFORE meta-tools and context_*, so they
//          can't bypass the policy.
// =============================================================================

#include "src/core/agent.hpp"
#include "src/sandbox/policy.hpp"
#include "src/providers/factory.hpp"
#include "src/protocol/parser.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

using namespace cortex::mk3;
namespace fs = std::filesystem;

static int passed = 0, failed = 0;

#define TEST(name)  do { std::cout << "  " << name << "... "; } while (0)
#define PASS()      do { passed++; std::cout << "PASS\n"; } while (0)
#define FAIL(msg)   do { failed++; std::cout << "FAIL: " << msg << "\n"; return; } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); } } while (0)

static fs::path g_tmpDir;
static fs::path g_workspace;
static fs::path g_outside;

static std::unique_ptr<Agent> makeSandboxedAgent() {
    AgentConfig cfg;
    cfg.name = "sandbox-test";
    cfg.provider = "deepseek";
    cfg.model = "deepseek-chat";
    auto p = providers::createProvider("deepseek", "deepseek-chat");
    auto agent = std::make_unique<Agent>(cfg, p);
    sandbox::SandboxPolicy policy = sandbox::makeHarnessSandbox(g_workspace.string());
    agent->setSandboxPolicy(policy);
    return agent;
}

static protocol::ParsedAction makeAction(const std::string &name,
                                          const std::string &path,
                                          int cycles = 0) {
    protocol::ParsedAction a;
    a.type = protocol::ActionType::TOOL;
    a.mode = protocol::ExecutionMode::SYNC;
    a.name = name;
    a.id = "x1";
    a.params = Json::Value(Json::objectValue);
    a.params["path"] = path;
    if (cycles > 0) a.params["cycles"] = cycles;
    return a;
}

// We can't directly call Agent::dispatchTool (private). But the public way
// to drive it is the parser's executor callback. For these tests we'll
// validate the policy directly — that's where the rules live.

// ─────────────────────────────────────────────────────────────────────────────
// SB02 — `workspace/../outside` must be rejected
// ─────────────────────────────────────────────────────────────────────────────
void test_SB02_traversal_rejected() {
    TEST("SB02 workspace/../outside is rejected");
    sandbox::SandboxPolicy p = sandbox::makeHarnessSandbox(g_workspace.string());

    std::string traversalAbs = (g_workspace / "subdir" / ".." / ".." /
                                 g_outside.filename()).string();
    std::string params = "{\"path\":\"" + traversalAbs + "\"}";
    std::string err = p.validate("fs_read", params);
    CHECK(!err.empty(), "traversal path should have been rejected");
    CHECK(err.find("outside workspace") != std::string::npos,
          "error should mention 'outside workspace'");
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// SB07 — context_pin / context_peek are gated by sandbox
// ─────────────────────────────────────────────────────────────────────────────
void test_SB07_context_pin_outside_workspace_blocked() {
    TEST("SB07 context_pin outside workspace is blocked");
    sandbox::SandboxPolicy p = sandbox::makeHarnessSandbox(g_workspace.string());
    std::string params = "{\"path\":\"" + g_outside.string() + "/escape.txt\"}";
    std::string err = p.validate("context_pin", params);
    CHECK(!err.empty(), "context_pin outside workspace should be rejected");
    CHECK(err.find("outside workspace") != std::string::npos,
          "error mentions outside workspace");
    PASS();
}

void test_SB07_context_peek_outside_workspace_blocked() {
    TEST("SB07 context_peek outside workspace is blocked");
    sandbox::SandboxPolicy p = sandbox::makeHarnessSandbox(g_workspace.string());
    std::string params = "{\"path\":\"/etc/passwd\"}";
    std::string err = p.validate("context_peek", params);
    CHECK(!err.empty(), "context_peek of /etc/passwd should be rejected");
    PASS();
}

void test_SB07_context_pin_inside_workspace_allowed() {
    TEST("SB07 context_pin inside workspace is allowed");
    sandbox::SandboxPolicy p = sandbox::makeHarnessSandbox(g_workspace.string());
    std::string inside = (g_workspace / "ok.txt").string();
    std::string params = "{\"path\":\"" + inside + "\"}";
    std::string err = p.validate("context_pin", params);
    CHECK(err.empty(),
          (std::string("inside-workspace pin was blocked: '") + err + "'").c_str());
    PASS();
}

void test_SB07_context_unpin_not_blocked() {
    TEST("SB07 context_unpin always allowed (no disk access)");
    sandbox::SandboxPolicy p = sandbox::makeHarnessSandbox(g_workspace.string());
    // Even a path outside the workspace is fine for unpin — it doesn't touch
    // the disk, it only manipulates in-memory state.
    std::string params = "{\"path\":\"/etc/passwd\"}";
    std::string err = p.validate("context_unpin", params);
    CHECK(err.empty(), "unpin should not be sandbox-gated");
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Bonus: sanity that fs_read inside workspace still works
// ─────────────────────────────────────────────────────────────────────────────
void test_fs_read_inside_workspace_allowed() {
    TEST("fs_read inside workspace stays allowed");
    sandbox::SandboxPolicy p = sandbox::makeHarnessSandbox(g_workspace.string());
    std::string params = "{\"path\":\"" + (g_workspace / "ok.txt").string() + "\"}";
    std::string err = p.validate("fs_read", params);
    CHECK(err.empty(), "regression: inside-workspace read got blocked");
    PASS();
}

int main() {
    std::cout.setf(std::ios::unitbuf);
    std::cout << "\n╔══════════════════════════════════════════╗\n";
    std::cout << "║   MK3 Sandbox + context_* Tests          ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n\n";

    g_tmpDir = fs::temp_directory_path() / ("cortex-sandbox-test-" + std::to_string(::getpid()));
    g_workspace = g_tmpDir / "workspace";
    g_outside = g_tmpDir / "outside";
    fs::create_directories(g_workspace);
    fs::create_directories(g_outside);
    std::ofstream(g_workspace / "ok.txt") << "ok";
    std::ofstream(g_outside / "escape.txt") << "should be blocked";

    test_SB02_traversal_rejected();
    test_SB07_context_pin_outside_workspace_blocked();
    test_SB07_context_peek_outside_workspace_blocked();
    test_SB07_context_pin_inside_workspace_allowed();
    test_SB07_context_unpin_not_blocked();
    test_fs_read_inside_workspace_allowed();

    fs::remove_all(g_tmpDir);

    std::cout << "\n──────────────────────────────────────────\n";
    std::cout << "  " << passed << " passed, " << failed << " failed\n";
    std::cout << "──────────────────────────────────────────\n";
    return failed > 0 ? 1 : 0;
}
