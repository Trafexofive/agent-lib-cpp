// Session load repair tests:
//   1) empty agent_name/model/provider backfilled from Agent config_
//   2) non-empty identity not clobbered
//   3) records:[] + full .state.json history → loadSession repairs both

#include <unistd.h>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "src/core/agent.hpp"
#include "src/session/manager.hpp"

using namespace cortex::mk3;
namespace fs = std::filesystem;

static int g_fail = 0;

class NoopProvider : public ILlmProvider {
   public:
    std::string generate(const ChatMessages&) override {
        return "<response final=\"true\">OK</response>";
    }
    void generateStream(const ChatMessages&, StreamCallback cb) override {
        cb("<response final=\"true\">OK</response>", true);
    }
    void setModel(const std::string& model) override { model_ = model; }
    void setTemperature(double t) override { temperature_ = t; }
    void setMaxTokens(int n) override { maxTokens_ = n; }
    void setTopP(double p) override { topP_ = p; }
    std::string getModel() const override { return model_; }
    double getTemperature() const override { return temperature_; }
    int getMaxTokens() const override { return maxTokens_; }
    std::vector<ModelInfo> listModels() override { return {}; }
    std::string providerName() const override { return "noop-test"; }

   private:
    std::string model_ = "noop-1";
    double temperature_ = 0.7;
    int maxTokens_ = 65536;
    double topP_ = 0.95;
};

#define CHECK_OK(cond, msg)                                                    \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "  FAIL: " << msg << " at " << __FILE__ << ":"        \
                      << __LINE__ << "\n";                                     \
            ++g_fail;                                                          \
        } else {                                                               \
            std::cout << "PASS " << msg << "\n";                               \
        }                                                                      \
    } while (0)

static fs::path makeTmpCwd() {
    char tmpl[] = "/tmp/cortexmk3-sessXXXXXX";
    char* dir = ::mkdtemp(tmpl);
    assert(dir);
    return fs::path(dir);
}

static void test_identity_backfill(const fs::path& tmp) {
    std::cout << "identity backfill…\n";
    auto prev = fs::current_path();
    fs::current_path(tmp);

    AgentConfig acfg;
    acfg.name = "brainstormer-001";
    acfg.provider = "noop-test";
    acfg.model = "no-op-moap-1";
    Agent agent(acfg, std::make_shared<NoopProvider>());

    Session legacy;
    legacy.id = "legacy-empty";
    legacy.agentName = "";
    legacy.model = "";
    legacy.provider = "";
    legacy.created = session::SessionManager::iso8601();
    legacy.updated = legacy.created;
    legacy.records.push_back(
        {SessionRecord::USER, "preserved legacy prompt", legacy.created, ""});
    session::SessionManager sm;
    sm.save(legacy);

    agent.loadSession("legacy-empty");
    {
        auto loaded = sm.load("legacy-empty");
        CHECK_OK(loaded.agentName == "brainstormer-001",
                 "agentName corrected to config_.name");
        CHECK_OK(loaded.model == "no-op-moap-1", "model corrected");
        CHECK_OK(loaded.provider == "noop-test", "provider corrected");
        CHECK_OK(loaded.records.size() == 1, "records preserved");
    }

    Session identity;
    identity.id = "with-identity";
    identity.agentName = "persisted-writer";
    identity.model = "persisted-moap";
    identity.provider = "persisted-provider";
    identity.created = session::SessionManager::iso8601();
    identity.updated = identity.created;
    sm.save(identity);
    agent.loadSession("with-identity");
    {
        auto loaded = sm.load("with-identity");
        CHECK_OK(loaded.agentName == "persisted-writer",
                 "persisted agent_name NOT clobbered");
        CHECK_OK(loaded.model == "persisted-moap", "persisted model NOT clobbered");
        CHECK_OK(loaded.provider == "persisted-provider",
                 "persisted provider NOT clobbered");
    }

    fs::current_path(prev);
}

static void test_checkpoint_repairs_empty_records(const fs::path& tmp) {
    std::cout << "checkpoint repairs empty records…\n";
    auto prev = fs::current_path();
    fs::current_path(tmp);

    AgentConfig acfg;
    acfg.name = "brainstormer";
    acfg.provider = "opencode";
    acfg.model = "deepseek-v4-flash-free";
    Agent agent(acfg, std::make_shared<NoopProvider>());

    // Session file with empty records (wipe-bug shape).
    Session empty;
    empty.id = "wiped-sess";
    empty.agentName = "brainstormer";
    empty.model = acfg.model;
    empty.provider = acfg.provider;
    empty.created = session::SessionManager::iso8601();
    empty.updated = empty.created;
    session::SessionManager sm;
    sm.save(empty);

    // Sibling state checkpoint still has full history.
    fs::create_directories(tmp / ".cortex" / "state");
    {
        std::ofstream f((tmp / ".cortex" / "state" / "wiped-sess.json").string());
        f << R"({
  "format": "cortex-agent-state",
  "version": 1,
  "agent_name": "brainstormer",
  "history": [
    "User: ping a subagent",
    "Agent: alive — /tmp"
  ],
  "context_feeds": []
})";
    }

    agent.loadSession("wiped-sess");
    CHECK_OK(!agent.history().empty(), "history recovered from state checkpoint");
    CHECK_OK(agent.history().front().find("User: ping") != std::string::npos,
             "first history line is typed User prompt");

    auto repaired = sm.load("wiped-sess");
    CHECK_OK(!repaired.records.empty(), "session.records re-saved non-empty");
    CHECK_OK(repaired.records[0].role == SessionRecord::USER,
             "first repaired record is USER");
    CHECK_OK(repaired.records[0].content.find("ping") != std::string::npos,
             "repaired USER content preserved");

    // Empty-history save must not wipe repaired records.
    Agent emptyAgent(acfg, std::make_shared<NoopProvider>());
    emptyAgent.saveSession("wiped-sess");
    auto still = sm.load("wiped-sess");
    CHECK_OK(!still.records.empty(),
             "empty-history saveSession does not wipe existing records");

    fs::current_path(prev);
}

int main() {
    auto tmp = makeTmpCwd();
    test_identity_backfill(tmp);
    // fresh subdir for checkpoint test so state/sessions don't collide
    auto tmp2 = makeTmpCwd();
    test_checkpoint_repairs_empty_records(tmp2);

    std::cout << "\n────── " << (g_fail ? "FAIL" : "ok") << " ──────\n";
    fs::remove_all(tmp);
    fs::remove_all(tmp2);
    return g_fail ? 1 : 0;
}
