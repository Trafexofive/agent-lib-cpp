// =============================================================================
// agent-lib-MK3 — Parser Unit Tests
// Tests the XML streaming parser with hand-crafted token sequences.
// =============================================================================

#include "src/protocol/parser.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace cortex::mk3::protocol;

// ── Test harness ──────────────────────────────────────────────────────
struct TestHarness {
    std::vector<TokenEvent> events;
    std::vector<ParsedAction> actions;
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
        actions.push_back(action);
        Json::Value r;
        r["success"] = true;
        r["id"] = action.id;
        r["name"] = action.name;
        r["result"] = "ok";
        return r;
    }
};

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
#define FAIL(msg)                             \
    do {                                      \
        failed++;                             \
        std::cout << "FAIL: " << msg << "\n"; \
    } while (0)
#define CHECK(cond, msg) \
    do {                 \
        if (!(cond)) {   \
            FAIL(msg);   \
            return;      \
        }                \
    } while (0)

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

void test_response_literal_protocol_tags() {
    TEST("response can contain literal protocol tag examples");
    TestHarness h;
    Parser parser([&](const ParsedAction& a) { return h.executeAction(a); });
    parser.onEvent([&](const TokenEvent& ev) { h.onEvent(ev); });

    parser.feed("<response final=\"true\">", false);
    parser.feed("Use `<response>`, `<action type=\"tool\">`, and `<thought>` examples.", false);
    parser.feed(" Final answers look like `<response final=\"true\">ok</response>`.", false);
    parser.feed(" Do not emit `<result>` yourself.", false);
    parser.feed("</response>", true);

    CHECK(h.responseFinal, "expected final=true despite literal tags");
    CHECK(h.actions.empty(), "literal <action> example must not dispatch");
    CHECK(h.responseText.find("`<response>`") != std::string::npos,
          "should preserve literal response tag");
    CHECK(h.responseText.find("`<action type=\"tool\">`") != std::string::npos,
          "should preserve literal action tag");
    CHECK(h.responseText.find("`<response final=\"true\">ok</response>`") != std::string::npos,
          "should preserve literal closing response tag");
    CHECK(h.responseText.find("`<result>`") != std::string::npos,
          "should preserve literal result tag");
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
        if (ev.type == TokenEvent::ACTION_START)
            hasStart = true;
        if (ev.type == TokenEvent::ACTION_RESULT)
            hasResult = true;
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
        if (ev.type == TokenEvent::ACTION_START)
            hasStart = true;
        if (ev.type == TokenEvent::ACTION_RESULT)
            hasResult = true;
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

    parser.feed(
        "<action type=\"tool\" name=\"list\" id=\"s1\" mode=\"sync\">{\"path\":\".\"}</action>",
        false);
    parser.feed("<response final=\"true\">Files listed.</response>", true);

    bool hasResult = false;
    for (auto& ev : h.events) {
        if (ev.type == TokenEvent::ACTION_RESULT)
            hasResult = true;
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
        if (ev.type == TokenEvent::THOUGHT)
            hasThought = true;
    }
    CHECK(hasThought, "expected THOUGHT event");
    CHECK(h.responseText == "Here is the answer.", "response should not include thought");
    PASS();
}

void test_thinking_tag_alias() {
    // <thinking> must fuse to the same THOUGHT stream as <thought>/<think> so
    // the harness context is uniform regardless of which alias the model
    // emits — real test-time-compute thinking and harness-time thinking
    // (model emitting <thinking>...</thinking> organically) look identical
    // downstream.
    TEST("<thinking> tag aliases to thought and closes on </thinking>");
    TestHarness h;
    Parser parser([&](const ParsedAction& a) { return h.executeAction(a); });
    parser.onEvent([&](const TokenEvent& ev) { h.onEvent(ev); });

    parser.feed("<thinking>reasoning across the thinking tag</thinking>", true);
    bool hasThought = false;
    std::string thoughtText;
    for (auto& ev : h.events) {
        if (ev.type == TokenEvent::THOUGHT) {
            hasThought = true;
            thoughtText = ev.content;
        }
    }
    CHECK(hasThought, "<thinking> must emit a THOUGHT event (fused stream)");
    CHECK(thoughtText == "reasoning across the thinking tag",
          "<thinking> body routes to THOUGHT content");
    PASS();
}

