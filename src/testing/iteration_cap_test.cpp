// =============================================================================
// Iteration cap → <harness> tag (LLM) + [LIMIT] block (TUI) test.
//
// Drives an agent past its max_iterations cap with a provider that never
// emits final="true", then asserts:
//   1. history_ gained a runtime <harness limit="max_iterations=N"> tag.
//   2. The finalization-turn prompt carries status="finalization".
//   3. protocolEvents_ gained a [LIMIT] Status block (the TUI block).
//
// No network. Deterministic.
// =============================================================================

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "src/core/agent.hpp"
#include "src/core/types.hpp"

using namespace cortex::mk3;

static int passed = 0, failed = 0;

#define CHECK(cond, msg)              \
    do {                              \
        if (!(cond)) {                \
            failed++;                 \
            std::cout << "FAIL: " << msg << "\n"; \
            return;                   \
        }                             \
    } while (0)

// Provider that never emits final="true" — keeps the loop running until the
// iteration cap forces the FINALIZATION turn. Captures the last user-role
// message so we can assert the <harness status="finalization"> prompt.
class NonFinalizingProvider : public ILlmProvider {
   public:
    int calls = 0;
    std::string lastUserMsg;

    std::string generate(const ChatMessages& msgs) override { return next(msgs); }
    void generateStream(const ChatMessages& msgs, StreamCallback cb) override {
        lastStats_ = StreamStats{};
        lastStats_.anyContent = true;
        lastStats_.finishReason = "stop";
        cb(next(msgs), true);
    }
    StreamStats lastStreamStats() const override { return lastStats_; }

    std::string next(const ChatMessages& msgs) {
        calls++;
        for (const auto& m : msgs)
            if (m.role == ChatRole::USER) lastUserMsg = m.content;
        return "<action type=\"tool\" name=\"list\" id=\"cap" +
               std::to_string(calls) + "\">{\"path\":\".\"}</action>";
    }

    void setModel(const std::string& m) override { model_ = m; }
    void setTemperature(double t) override { temperature_ = t; }
    void setMaxTokens(int n) override { maxTokens_ = n; }
    void setTopP(double p) override { topP_ = p; }
    std::string getModel() const override { return model_; }
    double getTemperature() const override { return temperature_; }
    int getMaxTokens() const override { return maxTokens_; }
    std::vector<ModelInfo> listModels() override { return {}; }
    std::string providerName() const override { return "nonfinal"; }

   private:
    std::string model_ = "nonfinal";
    double temperature_ = 0.7;
    int maxTokens_ = 4096;
    double topP_ = 0.95;
    StreamStats lastStats_{};
};

void test_harness_tag_and_limit_block() {
    std::cout << "  iteration cap -> <harness> tag + [LIMIT] TUI block... ";
    AgentConfig cfg;
    cfg.name = "cap-test";
    cfg.iterationCap = 2;

    auto provider = std::make_shared<NonFinalizingProvider>();
    Agent agent(cfg, provider);
    agent.prompt("keep working until capped");

    // 1. LLM side: runtime <harness limit="max_iterations=2"> in history_.
    bool inHistory = false;
    for (const auto& h : agent.history())
        if (h.find("<harness limit=\"max_iterations=2\">") != std::string::npos)
            inHistory = true;
    CHECK(inHistory, "history_ should contain <harness limit=\"max_iterations=2\">");

    // 2. Finalization-turn prompt is a <harness status="finalization"> tag.
    CHECK(provider->lastUserMsg.find("status=\"finalization\"") != std::string::npos,
          "finalization prompt should carry status=\"finalization\"");

    // 3. TUI side: a [LIMIT] Status block in protocolEvents_.
    bool inEvents = false;
    for (const auto& ev : agent.protocolEvents())
        if (ev.kind == ProtocolEventKind::STATUS &&
            ev.text.find("[LIMIT]") != std::string::npos)
            inEvents = true;
    CHECK(inEvents, "protocolEvents_ should contain a [LIMIT] Status block");

    // 4. The loop actually ran work turns + one finalization turn.
    CHECK(provider->calls >= 3,
          "expected >= 3 model calls (2 work + 1 finalization)");

    passed++;
    std::cout << "PASS\n";
}

int main() {
    test_harness_tag_and_limit_block();
    std::cout << (failed == 0 ? "ALL PASS" : "SOME FAIL") << " (" << passed
              << " passed, " << failed << " failed)\n";
    return failed == 0 ? 0 : 1;
}
