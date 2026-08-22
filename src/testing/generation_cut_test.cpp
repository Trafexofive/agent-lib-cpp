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

// Live plate: 39s thinking, open=0, same "I'll scout… list path… grep path"
// growing as native thinking. No <action> yet. Must cut, not bill a furnace.
class PreActionToolPlanProvider : public ILlmProvider {
   public:
    int calls = 0;
    size_t thinkBytes = 0;
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
            const std::string plan =
                "I'll scout the workflow page and manifests quickly. "
                "list path /home/mlamkadm/repos/active/agent-lib-cpp max_entries 80 "
                "list path /home/mlamkadm/repos/active/agent-lib-cpp/manifests "
                "recursive true grep path /tmp pattern workflow path_glob *.md "
                "max_matches 40 git_status cwd /tmp fs_read path /tmp/PLANNED.md ";
            for (int i = 0; i < 40; ++i) {
                std::string chunk = std::string(1, '\x01') + plan;
                thinkBytes += plan.size();
                if (abortGeneration_.load(std::memory_order_acquire)) {
                    cutSeen = true;
                    lastStats_.finishReason = "generation_cut";
                    break;
                }
                cb(chunk, false);
            }
            return;
        }
        cb("<action type=\"tool\" name=\"echo\" id=\"e1\">{\"msg\":\"ok\"}</action>",
           false);
        cb("<response final=\"true\">plan delivered</response>", true);
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
    std::string providerName() const override { return "pre-action-plan"; }
   private:
    std::string model_ = "drip";
    double temperature_ = 0.7;
    int maxTokens_ = 4096;
    double topP_ = 0.95;
    StreamStats lastStats_{};
    std::atomic<bool> abortGeneration_{false};
};

void test_pre_action_tool_plan_thinking_is_cut() {
    auto sp = std::make_shared<PreActionToolPlanProvider>();
    AgentConfig cfg;
    cfg.name = "generation-cut-preaction";
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
    std::string out = agent.prompt("scout", "", true);
    CHECK(sp->cutSeen, "pre-action tool-plan thinking is cut");
    CHECK(sp->thinkBytes < 20000, "pre-action furnace cut before 20KB");
    CHECK(sp->calls >= 2, "ReAct still iterates after pre-action cut");
    CHECK(out.find("plan delivered") != std::string::npos,
          "next iter still delivers after pre-action cut");
    bool dashed = false, sawScout = false, sawListPath = false, sawHarness = false;
    for (const auto& pe : agent.protocolEvents()) {
        if (pe.kind == ProtocolEventKind::THOUGHT) {
            if (pe.text.find("\n\n---\n\n") != std::string::npos) dashed = true;
            if (pe.text.find("I'll scout") != std::string::npos) sawScout = true;
            if (pe.text.find("list path") != std::string::npos) sawListPath = true;
        }
        if (pe.kind == ProtocolEventKind::STATUS &&
            pe.text.find("TOOL_PLAN") != std::string::npos)
            sawHarness = true;
    }
    bool harnessInHistory = false;
    for (const auto& h : agent.history())
        if (h.find("code=\"TOOL_PLAN\"") != std::string::npos) harnessInHistory = true;
    CHECK(!dashed, "thought well has no --- stack");
    CHECK(sawScout, "scout sentence stays in the well");
    CHECK(!sawListPath, "dictated tool calls are not in the well");
    CHECK(sawHarness && harnessInHistory,
          "TOOL_PLAN harness injected — next emit must be <action>");
}

int main() {
    std::cout << "generation_cut_test\n";
    test_leftover_thinking_after_action_is_cut();
    test_hollow_unclosed_action_then_thinking_is_cut();
    test_sibling_action_with_body_still_runs();
    test_pre_action_tool_plan_thinking_is_cut();
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
