#pragma once
// =============================================================================
// ScriptedProvider — queue-driven ILlmProvider fake for tests.
//
// The existing NoopProvider in session_test.cpp always returns a hardcoded
// "<response final=\"true\">OK</response>", which makes it impossible to
// exercise any code path that depends on the LLM emitting something other
// than a final response — i.e. <action type="agent">, <action type="tool">,
// multi-turn, error/refusal, streaming chunk boundaries.
//
// ScriptedProvider takes a queue of complete responses. Each call to
// generate() / generateStream() pops the front response and delivers it.
// Streaming delivers the whole response as a single chunk with isFinal=true
// (deterministic, parser-stable). An empty queue throws — loud failure.
//
// Usage:
//   ScriptedProvider p({"<response final=\"true\">R1</response>",
//                      "<action type=\"agent\" name=\"reader\" id=\"a1\">go</action>",
//                      "<response final=\"true\">R2</response>"});
//   Agent parent(parentCfg, std::make_shared<ScriptedProvider>(std::move(p)));
//   Agent child(childCfg,  std::make_shared<NoopProvider>());
//   parent.addSubAgent(std::make_shared<Agent>(...));
//   parent.prompt("test");  // first emit: R1; child invoked; child returns OK;
//                           // parent resumes and emits R2.
//
// This is the enabler for the sub-agent test phase and all protocol-driven
// integration tests. Test-only — never used in production code.
// =============================================================================

#include <atomic>
#include <deque>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "src/core/provider.hpp"
#include "src/core/types.hpp"

namespace cortex::mk3::testing {

class ScriptedProvider : public ILlmProvider {
   public:
    ScriptedProvider() = default;
    explicit ScriptedProvider(std::deque<std::string> script)
        : script_(std::move(script)) {}

    // Non-streaming: returns the front response, pops it.
    std::string generate(const ChatMessages& /*msgs*/) override {
        popOrThrow();
        std::string r = std::move(script_.front());
        script_.pop_front();
        return r;
    }

    // Streaming: delivers the whole response as one chunk with isFinal=true.
    // Deterministic — keeps the protocol parser and incremental wrap cache
    // on the well-trodden single-chunk path.
    void generateStream(const ChatMessages& /*msgs*/, StreamCallback cb) override {
        if (!cb) throw std::runtime_error("ScriptedProvider: null stream callback");
        abortGeneration_.store(false, std::memory_order_release);
        popOrThrow();
        std::string r = std::move(script_.front());
        script_.pop_front();
        lastStats_ = StreamStats{};
        lastStats_.anyContent = !r.empty();
        lastStats_.finishReason = "stop";
        cb(r, true);
    }

    StreamStats lastStreamStats() const override { return lastStats_; }
    void abortGeneration() override {
        abortGeneration_.store(true, std::memory_order_release);
    }
    bool generationAborted() const override {
        return abortGeneration_.load(std::memory_order_acquire);
    }

    // Configuration — pass-through; tests don't tune these.
    void setModel(const std::string& model) override { model_ = model; }
    void setTemperature(double t) override { temperature_ = t; }
    void setMaxTokens(int n) override { maxTokens_ = n; }
    void setTopP(double p) override { topP_ = p; }

    // Getters.
    std::string getModel() const override { return model_; }
    double getTemperature() const override { return temperature_; }
    int getMaxTokens() const override { return maxTokens_; }
    std::string providerName() const override { return "scripted"; }

    // Test introspection.
    size_t remaining() const { return script_.size(); }
    bool exhausted() const { return script_.empty(); }

   private:
    void popOrThrow() {
        if (script_.empty()) {
            throw std::runtime_error(
                "ScriptedProvider: script exhausted (no more responses queued)");
        }
    }

    std::deque<std::string> script_;
    std::atomic<bool> abortGeneration_{false};
    std::string model_ = "scripted-model";
    double temperature_ = 0.7;
    int maxTokens_ = 4096;
    double topP_ = 0.95;
    StreamStats lastStats_;
};

}  // namespace cortex::mk3::testing