void test_thought_streams_accumulate_live() {
    // Real test-time-compute thinking arrives as a stream of partial tokens
    // (the provider sends '\x01'-prefixed chunks) and harness-time thinking
    // arrives as <thought> chunks. Both must accumulate into the same single
    // thought row in the chat so the operator SEES the reasoning being typed
    // — not a wall of text at the end. This test feeds the thought body
    // token-by-token and asserts the parser emits a single accumulated
    // THOUGHT event with the full body (the agent loop back-appends to the
    // last THOUGHT ProtocolEvent, so even if multiple THOUGHT events fire the
    // timeline shows one growing row).
    TEST("thought stream accumulates progressively across tokens");
    TestHarness h;
    Parser parser([&](const ParsedAction& a) { return h.executeAction(a); });
    parser.onEvent([&](const TokenEvent& ev) { h.onEvent(ev); });

    parser.feed("<thought>part ", false);
    parser.feed("one ", false);
    parser.feed("part two</thought>", true);
    int thoughtCount = 0;
    std::string body;
    for (auto& ev : h.events) {
        if (ev.type == TokenEvent::THOUGHT) {
            ++thoughtCount;
            body += ev.content;
        }
    }
    CHECK(thoughtCount >= 1, "thought stream emits at least one THOUGHT event");
    CHECK(body == "part one part two",
          "thought body accumulates across streamed chunks");
    PASS();
}

// ═══════════════════════════════════════════════════════════════════════
// Test 8: Variable resolution — ${id}
// ═══════════════════════════════════════════════════════════════════════
void test_variable_resolution() {
    TEST("variable resolution ${id} / ${id.field} dispatch-time");
    std::string step2Cmd;
    std::string step3Cmd;
    Parser parser([&](const ParsedAction& a) -> Json::Value {
        if (a.id == "step2" && a.params.isMember("command"))
            step2Cmd = a.params["command"].asString();
        if (a.id == "step3" && a.params.isMember("command"))
            step3Cmd = a.params["command"].asString();
        Json::Value r;
        r["success"] = true;
        r["id"] = a.id;
        r["name"] = a.name;
        r["result"] = "ok";
        r["output"] = "hello-from-" + a.id;
        return r;
    });

    parser.feed(
        "<action type=\"tool\" name=\"exec\" id=\"step1\" mode=\"sync\">"
        "{\"command\":\"echo hello\"}</action>",
        false);

    // Field path
    parser.feed(
        "<action type=\"tool\" name=\"exec\" id=\"step2\" mode=\"sync\" depends_on=\"step1\">"
        "{\"command\":\"${step1.result}\"}</action>",
        false);

    // ${id} shorthand → output (CANON §6)
    parser.feed(
        "<action type=\"tool\" name=\"exec\" id=\"step3\" mode=\"sync\" depends_on=\"step1\">"
        "{\"command\":\"${step1}\"}</action>",
        true);

    CHECK(parser.getResult("step1").isObject(), "step1 should have result");
    CHECK(parser.getResult("step2").isObject(), "step2 should have result");
    CHECK(step2Cmd == "ok", (std::string("step2 cmd expected 'ok', got '") + step2Cmd + "'").c_str());
    CHECK(step3Cmd == "hello-from-step1",
          (std::string("${id} shorthand expected output, got '") + step3Cmd + "'").c_str());
    PASS();
}

void test_clear_results_keeps_used_ids() {
    TEST("clearResults keeps usedActionIds across iterations");
    int execCount = 0;
    Parser parser([&](const ParsedAction& a) -> Json::Value {
        (void)a;
        execCount++;
        Json::Value r;
        r["success"] = true;
        r["output"] = "x";
        return r;
    });
    bool sawDupReplay = false;
    parser.onEvent([&](const TokenEvent& ev) {
        if (ev.type == TokenEvent::ERROR &&
            ev.metadata.count("reason") &&
            ev.metadata.at("reason") == "duplicate_action_id_replay")
            sawDupReplay = true;
    });

    parser.feed(
        "<action type=\"tool\" name=\"list\" id=\"same\" mode=\"sync\">{\"path\":\".\"}</action>",
        true);
    CHECK(execCount == 1, "first action should execute");

    parser.clearResults();  // end of iteration — must NOT free the id

    // Duplicate id of a SUCCESSFULLY-completed action now idempotently replays
    // the retained result (fixes the non-recovering stall: a model that
    // re-emits an id sees its prior result instead of a deadlocking error).
    parser.feed(
        "<action type=\"tool\" name=\"list\" id=\"same\" mode=\"sync\">{\"path\":\"/tmp\"}</action>",
        true);
    CHECK(execCount == 1, "duplicate id after clearResults must not re-execute");
    CHECK(sawDupReplay, "duplicate id replay signals duplicate_action_id_replay");
    Json::Value dup = parser.getResult("same");
    CHECK(dup.isObject() && dup.get("success", false).asBool(),
          "replayed duplicate returns the prior successful result");
    PASS();
}

