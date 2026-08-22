// Completion policy — bare / non-final auto-resume + promote.
#include <iostream>
#include <memory>
#include <string>

#include "src/core/agent.hpp"
#include "src/core/agent_run_helpers.hpp"
#include "src/core/types.hpp"
#include "src/testing/scripted_provider.hpp"

using namespace cortex::mk3;
using namespace cortex::mk3::testing;

static int passed = 0, failed = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        std::cout << "  " << (msg) << "... ";                                  \
        if (cond) {                                                            \
            ++passed;                                                          \
            std::cout << "PASS\n";                                             \
        } else {                                                               \
            ++failed;                                                          \
            std::cout << "FAIL\n";                                             \
        }                                                                      \
    } while (0)

static AgentConfig baseCfg(const std::string& mode, const std::string& policy = "") {
    AgentConfig cfg;
    cfg.name = "completion-policy-test";
    cfg.provider = "scripted";
    cfg.model = "scripted";
    cfg.iterationCap = 4;
    cfg.runtimeMode = mode;
    cfg.completionPolicy = policy;
    cfg.systemPromptText = "test harness";
    return cfg;
}

void test_bare_text_recovers_then_final() {
    // Iter1: bare prose. Iter2: proper final wrapping salvage intent.
    auto sp = std::make_shared<ScriptedProvider>(std::deque<std::string>{
        "Here is the answer without tags.",
        "<response final=\"true\">Here is the answer without tags.</response>",
    });
    Agent agent(baseCfg("normal"), sp);
    std::string out = agent.prompt("go", /*sessionId=*/"", /*ephemeral=*/true);
    CHECK(out.find("Here is the answer without tags.") != std::string::npos,
          "bare text recovers and final response is returned");
    bool sawHarness = false, sawNonFinal = false;
    for (const auto& h : agent.history()) {
        if (h.find("code=\"BARE_TEXT\"") != std::string::npos) sawHarness = true;
        if (h.find("<response>") != std::string::npos &&
            h.find("final=\"true\"") == std::string::npos &&
            h.find("Here is the answer without tags.") != std::string::npos)
            sawNonFinal = true;
    }
    CHECK(sawHarness, "harness BARE_TEXT injected");
    CHECK(!sawNonFinal, "untagged text is NOT minted as <response>");
}

void test_autonomous_bare_continues_nonfinal() {
    // One BARE_TEXT, one recovery gen. Second thought-only → THOUGHT_STALL.
    auto sp = std::make_shared<ScriptedProvider>(std::deque<std::string>{
        "Draft answer v1 — still bare.",
        "Draft answer v2 — still bare.",
        "<response final=\"true\">should not be reached</response>",
    });
    AgentConfig cfg = baseCfg("autonomous");
    cfg.iterationCap = 6;
    Agent agent(cfg, sp);
    std::string out = agent.prompt("go", "", true);
    CHECK(out.find("THOUGHT_STALL") != std::string::npos,
          "second thought-only gen stalls instead of burning cap");
    CHECK(out.find("should not be reached") == std::string::npos,
          "stall does not consume a third generation");
    int bareN = 0;
    bool earlyFinal = false;
    for (const auto& h : agent.history()) {
        if (h.find("code=\"BARE_TEXT\"") != std::string::npos) ++bareN;
        if (h.find("[AUTO-PROMOTED]") != std::string::npos &&
            h.find("@ CAP") == std::string::npos)
            earlyFinal = true;
    }
    CHECK(bareN >= 1, "BARE_TEXT harness injected once (not spammed)");
    CHECK(!earlyFinal, "no mid-loop AUTO-PROMOTED final");
}

void test_strict_never_promotes_at_cap() {
    auto sp = std::make_shared<ScriptedProvider>(std::deque<std::string>{
        "orphan prose only",
        "orphan prose only again",
        "still orphan",
        "still orphan at cap",
    });
    AgentConfig cfg = baseCfg("normal", "strict");
    cfg.iterationCap = 3;
    Agent agent(cfg, sp);
    std::string out = agent.prompt("go", "", true);
    const bool stopped =
        out.find("stopped without emitting") != std::string::npos ||
        out.find("THOUGHT-ONLY HARD STOP") != std::string::npos ||
        out.find("THOUGHT_STALL") != std::string::npos;
    CHECK(stopped, "strict policy does not treat bare text as a final answer");
    CHECK(out.find("orphan prose") == std::string::npos || stopped,
          "strict does not return salvage as a successful completion");
}

