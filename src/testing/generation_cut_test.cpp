// After a completed action batch, leftover native thinking on the same
// generateStream must abort. Live hang: grok billed 67kB after list/grep
// results with open=0. This test FAILS if abortGeneration is never called.
#include <atomic>
#include <iostream>
#include <memory>
#include <string>

#include "src/core/agent.hpp"
#include "src/core/types.hpp"
#include "src/tools/tool.hpp"

using namespace cortex::mk3;

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

class DripAfterActionProvider : public ILlmProvider {
   public:
    int calls = 0;
    size_t leftoverThinking = 0;
    bool cutSeen = false;

    std::string generate(const ChatMessages&) override {
        return "<response final=\"true\">unused</response>";
    }
    void generateStream(const ChatMessages&, StreamCallback cb) override {
        abortGeneration_.store(false, std::memory_order_release);
        lastStats_ = StreamStats{};
        lastStats_.anyContent = true;
        lastStats_.finishReason = "stop";
        ++calls;
        if (!cb) return;
        if (calls == 1) {
            cb("<action type=\"tool\" name=\"echo\" id=\"e1\">{\"msg\":\"ok\"}</action>\n",
               false);
            const std::string chunk = std::string(1, '\x01') + std::string(256, 'x');
            for (int i = 0; i < 80; ++i) {
                leftoverThinking += 256;
                if (abortGeneration_.load(std::memory_order_acquire)) {
                    cutSeen = true;
                    lastStats_.finishReason = "generation_cut";
                    break;
                }
                cb(chunk, false);
            }
            return;
        }
        cb("<response final=\"true\">scout done</response>", true);
    }
    StreamStats lastStreamStats() const override { return lastStats_; }
    void abortGeneration() override {
        abortGeneration_.store(true, std::memory_order_release);
    }
    bool generationAborted() const override {
        return abortGeneration_.load(std::memory_order_acquire);
    }

    void setModel(const std::string& m) override { model_ = m; }
    void setTemperature(double t) override { temperature_ = t; }
    void setMaxTokens(int n) override { maxTokens_ = n; }
    void setTopP(double p) override { topP_ = p; }
    std::string getModel() const override { return model_; }
    double getTemperature() const override { return temperature_; }
    int getMaxTokens() const override { return maxTokens_; }
    std::string providerName() const override { return "drip-after-action"; }

   private:
    std::string model_ = "drip";
    double temperature_ = 0.7;
    int maxTokens_ = 4096;
    double topP_ = 0.95;
    StreamStats lastStats_{};
    std::atomic<bool> abortGeneration_{false};
};

void test_leftover_thinking_after_action_is_cut() {
    auto sp = std::make_shared<DripAfterActionProvider>();
    AgentConfig cfg;
    cfg.name = "generation-cut-test";
    cfg.provider = "scripted";
    cfg.model = "scripted";
    cfg.iterationCap = 6;
    cfg.runtimeMode = "normal";
    cfg.systemPromptText = "test";
    Agent agent(cfg, sp);

    ToolDef def;
    def.name = "echo";
    def.description = "echo";
    def.isNative = true;
    agent.addTool(tools::Tool(def, [](const Json::Value& p) {
        Json::Value r;
        r["success"] = true;
        r["msg"] = p.get("msg", "").asString();
        Json::StreamWriterBuilder w;
        w["indentation"] = "";
        return Json::writeString(w, r);
    }));

    std::string out = agent.prompt("scout", "", true);
    CHECK(sp->cutSeen, "abortGeneration fired during leftover thinking drip");
    CHECK(sp->leftoverThinking < 8192,
          "leftover thinking cut before 8KB (not the full 20KB drip)");
    CHECK(sp->leftoverThinking >= 4096,
          "cut waits ~4KB so sibling actions in the same emit still land");
    CHECK(out.find("scout done") != std::string::npos,
          "next ReAct iter still emits the final after the cut");
    CHECK(sp->calls >= 2, "second generateStream ran after the cut");
}