void test_duplicate_id_no_retained_rejects() {
    TEST("duplicate id without retained success remaps to unique id");
    int execCount = 0;
    std::string lastId;
    Parser parser([&](const ParsedAction& a) -> Json::Value {
        execCount++;
        lastId = a.id;
        Json::Value r;
        r["protocol_error"] = true;
        r["success"] = false;
        return r;
    });
    bool sawRemap = false;
    parser.onEvent([&](const TokenEvent& ev) {
        if (ev.type == TokenEvent::ERROR &&
            ev.metadata.count("reason") &&
            ev.metadata.at("reason") == "duplicate_action_id_remapped")
            sawRemap = true;
    });
    // First completion is a protocol_error (not replayable). Re-emit must
    // remap to a unique id and still execute — not poison the batch.
    // Use valid list bodies (hollow {} is rejected before executor).
    parser.feed(
        "<action type=\"tool\" name=\"list\" id=\"a\" mode=\"sync\">"
        "{\"path\":\".\"}</action>", true);
    parser.clearResults();
    parser.feed(
        "<action type=\"tool\" name=\"list\" id=\"a\" mode=\"sync\">"
        "{\"path\":\".\"}</action>", true);
    CHECK(sawRemap, "duplicate id remaps when no retained success");
    CHECK(execCount == 2, "remapped duplicate still executes");
    CHECK(lastId == "a-2" || lastId.rfind("a-", 0) == 0, "id was suffixed");
    PASS();
}

void test_hollow_object_then_full_body() {
    TEST("hollow {} then full body: empty rejected, full executes");
    int execCount = 0;
    std::string lastBody;
    Parser parser([&](const ParsedAction& a) -> Json::Value {
        execCount++;
        // JSON bodies land in params; content may be empty after parse.
        if (a.params.isMember("path") && a.params["path"].isString())
            lastBody = a.params["path"].asString();
        else
            lastBody = a.content;
        Json::Value r;
        r["success"] = true;
        r["output"] = "ok";
        return r;
    });
    int emptyErrs = 0;
    parser.onEvent([&](const TokenEvent& ev) {
        if (ev.type == TokenEvent::ERROR && ev.metadata.count("reason") &&
            ev.metadata.at("reason") == "empty_action_body")
            ++emptyErrs;
    });
    parser.feed(
        "<action type=\"tool\" name=\"list\" id=\"t1\" mode=\"sync\">{}</action>"
        "<action type=\"tool\" name=\"list\" id=\"t1\" mode=\"sync\">"
        "{\"path\":\".\"}</action>",
        true);
    CHECK(emptyErrs >= 1, "hollow {} emits empty_action_body");
    CHECK(execCount == 1, "only full body reaches executor");
    CHECK(lastBody == "." || lastBody.find(".") != std::string::npos,
          "executor saw full body path");
    PASS();
}

void test_invalid_json_body_fail_closed() {
    TEST("invalid JSON body fail-closed");
    int execCount = 0;
    Parser parser([&](const ParsedAction& a) -> Json::Value {
        (void)a;
        execCount++;
        Json::Value r;
        r["success"] = true;
        return r;
    });
    bool sawJsonError = false;
    parser.onEvent([&](const TokenEvent& ev) {
        if (ev.type == TokenEvent::ERROR &&
            ev.metadata.count("reason") &&
            ev.metadata.at("reason") == "invalid_json_body")
            sawJsonError = true;
    });

    parser.feed(
        "<action type=\"tool\" name=\"exec\" id=\"bad1\" mode=\"sync\">"
        "{\"command\": \"echo hi\""
        "</action>",
        true);

    CHECK(execCount == 0, "invalid JSON must not execute tool");
    CHECK(sawJsonError, "expected invalid_json_body error");
    Json::Value r = parser.getResult("bad1");
    CHECK(r.isObject() && r.get("protocol_error", false).asBool(),
          "expected protocol_error result");
    PASS();
}

