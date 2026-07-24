// Vet-fix: session-load backfill test. Verifies that Agent::loadSession
// wears the legacy empty agent_name / model / provider with a copy from
// the current Agent's config_, persisting the correction so the
// Sessions page shows the right name without operator intervention.

#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "src/core/agent.hpp"
#include "src/session/manager.hpp"

using namespace cortex::mk3;
namespace fs = std::filesystem;

static fs::path g_tmpDir;

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

#define CHECK_OK(cond, msg)                                          \
    do {                                                             \
        if (!(cond)) {                                               \
            std::cerr << "\n  FAIL: " << msg << " at "               \
                      << __FILE__ << ":" << __LINE__ << "\n";        \
            return 1;                                                \
        }                                                            \
        std::cout << "PASS " << msg << "\n";                         \
    } while (0)

int main() {
    std::cout << "load-session backfill test...\n";

    char tmpl[] = "/tmp/cortexmk3-bfXXXXXX";
    char* dir = ::mkdtemp(tmpl);
    assert(dir);
    g_tmpDir = dir;
    ::setenv("CORTEX_HOME", dir, 1);

    AgentConfig acfg;
    acfg.name = "brainstormer-001";
    acfg.provider = "noop-test";
    acfg.model = "no-op-moap-1";
    auto provider = std::make_shared<NoopProvider>();
    Agent agent(acfg, provider);

    // Step 1: write a session "as the legacy code path would have" — an
    // empty agent_name + empty model + empty provider.
    Session legacyWritten;
    legacyWritten.id = "legacy-empty";
    legacyWritten.agentName = "";
    legacyWritten.model = "";
    legacyWritten.provider = "";
    legacyWritten.created = session::SessionManager::iso8601();
    legacyWritten.updated = legacyWritten.created;
    legacyWritten.records.push_back({SessionRecord::USER,
                                     "preserved legacy prompt",
                                     legacyWritten.created,
                                     ""});
    {
        session::SessionManager sm;
        sm.save(legacyWritten);
    }

    // Step 2: load it back through the Agent with empty fields. After
    // the vet-fix, agent.loadSession should backfill from config_ and
    // re-save the file with the corrected identity.
    agent.loadSession("legacy-empty");

    // Verify on-disk persisted state is corrected.
    {
        session::SessionManager sm;
        auto loaded = sm.load("legacy-empty");
        CHECK_OK(loaded.agentName == "brainstormer-001",
                 "loaded agentName corrected to config_.name");
        CHECK_OK(loaded.model == "no-op-moap-1",
                 "loaded model corrected to config_.model");
        CHECK_OK(loaded.provider == "noop-test",
                 "loaded provider corrected to config_.provider");
        // Existing records preserved across the backfill pass
        CHECK_OK(loaded.records.size() == 1,
                 "records preserved (no data loss)");
    }

    // Step 3: load a session that already has identity. Backfill must
    // not clobber persisted identity. This is the AC1 invariant from
    // 48582e5.
    Session identitySet;
    identitySet.id = "with-identity";
    identitySet.agentName = "persisted-writer";
    identitySet.model = "persisted-moap";
    identitySet.provider = "persisted-provider";
    identitySet.created = session::SessionManager::iso8601();
    identitySet.updated = identitySet.created;
    {
        session::SessionManager sm;
        sm.save(identitySet);
    }
    agent.loadSession("with-identity");
    {
        session::SessionManager sm;
        auto loaded = sm.load("with-identity");
        CHECK_OK(loaded.agentName == "persisted-writer",
                 "persisted agent_name NOT clobbered on reload");
        CHECK_OK(loaded.model == "persisted-moap",
                 "persisted model NOT clobbered");
        CHECK_OK(loaded.provider == "persisted-provider",
                 "persisted provider NOT clobbered");
    }

    std::cout << "\n────── ok ──────\n";
    std::cout << "load-session backfill test: PASS\n";
    std::cout << "────────────────\n";

    fs::remove_all(g_tmpDir);
    return 0;
}
