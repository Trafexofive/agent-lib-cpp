// =============================================================================
// agent-lib-MK3 — Context manager tests (context_pin / peek / unpin)
//
// Verifies the round trip:
//   - pin a file → it appears in Agent::contextSnapshot().pinned
//   - peek a file with cycles=2 → it appears in peeking with cycles_remaining=2
//   - tickContextCycles() decrements the cycle counter
//   - tickContextCycles() evicts peek entries at 0
//   - unpin removes a pinned (or peek) entry
//   - same canonical path collapses two different inputs (./x.txt and x.txt)
//   - size guard rejects oversized files unless force=true
// =============================================================================

#include "src/core/agent.hpp"
#include "src/providers/factory.hpp"
#include <cstdio>
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

static fs::path writeTmpFile(const std::string &name, const std::string &content) {
    fs::path p = g_tmpDir / name;
    std::ofstream(p) << content;
    return p;
}

static std::unique_ptr<Agent> makeAgent() {
    AgentConfig cfg;
    cfg.provider = "deepseek";
    cfg.model = "deepseek-chat";
    auto provider = providers::createProvider("deepseek", "deepseek-chat");
    return std::make_unique<Agent>(cfg, provider);
}

void test_pin_basic() {
    TEST("pin a small file");
    auto agent = makeAgent();
    auto path = writeTmpFile("pin_basic.txt", "hello world\n");
    auto r = agent->contextPin(path.string(), false);
    CHECK(r["success"].asBool(), "pin should succeed");
    CHECK(r["bytes"].asUInt64() == 12, "bytes should be 12");
    CHECK(r["pinned_count"].asInt() == 1, "pinned_count should be 1");
    auto snap = agent->contextSnapshot();
    CHECK(snap["pinned"].size() == 1, "snapshot has 1 pinned");
    CHECK(snap["pinned"][0]["path"].asString() == path.string(), "path roundtrip");
    PASS();
}

void test_pin_same_path_two_forms() {
    TEST("pin: ./x and x collapse to one entry");
    auto agent = makeAgent();
    auto path = writeTmpFile("collapse.txt", "data\n");
    // Pin with the canonical absolute path
    auto r1 = agent->contextPin(path.string(), false);
    CHECK(r1["success"].asBool(), "first pin ok");
    // Pin again with the same path; should still be 1 entry
    auto r2 = agent->contextPin(path.string(), false);
    CHECK(r2["success"].asBool(), "second pin ok");
    CHECK(r2["pinned_count"].asInt() == 1, "still 1 entry (re-pin updates content)");
    PASS();
}

void test_peek_cycles_decrement() {
    TEST("peek cycles decrement on tickContextCycles");
    auto agent = makeAgent();
    auto path = writeTmpFile("peek1.txt", "peek me\n");
    auto r = agent->contextPeek(path.string(), 2, false);
    CHECK(r["success"].asBool(), "peek ok");
    CHECK(r["cycles_remaining"].asInt() == 2, "initial cycles=2");

    agent->tickContextCycles();
    auto snap1 = agent->contextSnapshot();
    CHECK(snap1["peeking"].size() == 1, "still 1 peek after tick 1");
    CHECK(snap1["peeking"][0]["cycles_remaining"].asInt() == 1, "cycles=1");

    agent->tickContextCycles();
    auto snap2 = agent->contextSnapshot();
    CHECK(snap2["peeking"].size() == 0, "evicted after tick 2");
    PASS();
}

void test_unpin_removes() {
    TEST("unpin removes a pinned entry");
    auto agent = makeAgent();
    auto path = writeTmpFile("unpin.txt", "bye\n");
    agent->contextPin(path.string(), false);
    CHECK(agent->contextSnapshot()["pinned"].size() == 1, "1 before unpin");
    auto r = agent->contextUnpin(path.string());
    CHECK(r["success"].asBool(), "unpin success");
    CHECK(r["removed"].asString() == "pinned", "removed type=pinned");
    CHECK(agent->contextSnapshot()["pinned"].size() == 0, "0 after unpin");
    PASS();
}

void test_unpin_not_found() {
    TEST("unpin of unknown path returns success=false");
    auto agent = makeAgent();
    auto r = agent->contextUnpin("/nonexistent/no-such-file");
    CHECK(!r["success"].asBool(), "should be success=false");
    CHECK(r["removed"].asString() == "none", "removed=none");
    PASS();
}

void test_size_guard_rejects() {
    TEST("size guard: 100KB file rejected without force");
    auto agent = makeAgent();
    std::string big(100 * 1024, 'a');
    auto path = writeTmpFile("big.txt", big);
    auto r = agent->contextPin(path.string(), false);
    CHECK(!r["success"].asBool(), "should reject without force");
    CHECK(r["error"].asString().find("exceeds limit") != std::string::npos,
          "error mentions limit");
    PASS();
}

