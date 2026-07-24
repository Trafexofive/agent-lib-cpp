// Regression: collectProtocolChanges must not OOB-write on RETRY rotation.
//
// Live repro was: empty-response retry → protocolEvents_ = [RETRY, …] →
// UI baseline previous was pad-then-clear-then previous[i]=… with size 0 →
// tcache_thread_shutdown / unaligned tcache / SEGV.
//
// This test is pure (no network, no inkcell). If it fails under ASAN or
// aborts, the OOB path is back.

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "src/core/types.hpp"
#include "src/ui/model/protocol_event_diff.hpp"

using namespace cortex::mk3;
using namespace cortex::mk3::ui;

static int passed = 0, failed = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "FAIL: " << (msg) << "\n";                            \
            ++failed;                                                          \
        } else {                                                               \
            std::cout << "PASS " << (msg) << "\n";                             \
            ++passed;                                                          \
        }                                                                      \
    } while (0)

static ProtocolEvent makeRetry(const std::string& text) {
    ProtocolEvent e;
    e.kind = ProtocolEventKind::RETRY;
    e.text = text;
    return e;
}

static ProtocolEvent makeThought(const std::string& text) {
    ProtocolEvent e;
    e.kind = ProtocolEventKind::THOUGHT;
    e.text = text;
    return e;
}

static ProtocolEvent makeStatus(const std::string& text) {
    ProtocolEvent e;
    e.kind = ProtocolEventKind::STATUS;
    e.text = text;
    return e;
}

// Attempt N baseline: several events already mirrored into previous.
// Attempt N+1: agent cleared protocolEvents_ and pushed RETRY then new stream.
// Must not crash; previous ends sized == current; out has RETRY + new rows.
static void test_retry_rotation_no_oob() {
    std::vector<ProtocolEvent> previous = {
        makeThought("attempt-0 thought"),
        makeStatus("attempt-0 status"),
        makeThought("attempt-0 more"),
    };
    std::vector<ProtocolEvent> current = {
        makeRetry("retry 1 / 2"),
        makeThought("attempt-1 thought"),
        makeStatus("attempt-1 status"),
    };
    std::vector<UiEvent> out;
    collectProtocolChanges(out, current, previous);

    CHECK(previous.size() == current.size(),
          "previous sized to current after retry rotation");
    CHECK(!out.empty(), "emits at least the RETRY marker + dirty rows");
    CHECK(previous[0].kind == ProtocolEventKind::RETRY,
          "previous[0] is RETRY after rotation");
    CHECK(previous[1].text == "attempt-1 thought",
          "previous tracks new attempt thought");
}

// No rotation: identical stream → no new UiEvents, previous unchanged size.
static void test_stable_stream_no_emit() {
    std::vector<ProtocolEvent> previous = {makeThought("t"), makeStatus("s")};
    std::vector<ProtocolEvent> current = previous;
    std::vector<UiEvent> out;
    collectProtocolChanges(out, current, previous);
    CHECK(out.empty(), "identical baseline emits nothing");
    CHECK(previous.size() == 2, "previous size stable on no-op");
}

// Growth without retry: append one event → one emit, previous grows by 1.
static void test_append_grows_safely() {
    std::vector<ProtocolEvent> previous = {makeThought("t0")};
    std::vector<ProtocolEvent> current = {makeThought("t0"), makeThought("t1")};
    std::vector<UiEvent> out;
    collectProtocolChanges(out, current, previous);
    CHECK(out.size() == 1, "one dirty append emits one event");
    CHECK(previous.size() == 2, "previous grew via push_back not OOB assign");
    CHECK(previous[1].text == "t1", "appended text preserved");
}

// Shrink: previous longer than current → truncate, no pad.
static void test_truncate_on_shrink() {
    std::vector<ProtocolEvent> previous = {
        makeThought("a"), makeThought("b"), makeThought("c"),
    };
    std::vector<ProtocolEvent> current = {makeThought("a")};
    std::vector<UiEvent> out;
    collectProtocolChanges(out, current, previous);
    CHECK(previous.size() == 1, "previous truncated to current size");
    CHECK(out.empty(), "identical head after truncate emits nothing");
}

// Empty → non-empty after clear (first token of a brand-new turn).
static void test_empty_to_first_event() {
    std::vector<ProtocolEvent> previous;
    std::vector<ProtocolEvent> current = {makeThought("first")};
    std::vector<UiEvent> out;
    collectProtocolChanges(out, current, previous);
    CHECK(out.size() == 1, "first event from empty baseline emits");
    CHECK(previous.size() == 1, "previous grew from empty via push_back");
}

int main() {
    std::cout << "protocol_event_diff regression…\n";
    test_retry_rotation_no_oob();
    test_stable_stream_no_emit();
    test_append_grows_safely();
    test_truncate_on_shrink();
    test_empty_to_first_event();
    std::cout << "──────────────────────────────────────────\n";
    std::cout << "  " << passed << " passed, " << failed << " failed\n";
    std::cout << "──────────────────────────────────────────\n";
    return failed ? 1 : 0;
}
