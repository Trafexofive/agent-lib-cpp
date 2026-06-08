// =============================================================================
// agent-lib-MK3 — Parser Unit Tests
// Tests the XML streaming parser with hand-crafted token sequences.
// =============================================================================

#include "src/protocol/parser.hpp"
#include <iostream>
#include <cassert>
#include <vector>
#include <string>

using namespace cortex::mk3::protocol;

// ── Test harness ──────────────────────────────────────────────────────
struct TestHarness {
    std::vector<TokenEvent> events;
    std::map<std::string, Json::Value> results;
    bool responseFinal = false;
    std::string responseText;

    void onEvent(const TokenEvent& ev) {
        events.push_back(ev);
        if (ev.type == TokenEvent::RESPONSE) {
            responseText += ev.content;
            if (ev.metadata.count("is_final") && ev.metadata.at("is_final") == "true")
                responseFinal = true;
        }
    }

    Json::Value executeAction(const ParsedAction& action) {
        Json::Value r;
        r["success"] = true;
        r["id"] = action.id;
        r["name"] = action.name;
        r["result"] = "ok";
        return r;
    }
};

static int passed = 0, failed = 0;

#define TEST(name) do { std::cout << "  " << name << "... "; } while(0)
#define PASS() do { passed++; std::cout << "PASS\n"; } while(0)
#define FAIL(msg) do { failed++; std::cout << "FAIL: " << msg << "\n"; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

// ═══════════════════════════════════════════════════════════════════════
// Test 1: Simple text passthrough (no tags)
// ═══════════════════════════════════════════════════════════════════════
void test_simple_text() {
    TEST("simple text passthrough");
    TestHarness h;
    Parser parser([&](const ParsedAction& a) { return h.executeAction(a); });
    parser.onEvent([&](const TokenEvent& ev) { h.onEvent(ev); });

    parser.feed("Hello", false);
    parser.feed(" world", false);
    parser.feed("!", true);

    CHECK(h.events.size() >= 1, "expected at least 1 TEXT event");
    CHECK(h.events[0].type == TokenEvent::TEXT, "first event should be TEXT");
    CHECK(h.events[0].content.find("Hello") != std::string::npos, "should contain Hello");
    PASS();
}

// ═══════════════════════════════════════════════════════════════════════
// Test 2: Full response tag — one token
// ═══════════════════════════════════════════════════════════════════════
void test_response_single_token() {
    TEST("full response tag in one token");
    TestHarness h;
    Parser parser([&](const ParsedAction& a) { return h.executeAction(a); });
    parser.onEvent([&](const TokenEvent& ev) { h.onEvent(ev); });

    parser.feed("<response final=\"true\">Hello, world!</response>", true);

    CHECK(!h.responseText.empty(), "expected response text");
    CHECK(h.responseFinal, "expected final=true");
    CHECK(h.responseText == "Hello, world!", "expected exact response text");
    PASS();
}

// ═══════════════════════════════════════════════════════════════════════
// Test 3: Response tag — token-by-token (streaming)
// ═══════════════════════════════════════════════════════════════════════
void test_response_streaming() {
    TEST("response tag token-by-token");
    TestHarness h;
    Parser parser([&](const ParsedAction& a) { return h.executeAction(a); });
    parser.onEvent([&](const TokenEvent& ev) { h.onEvent(ev); });

    // Simulate DeepSeek streaming: one token at a time
    parser.feed("<", false);
    parser.feed("response", false);
    parser.feed(" final", false);
    parser.feed("=\"true", false);
    parser.feed("\"", false);
    parser.feed(">", false);
    parser.feed("Hello", false);
    parser.feed(",", false);
    parser.feed(" world", false);
    parser.feed("!", false);
    parser.feed("</response", false);
    parser.feed(">", true);

    CHECK(!h.responseText.empty(), "expected response text");
    CHECK(h.responseFinal, "expected final=true");
    CHECK(h.responseText == "Hello, world!", "expected exact response text");
    PASS();
}

// ═══════════════════════════════════════════════════════════════════════
// Test 4: Action tag — one token
// ═══════════════════════════════════════════════════════════════════════
void test_action_single_token() {
    TEST("full action tag in one token");
    TestHarness h;
    Parser parser([&](const ParsedAction& a) { return h.executeAction(a); });
    parser.onEvent([&](const TokenEvent& ev) { h.onEvent(ev); });

    parser.feed(
        "<action type=\"tool\" name=\"list\" id=\"s1\" mode=\"sync\">"
        "{\"path\":\".\"}"
        "</action>",
        true);

    // Should have: ACTION_START event + ACTION_RESULT event
    bool hasStart = false, hasResult = false;
    for (auto& ev : h.events) {
        if (ev.type == TokenEvent::ACTION_START) hasStart = true;
        if (ev.type == TokenEvent::ACTION_RESULT) hasResult = true;
    }
    CHECK(hasStart, "expected ACTION_START event");
    CHECK(hasResult, "expected ACTION_RESULT event");

    auto result = parser.getResult("s1");
    CHECK(result.isMember("success") && result["success"].asBool(), "expected success result");
    PASS();
}

