// =============================================================================
// ScriptedProvider — pinned behavior test.
//
// ScriptedProvider is the enabler for the sub-agent test phase: it lets a
// test script the exact protocol responses the parent and child agents emit,
// so we can drive real <action type="agent"> delegation, tool calls,
// multi-turn flows, and edge cases (refusals, errors) without touching the
// network. This test pins the contract.
//
// The contract is deliberately small:
//   - generate()        : pops the front response, returns it (FIFO).
//   - generateStream()  : delivers the whole response as one chunk with
//                         isFinal=true (deterministic, parser-stable).
//   - exhaustion        : throws std::runtime_error (loud failure, not silent).
//   - providerName()    : "scripted".
//   - remaining/exhausted: introspection for test setup.
// =============================================================================

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/core/types.hpp"
#include "src/testing/scripted_provider.hpp"

using namespace cortex::mk3;
using namespace cortex::mk3::testing;

static int passed = 0, failed = 0;

#define TEST(name)                           \
    do {                                     \
        std::cout << "  " << name << "... "; \
    } while (0)
#define PASS()                 \
    do {                       \
        passed++;              \
        std::cout << "PASS\n"; \
    } while (0)
#define FAIL(msg)                                \
    do {                                         \
        failed++;                                \
        std::cout << "FAIL — " << msg << "\n";   \
    } while (0)
#define CHECK(cond, msg)            \
    do {                            \
        if (!(cond)) {              \
            FAIL(msg);              \
        }                           \
    } while (0)

void test_generate_pops_fifo() {
    TEST("generate() pops the front response in FIFO order");
    ScriptedProvider p(std::deque<std::string>{"R1", "R2", "R3"});
    CHECK(p.remaining() == 3, "queue starts with 3 responses");
    CHECK(!p.exhausted(), "queue is not exhausted before draining");

    CHECK(p.generate({}) == "R1", "first generate returns R1");
    CHECK(p.remaining() == 2, "remaining decremented after first pop");
    CHECK(p.generate({}) == "R2", "second generate returns R2");
    CHECK(p.generate({}) == "R3", "third generate returns R3");
    CHECK(p.remaining() == 0, "remaining is 0 after draining");
    CHECK(p.exhausted(), "exhausted reports true when empty");
    PASS();
}

void test_generate_stream_delivers_single_chunk() {
    TEST("generateStream() delivers the whole response as one chunk with isFinal=true");
    ScriptedProvider p(std::deque<std::string>{"<response final=\"true\">hello</response>"});
    std::vector<std::string> chunks;
    std::vector<bool> finals;
    p.generateStream({}, [&](const std::string& token, bool isFinal) {
        chunks.push_back(token);
        finals.push_back(isFinal);
    });
    CHECK(chunks.size() == 1, "exactly one streaming chunk");
    CHECK(chunks[0] == "<response final=\"true\">hello</response>",
          "chunk content is the full scripted response");
    CHECK(finals[0] == true, "the single chunk is marked isFinal=true");
    PASS();
}

void test_stream_null_callback_throws() {
    TEST("generateStream() throws on null callback");
    ScriptedProvider p(std::deque<std::string>{"x"});
    bool threw = false;
    try {
        p.generateStream({}, nullptr);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw, "null callback must throw (loud, not silent)");
    PASS();
}

void test_exhaustion_throws() {
    TEST("generate() and generateStream() throw on exhaustion");
    ScriptedProvider p(std::deque<std::string>{"only"});
    (void)p.generate({});
    CHECK(p.exhausted(), "single-response queue is exhausted after one pop");

    bool genThrew = false;
    try {
        (void)p.generate({});
    } catch (const std::runtime_error&) {
        genThrew = true;
    }
    CHECK(genThrew, "generate() on empty queue must throw");

    bool streamThrew = false;
    try {
        p.generateStream({}, [](const std::string&, bool) {});
    } catch (const std::runtime_error&) {
        streamThrew = true;
    }
    CHECK(streamThrew, "generateStream() on empty queue must throw");
    PASS();
}

void test_provider_name_and_introspection() {
    TEST("providerName() is 'scripted'; setters store values");
    ScriptedProvider p(std::deque<std::string>{"x"});
    CHECK(p.providerName() == "scripted", "providerName returns 'scripted'");
    CHECK(p.getModel() == "scripted-model", "default model is 'scripted-model'");

    p.setModel("gpt-test");
    p.setTemperature(0.42);
    p.setMaxTokens(2048);
    p.setTopP(0.88);
    CHECK(p.getModel() == "gpt-test", "setModel updates model");
    CHECK(p.getTemperature() == 0.42, "setTemperature updates temperature");
    CHECK(p.getMaxTokens() == 2048, "setMaxTokens updates maxTokens");
    // (No getTopP() — the ILlmProvider interface does not expose it.)
    PASS();
}

void test_empty_constructor_starts_exhausted() {
    TEST("default-constructed provider is immediately exhausted");
    ScriptedProvider p;
    CHECK(p.exhausted(), "default-constructed queue is exhausted");
    CHECK(p.remaining() == 0, "default-constructed queue has 0 remaining");
    bool threw = false;
    try {
        (void)p.generate({});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw, "default-constructed provider throws on first call");
    PASS();
}

int main() {
    std::cout << "ScriptedProvider — behavior contract test\n";
    test_generate_pops_fifo();
    test_generate_stream_delivers_single_chunk();
    test_stream_null_callback_throws();
    test_exhaustion_throws();
    test_provider_name_and_introspection();
    test_empty_constructor_starts_exhausted();
    std::cout << "\n" << (failed == 0 ? "all passed" : "failures: " + std::to_string(failed))
              << " (" << passed << " passed)\n";
    return failed == 0 ? 0 : 1;
}