void test_max_iter_runs_finalization_turn() {
    // workCap=2 bare turns → finalization turn #3 must be offered and may close.
    auto sp = std::make_shared<ScriptedProvider>(std::deque<std::string>{
        "still working bare 1",
        "still working bare 2",
        "<response final=\"true\">closed on finalization</response>",
    });
    AgentConfig cfg = baseCfg("normal", "recover");
    cfg.iterationCap = 2;
    Agent agent(cfg, sp);
    std::string out = agent.prompt("go", "", true);
    CHECK(out.find("closed on finalization") != std::string::npos,
          "max_iterations triggers finalization turn that can close cleanly");
    bool sawLimit = false, sawFinalize = false;
    for (const auto& h : agent.history()) {
        if (h.find("[LIMIT]") != std::string::npos) sawLimit = true;
        if (h.find("[FINALIZE]") != std::string::npos ||
            h.find("[FINALIZATION TURN]") != std::string::npos)
            sawFinalize = true;
    }
    // Also check protocol STATUS events.
    for (const auto& pe : agent.protocolEvents()) {
        if (pe.kind == ProtocolEventKind::STATUS) {
            if (pe.text.find("[LIMIT]") != std::string::npos) sawLimit = true;
            if (pe.text.find("[FINALIZE]") != std::string::npos) sawFinalize = true;
        }
    }
    for (const auto& h : agent.history()) {
        if (h.find("status=\"finalization\"") != std::string::npos ||
            h.find("FINALIZATION") != std::string::npos)
            sawFinalize = true;
        if (h.find("kind=\"limit\"") != std::string::npos ||
            h.find("iteration budget exhausted") != std::string::npos)
            sawLimit = true;
    }
    CHECK(sawLimit, "limit harness is recorded when work budget exhausts");
    CHECK(sawFinalize, "finalization harness is recorded for the extra turn");
}

void test_nonfinal_response_body_salvaged() {
    // <response> without final=true still yields body the recovery can re-home.
    auto sp = std::make_shared<ScriptedProvider>(std::deque<std::string>{
        "<response>partial answer body</response>",
        "<response final=\"true\">partial answer body</response>",
    });
    Agent agent(baseCfg("normal"), sp);
    std::string out = agent.prompt("go", "", true);
    CHECK(out.find("partial answer body") != std::string::npos,
          "non-final response body is salvaged into a real final");
}

void test_decide_transport_catch() {
    using SK = RunStopKind;
    auto d = decideTransportCatch(TransportClass::HardExternal, SK::ExternalSignal,
                                  true, 0, 9, false, true);
    CHECK(d.action == CatchAction::HardTimeout, "SIGINT/SIGTERM is HardTimeout");
    d = decideTransportCatch(TransportClass::HardOperator, SK::Operator, false, 0,
                             9, false, true);
    CHECK(d.action == CatchAction::HardCancel, "Ctrl-X is HardCancel");
    d = decideTransportCatch(TransportClass::StreamAbort, SK::None, true, 0, 9,
                             false, true);
    CHECK(d.action == CatchAction::RetryPrimary && d.clearSoft,
          "stream abort retries primary and clears soft");
    d = decideTransportCatch(TransportClass::StreamAbort, SK::None, true, 8, 9,
                             false, true);
    CHECK(d.action == CatchAction::Fallback, "exhausted primary + fallback → Fallback");
    d = decideTransportCatch(TransportClass::RegionForbidden, SK::None, true, 0, 9,
                             false, true);
    CHECK(d.action == CatchAction::Fallback, "403 region skips retries, goes Fallback");
    d = decideTransportCatch(TransportClass::Fatal, SK::None, true, 0, 9, false,
                             true);
    CHECK(d.action == CatchAction::TerminalFail, "fatal is TerminalFail");
    d = decideTransportCatch(TransportClass::TransientNet, SK::None, true, 8, 9,
                             true, true);
    CHECK(d.action == CatchAction::TerminalFail,
          "exhausted + fallback already tried → TerminalFail");
}

int main() {
    std::cout << "completion_policy_test\n";
    test_bare_text_recovers_then_final();
    test_autonomous_bare_continues_nonfinal();
    test_strict_never_promotes_at_cap();
    test_max_iter_runs_finalization_turn();
    test_nonfinal_response_body_salvaged();
    test_decide_transport_catch();
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
