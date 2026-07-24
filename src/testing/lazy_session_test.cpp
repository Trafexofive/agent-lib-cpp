// Vet-fix: lazy-arm session-id round-trip test. Operator reported that
// `./cortex-mk3 --tui experimental` followed by typing a prompt produced
// zero files in ~/.config/cortex-mk3/sessions/. Root cause was that
// Phase 1 removed CLI-launched auto-mint, and submitComposer had no
// path to arm an id at first non-empty submit. This test asserts the
// fix: an empty-activeSessionId ShellModel, after one submit, has a
// non-empty activeSessionId; flushAgentSession writes the file.

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
#include "src/ui/model/inkcell_app_model.hpp"

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
    std::string providerName() const override { return "noop"; }
   private:
    std::string model_ = "noop";
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
    std::cout << "lazy-arm session-id round-trip...\n";

    char tmpl[] = "/tmp/cortexmk3-asanXXXXXX";
    char* dir = ::mkdtemp(tmpl);
    assert(dir);
    g_tmpDir = dir;
    ::setenv("CORTEX_HOME", dir, 1);

    AgentConfig acfg;
    acfg.name = "lazy-agent";
    acfg.provider = "noop";
    acfg.model = "noop-1";

    auto provider = std::make_shared<NoopProvider>();
    Agent agent(acfg, provider);

    auto model = std::make_shared<ui::ShellModel>();
    model->setRootAgent(&agent);

    ui::InkcellAppConfig cfg;
    cfg.ephemeral = false;
    cfg.sessionId = "";
    // skip initializeChatModel: we're only testing submitComposer + saveSession

    CHECK_OK(model->activeSessionId.empty(), "activeSessionId starts empty");

    model->composer.value = "hello world";
    bool sub = model->submitComposer();
    CHECK_OK(sub == true, "submitComposer accepts non-empty text");
    CHECK_OK(!model->activeSessionId.empty(),
             "submitComposer arms a session id lazily");

    // saveSession writes to sm.save() — confirms the disk path.
    agent.saveSession(model->activeSessionId);

    session::SessionManager sm;
    CHECK_OK(sm.exists(model->activeSessionId),
             "saveSession wrote the session file under tmp base");

    // Vet-fix: Operator-reported scenario — bare TUI exit between submit
    // and runAgentTurn's first iteration. The session file must contain
    // at least the typed User record so `recover` shows the chat.
    auto loaded = sm.load(model->activeSessionId);
    CHECK_OK(!loaded.records.empty(),
             "saved session has at least one record (typed user prompt)");
    bool foundUser = false;
    for (const auto& rec : loaded.records) {
        if (rec.role == SessionRecord::USER && rec.content == "hello world") {
            foundUser = true;
            break;
        }
    }
    CHECK_OK(foundUser,
             "saved session carries the typed User record");

    std::cout << "\n────── ok ──────\n";
    std::cout << "lazy-arm session test: PASS\n";
    std::cout << "────────────────\n";

    fs::remove_all(g_tmpDir);
    return 0;
}