void test_depends_on_async_rejected() {
    TEST("depends_on + async → protocol_error");
    int execCount = 0;
    Parser parser([&](const ParsedAction& a) -> Json::Value {
        (void)a;
        execCount++;
        Json::Value r;
        r["success"] = true;
        r["output"] = "ok";
        return r;
    });
    bool sawModeError = false;
    parser.onEvent([&](const TokenEvent& ev) {
        if (ev.type == TokenEvent::ERROR &&
            ev.metadata.count("reason") &&
            ev.metadata.at("reason") == "depends_on_mode")
            sawModeError = true;
    });

    parser.feed(
        "<action type=\"tool\" name=\"list\" id=\"p1\" mode=\"sync\">{\"path\":\".\"}</action>"
        "<action type=\"tool\" name=\"list\" id=\"c1\" mode=\"async\" depends_on=\"p1\">"
        "{\"path\":\"/tmp\"}</action>",
        true);
    parser.waitForActions();

    CHECK(execCount == 1, "only producer should execute");
    CHECK(sawModeError, "expected depends_on_mode error");
    Json::Value c1 = parser.getResult("c1");
    CHECK(c1.isObject() && c1.get("protocol_error", false).asBool(),
          "expected protocol_error on consumer");
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
    parser.feed(
        "<action type=\"tool\" name=\"list\" id=\"s1\" mode=\"sync\">{\"path\":\".\"}</action>",
        false);
    parser.feed(" between ", false);
    parser.feed("<response final=\"true\">After</response>", true);

    bool hasText = false, hasResponse = false;
    for (auto& ev : h.events) {
        if (ev.type == TokenEvent::TEXT) {
            if (ev.content.find("Before") != std::string::npos)
                hasText = true;
        }
        if (ev.type == TokenEvent::RESPONSE)
            hasResponse = true;
    }
    CHECK(hasText, "expected TEXT events between tags");
    CHECK(hasResponse, "expected RESPONSE event");
    PASS();
}

// ═══════════════════════════════════════════════════════════════════════
// PP01 — nested-tag depth: action body contains </action> as a quoted
// string (e.g. agent emitting an example snippet). Current parser does
// plain substring search for </action>, truncating the JSON at the inner
// closer. Expected: outer action's content is the FULL JSON, and the
// dispatched action's parameter survives intact.
// ═══════════════════════════════════════════════════════════════════════
void test_PP01_nested_action_substring() {
    TEST("PP01 nested </action> in JSON body");
    TestHarness h;
    std::string snippetCaptured;
    Parser parser([&](const ParsedAction& a) -> Json::Value {
        if (a.params.isMember("snippet"))
            snippetCaptured = a.params["snippet"].asString();
        Json::Value r;
        r["success"] = true;
        r["id"] = a.id;
        return r;
    });
    parser.onEvent([&](const TokenEvent& ev) { h.onEvent(ev); });

    parser.feed(
        "<action type=\"tool\" name=\"echo\" id=\"e1\" mode=\"sync\">"
        "{\"snippet\": \"<action></action>\"}"
        "</action>",
        true);

    CHECK(!snippetCaptured.empty(), "snippet should be parsed from JSON body");
    CHECK(
        snippetCaptured == "<action></action>",
        (std::string("snippet truncated by inner closer: got '") + snippetCaptured + "'").c_str());
    PASS();
}

// ═══════════════════════════════════════════════════════════════════════
// PP02 — closingTagStart math correctness across response + action paths.
// Pin the boundary: content should end exactly before `</tag>`, no extra
// byte, no missing byte.
// ═══════════════════════════════════════════════════════════════════════
void test_PP02_closing_tag_boundary() {
    TEST("PP02 closing-tag boundary exact");
    TestHarness h;
    Parser parser([&](const ParsedAction& a) { return h.executeAction(a); });
    parser.onEvent([&](const TokenEvent& ev) { h.onEvent(ev); });

    parser.feed("<response final=\"true\">EXACT_CONTENT_NO_EXTRA</response>", true);

    CHECK(h.responseText == "EXACT_CONTENT_NO_EXTRA",
          (std::string("response truncated/extended: '") + h.responseText + "'").c_str());
    PASS();
}

