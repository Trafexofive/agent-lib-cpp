// Completion policy — bare / non-final auto-resume + promote.
#include <iostream>
#include <memory>
#include <string>

#include "src/core/agent.hpp"
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
    bool sawRecovery = false;
    for (const auto& h : agent.history()) {
        if (h.find("[PROTOCOL RECOVERY]") != std::string::npos &&
            h.find("BEGIN SALVAGE") != std::string::npos)
            sawRecovery = true;
    }
    CHECK(sawRecovery, "recovery correction injects full salvage block");
}

void test_autonomous_promotes_after_repeated_bare() {
    // Autonomous promotes after 2 bare recoveries (default promoteAfter=2).
    // Three bare dumps → promote on the 2nd recovery path... actually:
    // recovery 1 after first bare, recovery 2 after second bare → early promote.
    auto sp = std::make_shared<ScriptedProvider>(std::deque<std::string>{
        "Draft answer v1 — still bare.",
        "Draft answer v2 — still bare.",
        "Draft answer v3 — should not be needed.",
    });
    AgentConfig cfg = baseCfg("autonomous");
    cfg.iterationCap = 6;
    Agent agent(cfg, sp);
    std::string out = agent.prompt("go", "", true);
    CHECK(out.find("Draft answer") != std::string::npos,
          "autonomous mode promotes salvaged bare text");
    CHECK(out.find("stopped without emitting") == std::string::npos,
          "promoted path does not return stop banner");
    bool sawPromote = false;
    for (const auto& h : agent.history()) {
        if (h.find("[AUTO-PROMOTED]") != std::string::npos) sawPromote = true;
    }
    CHECK(sawPromote, "history records AUTO-PROMOTED note");
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
    CHECK(out.find("stopped without emitting") != std::string::npos,
          "strict policy keeps stop banner at cap");
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

int main() {
    std::cout << "completion_policy_test\n";
    test_bare_text_recovers_then_final();
    test_autonomous_promotes_after_repeated_bare();
    test_strict_never_promotes_at_cap();
    test_nonfinal_response_body_salvaged();
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