// Dump 1787415370663: closed list, then hollow unclosed <action fs_read>, then
// 490kB native thinking. generationSettled used to stay false on the hollow
// tag so the cut never fired. ReAct must still iterate.
class DumpHollowThenThinkProvider : public ILlmProvider {
   public:
    int calls = 0;
    size_t leftoverThinking = 0;
    bool cutSeen = false;
    int fsReadCalls = 0;

    std::string generate(const ChatMessages&) override {
        return "<response final=\"true\">unused</response>";
    }
    void generateStream(const ChatMessages&, StreamCallback cb) override {
        abortGeneration_.store(false, std::memory_order_release);
        lastStats_ = StreamStats{};
        lastStats_.anyContent = true;
        lastStats_.finishReason = "stop";
        ++calls;
        if (!cb) return;
        if (calls == 1) {
            cb("<action type=\"tool\" name=\"echo\" id=\"e1\">{\"msg\":\"ok\"}</action>\n",
               false);
            cb("<action type=\"tool\" name=\"fs_read\" id=\"read_schema_size\" mode=\"sync\">",
               false);
            const std::string chunk = std::string(1, '\x01') + std::string(256, 'x');
            for (int i = 0; i < 80; ++i) {
                leftoverThinking += 256;
                if (abortGeneration_.load(std::memory_order_acquire)) {
                    cutSeen = true;
                    lastStats_.finishReason = "generation_cut";
                    break;
                }
                cb(chunk, false);
            }
            return;
        }
        cb("<response final=\"true\">scout done</response>", true);
    }
    StreamStats lastStreamStats() const override { return lastStats_; }
    void abortGeneration() override {
        abortGeneration_.store(true, std::memory_order_release);
    }
    bool generationAborted() const override {
        return abortGeneration_.load(std::memory_order_acquire);
    }
    void setModel(const std::string& m) override { model_ = m; }
    void setTemperature(double t) override { temperature_ = t; }
    void setMaxTokens(int n) override { maxTokens_ = n; }
    void setTopP(double p) override { topP_ = p; }
    std::string getModel() const override { return model_; }
    double getTemperature() const override { return temperature_; }
    int getMaxTokens() const override { return maxTokens_; }
    std::string providerName() const override { return "dump-hollow"; }

   private:
    std::string model_ = "drip";
    double temperature_ = 0.7;
    int maxTokens_ = 4096;
    double topP_ = 0.95;
    StreamStats lastStats_{};
    std::atomic<bool> abortGeneration_{false};
};

void test_hollow_unclosed_action_then_thinking_is_cut() {
    auto sp = std::make_shared<DumpHollowThenThinkProvider>();
    AgentConfig cfg;
    cfg.name = "generation-cut-hollow";
    cfg.provider = "scripted";
    cfg.model = "scripted";
    cfg.iterationCap = 6;
    cfg.runtimeMode = "normal";
    cfg.systemPromptText = "test";
    Agent agent(cfg, sp);

    ToolDef echo;
    echo.name = "echo";
    echo.description = "echo";
    echo.isNative = true;
    agent.addTool(tools::Tool(echo, [](const Json::Value& p) {
        Json::Value r;
        r["success"] = true;
        r["msg"] = p.get("msg", "").asString();
        Json::StreamWriterBuilder w;
        w["indentation"] = "";
        return Json::writeString(w, r);
    }));
    ToolDef rd;
    rd.name = "fs_read";
    rd.description = "read";
    rd.isNative = true;
    agent.addTool(tools::Tool(rd, [sp](const Json::Value&) {
        ++sp->fsReadCalls;
        Json::Value r;
        r["success"] = true;
        r["content"] = "should not run";
        Json::StreamWriterBuilder w;
        w["indentation"] = "";
        return Json::writeString(w, r);
    }));

    std::string out = agent.prompt("scout", "", true);
    CHECK(sp->cutSeen, "dump path: cut fires after hollow unclosed fs_read");
    CHECK(sp->fsReadCalls == 0, "hollow fs_read is not executed");
    CHECK(sp->leftoverThinking < 8192, "dump path: leftover thinking cut before 8KB");
    CHECK(out.find("scout done") != std::string::npos,
          "dump path: ReAct still iterates after the cut");
    bool hollowCard = false;
    for (const auto& pe : agent.protocolEvents()) {
        if (pe.kind == ProtocolEventKind::ACTION && pe.action.id == "read_schema_size" &&
            pe.action.body.empty())
            hollowCard = true;
    }
    CHECK(!hollowCard, "hollow fs_read card is dropped from protocol");
}

