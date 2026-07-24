// Substance regression: seedUserPrompt + prompt(final) must leave
// session.records with User AND Agent — not User-only.
//
// Live bug: prompt() reloaded disk mid-turn (wiping Agent accumulation)
// and taskComplete broke without pushing Agent: into history_.

#include <unistd.h>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "src/core/agent.hpp"
#include "src/session/manager.hpp"
#include "src/testing/scripted_provider.hpp"

using namespace cortex::mk3;
using namespace cortex::mk3::testing;
namespace fs = std::filesystem;

static int g_fail = 0;

#define CHECK_OK(cond, msg)                                                    \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "  FAIL: " << msg << "\n";                            \
            ++g_fail;                                                          \
        } else {                                                               \
            std::cout << "PASS " << msg << "\n";                               \
        }                                                                      \
    } while (0)

int main() {
    char tmpl[] = "/tmp/cortexmk3-rtXXXXXX";
    char* dir = ::mkdtemp(tmpl);
    assert(dir);
    fs::path tmp = dir;
    auto prev = fs::current_path();
    fs::current_path(tmp);

    AgentConfig acfg;
    acfg.name = "roundtrip-agent";
    acfg.provider = "scripted";
    acfg.model = "scripted-1";
    acfg.runtimeMode = "small";
    acfg.completionPolicy = "promote";

    auto provider = std::make_shared<ScriptedProvider>(std::deque<std::string>{
        "<response final=\"true\">triggers live in workflows; agents can share</response>",
    });
    Agent agent(acfg, provider);

    const std::string sid = "rt-sess-1";
    const std::string user = "what are the triggers we have implemented?";

    // Mirror submitComposer: seed + early save (User only on disk momentarily).
    agent.seedUserPrompt(user);
    agent.saveSession(sid);
    {
        session::SessionManager sm;
        auto early = sm.load(sid);
        CHECK_OK(early.records.size() == 1, "early save has User only");
        CHECK_OK(early.records[0].role == SessionRecord::USER, "early record is USER");
    }

    // Full turn — must NOT wipe seed; must append Agent and save both.
    std::string out = agent.prompt(user, sid, /*ephemeral=*/false);
    CHECK_OK(out.find("triggers") != std::string::npos || !out.empty(),
             "prompt returns model text");

    {
        session::SessionManager sm;
        auto final = sm.load(sid);
        CHECK_OK(final.records.size() >= 2,
                 "final session has User + Agent (got " +
                     std::to_string(final.records.size()) + ")");
        bool hasUser = false, hasAgent = false;
        for (const auto& r : final.records) {
            if (r.role == SessionRecord::USER &&
                r.content.find("triggers") != std::string::npos)
                hasUser = true;
            if (r.role == SessionRecord::AGENT && !r.content.empty())
                hasAgent = true;
        }
        CHECK_OK(hasUser, "User record preserved after prompt");
        CHECK_OK(hasAgent, "Agent record present after final response");
    }

    // Second Agent instance cold-resume (reboot simulation).
    Agent agent2(acfg, std::make_shared<ScriptedProvider>());
    agent2.loadSession(sid);
    CHECK_OK(agent2.history().size() >= 2, "cold load restores multi-line history");
    CHECK_OK(agent2.history()[0].find("User:") != std::string::npos,
             "cold load first line User");
    bool agentLine = false;
    for (const auto& h : agent2.history())
        if (h.rfind("Agent:", 0) == 0) agentLine = true;
    CHECK_OK(agentLine, "cold load includes Agent line");

    fs::current_path(prev);
    fs::remove_all(tmp);
    std::cout << "\n────── " << (g_fail ? "FAIL" : "ok") << " ──────\n";
    return g_fail ? 1 : 0;
}