// ═══════════════════════════════════════════════════════════════════════
// Test 5: Action tag — token-by-token (streaming)
// ═══════════════════════════════════════════════════════════════════════
void test_action_streaming() {
    TEST("action tag token-by-token");
    TestHarness h;
    Parser parser([&](const ParsedAction& a) { return h.executeAction(a); });
    parser.onEvent([&](const TokenEvent& ev) { h.onEvent(ev); });

    // Token by token — worst case streaming
    parser.feed("<", false);
    parser.feed("action", false);
    parser.feed(" ", false);
    parser.feed("type=\"tool\"", false);
    parser.feed(" ", false);
    parser.feed("name=\"exec\"", false);
    parser.feed(" ", false);
    parser.feed("id=\"s1\"", false);
    parser.feed(" ", false);
    parser.feed("mode=\"sync\"", false);
    parser.feed(">", false);
    parser.feed("{\"command\":\"ls\"}", false);
    parser.feed("</action", false);
    parser.feed(">", true);

    bool hasStart = false, hasResult = false;
    for (auto& ev : h.events) {
        if (ev.type == TokenEvent::ACTION_START) hasStart = true;
        if (ev.type == TokenEvent::ACTION_RESULT) hasResult = true;
    }
    CHECK(hasStart, "expected ACTION_START event");
    CHECK(hasResult, "expected ACTION_RESULT event");

    auto result = parser.getResult("s1");
    CHECK(result.isMember("success") && result["success"].asBool(), "expected success result");
    PASS();
}

// ═══════════════════════════════════════════════════════════════════════
// Test 6: Action + Response in sequence
// ═══════════════════════════════════════════════════════════════════════
void test_action_then_response() {
    TEST("action then response");
    TestHarness h;
    Parser parser([&](const ParsedAction& a) { return h.executeAction(a); });
    parser.onEvent([&](const TokenEvent& ev) { h.onEvent(ev); });

    parser.feed("<action type=\"tool\" name=\"list\" id=\"s1\" mode=\"sync\">{\"path\":\".\"}</action>", false);
    parser.feed("<response final=\"true\">Files listed.</response>", true);

    bool hasResult = false;
    for (auto& ev : h.events) {
        if (ev.type == TokenEvent::ACTION_RESULT) hasResult = true;
    }
    CHECK(hasResult, "expected ACTION_RESULT");
    CHECK(h.responseFinal, "expected final response");
    PASS();
}

// ═══════════════════════════════════════════════════════════════════════
// Test 7: Thought tag (should be internal, not in response)
// ═══════════════════════════════════════════════════════════════════════
void test_thought_tag() {
    TEST("thought tag is internal");
    TestHarness h;
    Parser parser([&](const ParsedAction& a) { return h.executeAction(a); });
    parser.onEvent([&](const TokenEvent& ev) { h.onEvent(ev); });

    parser.feed("<thought>Let me think about this.</thought>", false);
    parser.feed("<response final=\"true\">Here is the answer.</response>", true);

    bool hasThought = false;
    for (auto& ev : h.events) {
        if (ev.type == TokenEvent::THOUGHT) hasThought = true;
    }
    CHECK(hasThought, "expected THOUGHT event");
    CHECK(h.responseText == "Here is the answer.", "response should not include thought");
    PASS();
}

// ═══════════════════════════════════════════════════════════════════════
// Test 8: Variable resolution — ${id}
// ═══════════════════════════════════════════════════════════════════════
void test_variable_resolution() {
    TEST("variable resolution ${id}");
    TestHarness h;
    Parser parser([&](const ParsedAction& a) { return h.executeAction(a); });
    parser.onEvent([&](const TokenEvent& ev) { h.onEvent(ev); });

    // First action produces a result with ID "step1"
    parser.feed("<action type=\"tool\" name=\"exec\" id=\"step1\" mode=\"sync\">"
                "{\"command\":\"echo hello\"}</action>", false);

    // Second action references ${step1} — should be resolved
    parser.feed("<action type=\"tool\" name=\"exec\" id=\"step2\" mode=\"sync\">"
                "{\"command\":\"${step1.result}\"}</action>", true);

    CHECK(parser.getResult("step1").isObject(), "step1 should have result");
    CHECK(parser.getResult("step2").isObject(), "step2 should have result");
    PASS();
}

// ═══════════════════════════════════════════════════════════════════════
// Test 9: Text between tags should be emitted as TEXT events
// ═══════════════════════════════════════════════════════════════════════
void test_text_between_tags() {
    TEST("text between tags");
    TestHarness h;
    Parser parser([&](const ParsedAction& a) { return h.executeAction(a); });
    parser.onEvent([&](const TokenEvent& ev) { h.onEvent(ev); });

    parser.feed("Before ", false);
    parser.feed("<action type=\"tool\" name=\"list\" id=\"s1\" mode=\"sync\">{\"path\":\".\"}</action>", false);
    parser.feed(" between ", false);
    parser.feed("<response final=\"true\">After</response>", true);

    bool hasText = false, hasResponse = false;
    for (auto& ev : h.events) {
        if (ev.type == TokenEvent::TEXT) {
            if (ev.content.find("Before") != std::string::npos) hasText = true;
        }
        if (ev.type == TokenEvent::RESPONSE) hasResponse = true;
    }
    CHECK(hasText, "expected TEXT events between tags");
    CHECK(hasResponse, "expected RESPONSE event");
    PASS();
}

// ═══════════════════════════════════════════════════════════════════════
// main
// ═══════════════════════════════════════════════════════════════════════
int main() {
    std::cout << "\n╔══════════════════════════════════════════╗\n";
    std::cout << "║   MK3 Parser Unit Tests                  ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n\n";

    test_simple_text();
    test_response_single_token();
    test_response_streaming();
    test_action_single_token();
    test_action_streaming();
    test_action_then_response();
    test_thought_tag();
    test_variable_resolution();
    test_text_between_tags();

    std::cout << "\n──────────────────────────────────────────\n";
    std::cout << "  " << passed << " passed, " << failed << " failed\n";
    std::cout << "──────────────────────────────────────────\n";

    return failed > 0 ? 1 : 0;
}