// ═══════════════════════════════════════════════════════════════════════
// PP03 — injectResult must not self-deadlock. Run with a timeout watchdog.
// ═══════════════════════════════════════════════════════════════════════
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
void test_PP03_injectResult_no_deadlock() {
    TEST("PP03 injectResult does not deadlock");
    // Heap-allocate parser so we can leak it cleanly if the worker deadlocks.
    auto parser = std::make_shared<Parser>([](const ParsedAction& a) -> Json::Value {
        Json::Value r;
        r["success"] = true;
        r["id"] = a.id;
        return r;
    });

    // Seed a pending action so dispatchPending() walks the queue.
    parser->feed(
        "<action type=\"tool\" name=\"x\" id=\"a1\" mode=\"sync\" depends_on=\"injected\">"
        "{}</action>",
        false);

    std::atomic<bool> done{false};
    std::thread worker([parser, &done] {
        Json::Value v;
        v["success"] = true;
        parser->injectResult("injected", v);
        done.store(true);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && !done.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    if (!done.load()) {
        worker.detach();  // leak; otherwise we'd block program exit
        FAIL("injectResult deadlocked (timed out after 2s)");
        return;
    }
    worker.join();
    PASS();
}

// ═══════════════════════════════════════════════════════════════════════
// PP04 — async actions must not race on shared maps. Run many async
// actions; expect no crash and all results recorded.
// ═══════════════════════════════════════════════════════════════════════
void test_PP04_async_race_smoke() {
    TEST("PP04 async actions don't crash under load");
    std::atomic<int> count{0};
    Parser parser([&](const ParsedAction& a) -> Json::Value {
        count++;
        Json::Value r;
        r["success"] = true;
        r["id"] = a.id;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return r;
    });

    for (int i = 0; i < 50; ++i) {
        std::string id = "a" + std::to_string(i);
        std::string xml =
            "<action type=\"tool\" name=\"x\" id=\"" + id + "\" mode=\"async\">{}</action>";
        parser.feed(xml, false);
    }
    parser.feed("", true);
    parser.waitForActions(std::chrono::seconds(10));

    CHECK(count.load() == 50,
          (std::string("expected 50 executions, got ") + std::to_string(count.load())).c_str());
    PASS();
}

// ═══════════════════════════════════════════════════════════════════════
// PP05 — unresolved ${id.path} refs must be preserved, not collapsed to empty.
// Runtime-level expansion may resolve script-tool output fields later.
// ═══════════════════════════════════════════════════════════════════════
void test_PP05_unresolved_refs_preserved() {
    TEST("PP05 unresolved refs preserved");
    std::string captured;
    Parser parser([&](const ParsedAction& a) -> Json::Value {
        if (a.params.isMember("sources"))
            captured = a.params["sources"].asString();
        Json::Value r;
        r["success"] = true;
        r["id"] = a.id;
        return r;
    });

    parser.feed(
        "<action type=\"tool\" name=\"eval\" id=\"e1\" mode=\"sync\">"
        "{\"sources\":\"${s1.hits}\"}"
        "</action>",
        true);

    CHECK(captured == "${s1.hits}",
          (std::string("unresolved ref was not preserved: '") + captured + "'").c_str());
    PASS();
}

// ═══════════════════════════════════════════════════════════════════════
// PP06 — action attrs become params and text body is preserved.
// ═══════════════════════════════════════════════════════════════════════
void test_PP06_action_attrs_and_text_body() {
    TEST("PP06 action attrs + text body preserved");
    TestHarness h;
    Parser parser([&](const ParsedAction& a) { return h.executeAction(a); });
    parser.onEvent([&](const TokenEvent& ev) { h.onEvent(ev); });

    parser.feed(
        "<action type=\"tool\" name=\"simple_fs_write\" id=\"w1\" path=\"tmp_2.py\" "
        "append=\"false\" offset=\"1\" mode=\"sync\">hello world</action>",
        true);

    CHECK(!h.actions.empty(), "expected captured action");
    const auto& a = h.actions.front();
    CHECK(a.name == "simple_fs_write", "expected simple_fs_write action");
    CHECK(a.id == "w1", "expected id w1");
    CHECK(a.params.isObject(), "expected attrs as params object");
    CHECK(a.params["path"].asString() == "tmp_2.py", "expected path attr in params");
    CHECK(a.params["append"].isBool() && !a.params["append"].asBool(),
          "expected bool attr coercion");
    CHECK(a.params["offset"].isInt() && a.params["offset"].asInt() == 1,
          "expected int attr coercion");
    CHECK(a.content == "hello world", "expected text body in content");
    PASS();
}

// ═══════════════════════════════════════════════════════════════════════
// Test: LLM-emitted result tags are ignored
// ═══════════════════════════════════════════════════════════════════════
void test_model_result_tags_ignored() {
    TEST("model-emitted result tags ignored");
    TestHarness h;
    Parser parser([&](const ParsedAction& a) { return h.executeAction(a); });
    parser.onEvent([&](const TokenEvent& ev) { h.onEvent(ev); });

    parser.feed("<result id=\"fake\" status=\"ok\">{\"success\":true}</result>", false);
    parser.feed("<response final=\"true\">done</response>", true);

    CHECK(parser.getResult("fake").isNull(), "LLM-emitted <result> must not enter result map");
    CHECK(h.responseFinal, "expected final response");
    CHECK(h.responseText == "done", "expected response text");
    PASS();
}

// ═══════════════════════════════════════════════════════════════════════
// main
// ═══════════════════════════════════════════════════════════════════════

void test_provisional_action_on_open_tag() {
    // Opening <action ...> should emit ACTION_START before </action> arrives
    // so the UI can paint the card while the body streams.
    TEST("provisional ACTION_START on open tag before body closes");
    TestHarness h;
    Parser parser([&](const ParsedAction& a) { return h.executeAction(a); });
    parser.onEvent([&](const TokenEvent& ev) { h.onEvent(ev); });

    parser.feed("<action type=\"tool\" name=\"exec\" id=\"e1\">", false);
    bool sawProvisional = false;
    for (const auto& ev : h.events) {
        if (ev.type == TokenEvent::ACTION_START && ev.action && ev.action->id == "e1" &&
            ev.metadata.count("provisional") && ev.metadata.at("provisional") == "true")
            sawProvisional = true;
    }
    CHECK(sawProvisional, "expected provisional ACTION_START before body close");

    size_t n = h.events.size();
    parser.feed("{\"command\":\"true\"}</action>", true);
    bool sawFinal = false;
    for (size_t i = n; i < h.events.size(); ++i) {
        if (h.events[i].type == TokenEvent::ACTION_START && h.events[i].action &&
            h.events[i].action->id == "e1")
            sawFinal = true;
    }
    CHECK(sawFinal, "expected final ACTION_START after body close (same id)");
    CHECK(!h.actions.empty(), "executor runs once on full close, not provisional");
    PASS();
}

int main() {
    std::cout.setf(std::ios::unitbuf);
    std::cout << "\n╔══════════════════════════════════════════╗\n";
    std::cout << "║   MK3 Parser Unit Tests                  ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n\n";

    test_simple_text();
    test_response_single_token();
    test_response_streaming();
    test_response_literal_protocol_tags();
    test_action_single_token();
    test_action_streaming();
    test_action_then_response();
    test_thought_tag();
    test_thinking_tag_alias();
    test_thought_streams_accumulate_live();
    test_variable_resolution();
    test_text_between_tags();
    test_PP01_nested_action_substring();
    test_PP02_closing_tag_boundary();
    test_PP03_injectResult_no_deadlock();
    test_PP04_async_race_smoke();
    test_PP05_unresolved_refs_preserved();
    test_PP06_action_attrs_and_text_body();
    test_model_result_tags_ignored();
    test_clear_results_keeps_used_ids();
    test_duplicate_id_no_retained_rejects();
    test_invalid_json_body_fail_closed();
    test_hollow_object_then_full_body();
    test_depends_on_async_rejected();
    test_provisional_action_on_open_tag();

    std::cout << "\n──────────────────────────────────────────\n";
    std::cout << "  " << passed << " passed, " << failed << " failed\n";
    std::cout << "──────────────────────────────────────────\n";

    return failed > 0 ? 1 : 0;
}