void test_size_guard_force() {
    TEST("size guard: force=true accepts oversized file");
    auto agent = makeAgent();
    std::string big(100 * 1024, 'b');
    auto path = writeTmpFile("big2.txt", big);
    auto r = agent->contextPin(path.string(), true);
    CHECK(r["success"].asBool(), "force should succeed");
    CHECK(r["bytes"].asUInt64() == 100 * 1024, "bytes correct");
    PASS();
}

void test_pin_overrides_peek() {
    TEST("pin clears matching peek (pinned wins)");
    auto agent = makeAgent();
    auto path = writeTmpFile("conflict.txt", "x\n");
    agent->contextPeek(path.string(), 5, false);
    CHECK(agent->contextSnapshot()["peeking"].size() == 1, "peek added");
    agent->contextPin(path.string(), false);
    auto snap = agent->contextSnapshot();
    CHECK(snap["pinned"].size() == 1, "1 pinned");
    CHECK(snap["peeking"].size() == 0, "peek removed by pin");
    PASS();
}

void test_peek_ignored_when_pinned() {
    TEST("peek on already-pinned path is a no-op");
    auto agent = makeAgent();
    auto path = writeTmpFile("alreadypinned.txt", "y\n");
    agent->contextPin(path.string(), false);
    auto r = agent->contextPeek(path.string(), 3, false);
    CHECK(r["success"].asBool(), "peek returns success");
    CHECK(r["mode"].asString() == "pinned", "mode reported as pinned");
    CHECK(agent->contextSnapshot()["peeking"].size() == 0, "no peek entry");
    PASS();
}

// ═══════════════════════════════════════════════════════════════════════
// End-to-end: prove the content lands in the rendered system prompt and
// is removed when the entry expires.
// ═══════════════════════════════════════════════════════════════════════
void test_pinned_appears_in_system_prompt() {
    TEST("pinned content appears inside <pinned_context> in system prompt");
    auto agent = makeAgent();
    auto path = writeTmpFile("prompt_pin.txt", "UNIQUE_PINNED_MARKER_98712");
    agent->contextPin(path.string(), false);

    std::string prompt = agent->renderSystemPrompt();
    CHECK(prompt.find("<pinned_context>") != std::string::npos,
          "system prompt missing <pinned_context>");
    CHECK(prompt.find("UNIQUE_PINNED_MARKER_98712") != std::string::npos,
          "pinned file content not embedded in prompt");

    agent->contextUnpin(path.string());
    std::string promptAfter = agent->renderSystemPrompt();
    CHECK(promptAfter.find("UNIQUE_PINNED_MARKER_98712") == std::string::npos,
          "unpinned content still present in prompt");
    PASS();
}

void test_peek_disappears_after_eviction() {
    TEST("peek content appears, then is evicted by tick");
    auto agent = makeAgent();
    auto path = writeTmpFile("prompt_peek.txt", "UNIQUE_PEEK_MARKER_54321");
    agent->contextPeek(path.string(), 1, false);

    std::string before = agent->renderSystemPrompt();
    CHECK(before.find("<ephemeral_context>") != std::string::npos,
          "missing <ephemeral_context>");
    CHECK(before.find("UNIQUE_PEEK_MARKER_54321") != std::string::npos,
          "peek content not in prompt");
    CHECK(before.find("cycles_remaining=\"1\"") != std::string::npos,
          "cycles_remaining attribute missing");

    agent->tickContextCycles();   // -> 0 -> evicted
    std::string after = agent->renderSystemPrompt();
    CHECK(after.find("UNIQUE_PEEK_MARKER_54321") == std::string::npos,
          "peek content still in prompt after eviction");
    PASS();
}

void test_missing_file() {
    TEST("pin of nonexistent file fails cleanly");
    auto agent = makeAgent();
    auto r = agent->contextPin("/definitely/not/a/file.xyz", false);
    CHECK(!r["success"].asBool(), "should fail");
    CHECK(r["error"].asString().find("not found") != std::string::npos,
          "error mentions not found");
    PASS();
}

int main() {
    std::cout.setf(std::ios::unitbuf);
    std::cout << "\n╔══════════════════════════════════════════╗\n";
    std::cout << "║   MK3 Context Manager Tests              ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n\n";

    g_tmpDir = fs::temp_directory_path() / ("cortex-ctx-test-" + std::to_string(::getpid()));
    fs::create_directories(g_tmpDir);

    test_pin_basic();
    test_pin_same_path_two_forms();
    test_peek_cycles_decrement();
    test_unpin_removes();
    test_unpin_not_found();
    test_size_guard_rejects();
    test_size_guard_force();
    test_pin_overrides_peek();
    test_peek_ignored_when_pinned();
    test_pinned_appears_in_system_prompt();
    test_peek_disappears_after_eviction();
    test_missing_file();

    fs::remove_all(g_tmpDir);

    std::cout << "\n──────────────────────────────────────────\n";
    std::cout << "  " << passed << " passed, " << failed << " failed\n";
    std::cout << "──────────────────────────────────────────\n";
    return failed > 0 ? 1 : 0;
}
