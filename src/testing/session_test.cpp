// =============================================================================
// agent-lib-MK3 — Session round-trip tests
//
// Targets:
//   AC14 — saveSession must use config_.provider, not the hardcoded "deepseek"
//   AC04 — saveSession must preserve `created` timestamp across subsequent saves
//   AC18 — contextFeeds_ must round-trip through save→load
// =============================================================================

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "src/core/agent.hpp"
#include "src/providers/factory.hpp"
#include "src/session/manager.hpp"

using namespace cortex::mk3;
namespace fs = std::filesystem;

static int passed = 0, failed = 0;

#define TEST(name)                           \
    do {                                     \
        std::cout << "  " << name << "... "; \
    } while (0)
#define PASS()                 \
    do {                       \
        passed++;              \
        std::cout << "PASS\n"; \
    } while (0)
#define FAIL(msg)                             \
    do {                                      \
        failed++;                             \
        std::cout << "FAIL: " << msg << "\n"; \
        return;                               \
    } while (0)
#define CHECK(cond, msg) \
    do {                 \
        if (!(cond)) {   \
            FAIL(msg);   \
        }                \
    } while (0)

static fs::path g_tmpDir;

class NoopProvider : public ILlmProvider {
   public:
    std::string generate(const ChatMessages&) override {
        return "<response final=\"true\">OK</response>";
    }
    void generateStream(const ChatMessages&, StreamCallback cb) override {
        cb("<response final=\"true\">OK</response>", true);
    }
    void setModel(const std::string& model) override {
        model_ = model;
    }
    void setTemperature(double t) override {
        temperature_ = t;
    }
    void setMaxTokens(int n) override {
        maxTokens_ = n;
    }
    void setTopP(double p) override {
        topP_ = p;
    }
    std::string getModel() const override {
        return model_;
    }
    double getTemperature() const override {
        return temperature_;
    }
    int getMaxTokens() const override {
        return maxTokens_;
    }
    std::vector<ModelInfo> listModels() override {
        return {};
    }
    std::string providerName() const override {
        return "noop";
    }

   private:
    std::string model_ = "noop";
    double temperature_ = 0.7;
    int maxTokens_ = 65536;
    double topP_ = 0.95;
};

static std::unique_ptr<Agent> makeAgent(const std::string& provider, const std::string& model) {
    AgentConfig cfg;
    cfg.name = "session-test-agent";
    cfg.provider = provider;
    cfg.model = model;
    // Point the agent at our tmp sessions dir so we don't pollute ~/.cortex.
    setenv("HOME", g_tmpDir.string().c_str(), 1);
    auto p = providers::createProvider(provider, model);
    return std::make_unique<Agent>(cfg, p);
}

static std::unique_ptr<Agent> makeNoopAgent() {
    AgentConfig cfg;
    cfg.name = "session-test-agent";
    cfg.provider = "noop";
    cfg.model = "noop";
    setenv("HOME", g_tmpDir.string().c_str(), 1);
    return std::make_unique<Agent>(cfg, std::make_shared<NoopProvider>());
}

// ─────────────────────────────────────────────────────────────────────────────
// AC14 — provider isn't hardcoded
// ─────────────────────────────────────────────────────────────────────────────
void test_AC14_provider_not_hardcoded() {
    TEST("AC14 saveSession uses config_.provider (not hardcoded \"deepseek\")");
    auto agent = makeAgent("groq", "llama-3.3-70b-versatile");
    agent->saveSession("ac14-test");

    auto session = agent->sessionMgr().load("ac14-test");
    CHECK(session.provider == "groq",
          (std::string("expected provider=groq, got: '") + session.provider + "'").c_str());
    CHECK(session.model == "llama-3.3-70b-versatile", "model should roundtrip");
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// AC04 — saveSession preserves `created` across multiple saves
// ─────────────────────────────────────────────────────────────────────────────
void test_AC04_created_preserved() {
    TEST("AC04 saveSession preserves `created` timestamp on subsequent saves");
    auto agent = makeAgent("deepseek", "deepseek-chat");
    agent->saveSession("ac04-test");
    auto firstSave = agent->sessionMgr().load("ac04-test");
    std::string originalCreated = firstSave.created;
    CHECK(!originalCreated.empty(), "first save should set `created`");

    // Sleep a moment so any timestamp regeneration would visibly differ.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    agent->saveSession("ac04-test");
    auto secondSave = agent->sessionMgr().load("ac04-test");
    CHECK(secondSave.created == originalCreated,
          (std::string("`created` was overwritten: orig='") + originalCreated + "' new='" +
           secondSave.created + "'")
              .c_str());
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// AC18 — contextFeeds round-trip through SessionManager
// ─────────────────────────────────────────────────────────────────────────────
void test_AC18_context_feeds_roundtrip() {
    TEST("AC18 contextFeeds_ persists across save→load");
    // Construct a Session directly and round-trip via the SessionManager.
    setenv("HOME", g_tmpDir.string().c_str(), 1);
    session::SessionManager sm;
    Session s = sm.create("ac18-test", "test-agent", "deepseek-chat", "deepseek");
    s.contextFeeds = {"<feed name=\"git\">branch=master</feed>",
                      "<feed name=\"clock\">2026-06-11T22:00Z</feed>"};
    sm.save(s);

    Session loaded = sm.load("ac18-test");
    CHECK(loaded.contextFeeds.size() == 2,
          (std::string("expected 2 feeds, got ") + std::to_string(loaded.contextFeeds.size()))
              .c_str());
    CHECK(loaded.contextFeeds[0].find("branch=master") != std::string::npos,
          "feed 0 content not preserved");
    CHECK(loaded.contextFeeds[1].find("2026-06-11T22:00Z") != std::string::npos,
          "feed 1 content not preserved");
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// AC17 — dumpSessionArtifacts writes explicit traces to CWD
// ─────────────────────────────────────────────────────────────────────────────
void test_AC17_cwd_trace_dumps() {
    TEST("AC17 dumpSessionArtifacts writes raw.md to CWD when opted in");
    fs::path oldCwd = fs::current_path();
    fs::path cwd = g_tmpDir / "ac17-cwd";
    fs::create_directories(cwd);
    fs::current_path(cwd);

    auto agent = makeNoopAgent();
    agent->setRaw(true);
    agent->setVerbose(true);
    agent->prompt("dump test", "ac17-test", true);

    CHECK(fs::exists(cwd / "raw.md"), "raw.md should be written to CWD");
    CHECK(fs::exists(cwd / "iterations.md"), "iterations.md should be written to CWD");

    fs::current_path(oldCwd);
    PASS();
}

int main() {
    std::cout.setf(std::ios::unitbuf);
    std::cout << "\n╔══════════════════════════════════════════╗\n";
    std::cout << "║   MK3 Session Round-Trip Tests           ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n\n";

    g_tmpDir = fs::temp_directory_path() / ("cortex-session-test-" + std::to_string(::getpid()));
    fs::create_directories(g_tmpDir);

    test_AC14_provider_not_hardcoded();
    test_AC04_created_preserved();
    test_AC18_context_feeds_roundtrip();
    test_AC17_cwd_trace_dumps();

    fs::remove_all(g_tmpDir);

    std::cout << "\n──────────────────────────────────────────\n";
    std::cout << "  " << passed << " passed, " << failed << " failed\n";
    std::cout << "──────────────────────────────────────────\n";
    return failed > 0 ? 1 : 0;
}
