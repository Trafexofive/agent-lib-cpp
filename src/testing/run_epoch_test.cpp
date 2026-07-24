// Regression: a continuation prompt's RESPONSE stream must NOT bleed
// into the previous turn's finalized THOUGHT/RESPONSE block.
//
// Live bug (commit ea82265 reset tcache, not this): protocolEvents_
// was preserved across runLoop invocations on the same Agent and the
// `protocolEvents_.back().text += ev.content` merge logic tapped onto
// stale events from prior turns. Subsequent tokens rendered inside the
// PREVIOUS turn's thought row.
//
// The script queues two responses so the SAME agent runs both prompts
// against the same ScriptedProvider — the proper continuation shape.

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "src/core/agent.hpp"
#include "src/session/manager.hpp"
#include "src/testing/scripted_provider.hpp"

using namespace cortex::mk3;
using namespace cortex::mk3::testing;
namespace fs = std::filesystem;

static int g_fail = 0;

#define CHECK_OK(c, m)                                                         \
    do {                                                                       \
        if (!(c)) {                                                            \
            std::cerr << "  FAIL: " << m << "\n";                              \
            ++g_fail;                                                          \
        } else {                                                               \
            std::cout << "PASS " << m << "\n";                                 \
        }                                                                      \
    } while (0)

// Identify run-2 events: they must be the LAST RESPONSE + last THOUGHT
// in protocolEvents_, and the response text must be PURELY turn-2.
static bool run2ResponseIsPure(const Agent& a,
                               const std::string& t1Body,
                               const std::string& t2Body) {
    const auto& ev = a.protocolEvents();
    if (ev.empty()) return false;
    const ProtocolEvent& last = ev.back();
    if (last.kind != ProtocolEventKind::RESPONSE) return false;
    if (last.text.find(t2Body) == std::string::npos) return false;
    if (last.text.find(t1Body) != std::string::npos) return false;
    return true;
}

int main() {
    std::cout << "run-epoch stream isolation…\n";
    char tmpl[] = "/tmp/cortexmk3-epochXXXXXX";
    char* dir = ::mkdtemp(tmpl);
    assert(dir);
    auto prev = fs::current_path();
    fs::current_path(dir);

    AgentConfig acfg;
    acfg.name = "epoch-agent";
    acfg.provider = "scripted";
    acfg.model = "scripted-1";
    acfg.runtimeMode = "small";
    acfg.completionPolicy = "promote";

    const std::string t1Body = "hello-from-turn-1-UNIQUE";
    const std::string t2Body = "ack-from-turn-2-UNIQUE";
    // Use raw string literals to avoid the</response> close tag being
    // lexed as a comparator sequence outside the open quote.
    std::string t1Script =
        R"(<thought>t1-plan</thought><response final="true">)" + t1Body + R"(</response>)";
    std::string t2Script =
        R"(<thought>t2-plan</thought><response final="true">)" + t2Body + R"(</response>)";

    auto provider = std::make_shared<ScriptedProvider>(std::deque<std::string>{
        t1Script,
        t2Script,
    });
    Agent agent(acfg, provider);

    const std::string sid = "epoch-sess-1";
    const std::string user1 = "first turn prompt";
    const std::string user2 = "second turn prompt";

    agent.seedUserPrompt(user1);
    agent.saveSession(sid);

    // Turn 1 (cold prompt — no continuation, protocolEvents_ cleared).
    (void)agent.prompt(user1, sid, /*ephemeral=*/false);

    const size_t afterT1 = agent.protocolEvents().size();
    CHECK_OK(afterT1 >= 2,
             "turn 1 emitted ≥ 2 protocol events");
    CHECK_OK(run2ResponseIsPure(agent, "NON-EXISTENT", t1Body),
             "turn 1's last response is the t1 body");

    // Turn 2: same agent, continuation -> protocolEvents_ not cleared.
    // Without the run-epoch fix the new RESPONSES would have appended to
    // t1's last RESPONSE / THOUGHT event, doubling the body string.
    agent.seedUserPrompt(user2);
    agent.saveSession(sid);

    (void)agent.prompt(user2, sid, /*ephemeral=*/false);

    const size_t afterT2 = agent.protocolEvents().size();
    CHECK_OK(afterT2 > afterT1,
             "turn 2 added new protocol events past t1's set");

    // Critical assertion: the LAST response event of the WHOLE stream
    // (which is the t2 final response) must be t2 body alone — no
    // stitching with t1.
    CHECK_OK(run2ResponseIsPure(agent, t1Body, t2Body),
             "t2 final response is t2 body only (no stream merge into t1)");

    // Run-2 emitted at least one THOUGHT event after run-1's events.
    // Walk protocolEvents_ from afterT1 forward and confirm there's a
    // fresh thought at the start of run 2's tail.
    bool sawFreshThought = false;
    if (afterT2 > afterT1) {
        for (size_t i = afterT1; i < afterT2; ++i) {
            if (agent.protocolEvents()[i].kind == ProtocolEventKind::THOUGHT) {
                sawFreshThought = true;
                break;
            }
        }
    }
    CHECK_OK(sawFreshThought,
             "a fresh THOUGHT was appended for turn 2 after t1 events");

    fs::current_path(prev);
    fs::remove_all(dir);
    std::cout << (g_fail ? "\nFAIL\n" : "\nok\n");
    return g_fail ? 1 : 0;
}