// Sibling action with a real body must still execute — do not cut mid-JSON.
class SiblingBodyProvider : public ILlmProvider {
   public:
    int calls = 0;
    int fsReadCalls = 0;
    bool cutSeen = false;
    std::string generate(const ChatMessages&) override {
        return "<response final=\"true\">unused</response>";
    }
    void generateStream(const ChatMessages&, StreamCallback cb) override {
        abortGeneration_.store(false, std::memory_order_release);
        lastStats_ = StreamStats{};
        lastStats_.anyContent = true;
        lastStats_.finishReason = "stop";
        ++calls;
        if (!cb) return;
        if (calls == 1) {
            cb("<action type=\"tool\" name=\"echo\" id=\"e1\">{\"msg\":\"ok\"}</action>", false);
            cb("<action type=\"tool\" name=\"fs_read\" id=\"r1\">", false);
            cb("{\"path\":\"/tmp/x\"}</action>", false);
            return;
        }
        cb("<response final=\"true\">both ran</response>", true);
    }
    StreamStats lastStreamStats() const override { return lastStats_; }
    void abortGeneration() override {
        cutSeen = true;
        abortGeneration_.store(true, std::memory_order_release);
    }
    bool generationAborted() const override {
        return abortGeneration_.load(std::memory_order_acquire);
    }
    void setModel(const std::string& m) override { model_ = m; }
    void setTemperature(double t) override { temperature_ = t; }
    void setMaxTokens(int n) override { maxTokens_ = n; }
    void setTopP(double p) override { topP_ = p; }
    std::string getModel() const override { return model_; }
    double getTemperature() const override { return temperature_; }
    int getMaxTokens() const override { return maxTokens_; }
    std::string providerName() const override { return "sibling-body"; }
   private:
    std::string model_ = "drip";
    double temperature_ = 0.7;
    int maxTokens_ = 4096;
    double topP_ = 0.95;
    StreamStats lastStats_{};
    std::atomic<bool> abortGeneration_{false};
};

void test_sibling_action_with_body_still_runs() {
    auto sp = std::make_shared<SiblingBodyProvider>();
    AgentConfig cfg;
    cfg.name = "generation-cut-sibling";
    cfg.provider = "scripted";
    cfg.model = "scripted";
    cfg.iterationCap = 6;
    cfg.runtimeMode = "normal";
    cfg.systemPromptText = "test";
    Agent agent(cfg, sp);
    ToolDef echo;
    echo.name = "echo";
    echo.description = "echo";
    echo.isNative = true;
    agent.addTool(tools::Tool(echo, [](const Json::Value&) {
        Json::Value r; r["success"] = true;
        Json::StreamWriterBuilder w; w["indentation"] = "";
        return Json::writeString(w, r);
    }));
    ToolDef rd;
    rd.name = "fs_read";
    rd.description = "read";
    rd.isNative = true;
    agent.addTool(tools::Tool(rd, [sp](const Json::Value&) {
        ++sp->fsReadCalls;
        Json::Value r; r["success"] = true; r["content"] = "file";
        Json::StreamWriterBuilder w; w["indentation"] = "";
        return Json::writeString(w, r);
    }));
    std::string out = agent.prompt("scout", "", true);
    CHECK(sp->fsReadCalls == 1, "sibling fs_read with a body still executes");
    CHECK(!sp->cutSeen, "no leftover-thinking cut mid sibling body");
    CHECK(out.find("both ran") != std::string::npos, "ReAct iterates after both tools");
}

int main() {
    std::cout << "generation_cut_test\n";
    test_leftover_thinking_after_action_is_cut();
    test_hollow_unclosed_action_then_thinking_is_cut();
    test_sibling_action_with_body_still_runs();
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
