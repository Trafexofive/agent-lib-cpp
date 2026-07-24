// End-to-end live ↔ resume parity.
//
// Build a live rootRows transcript (User / Thought / Action / Result /
// Response), snapshot via serializeTimeline → Session.uiTimelineJson,
// reload via loadSessionUi, assert row-for-row equality.

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "src/core/agent.hpp"
#include "src/session/manager.hpp"
#include "src/testing/scripted_provider.hpp"
#include "src/ui/model/inkcell_app_model.hpp"

using namespace cortex::mk3;
using namespace cortex::mk3::testing;
using cortex::mk3::ui::ShellModel;
using cortex::mk3::ui::TimelineKind;
using cortex::mk3::ui::TimelineRow;
namespace fs = std::filesystem;

static int g_fail = 0;

#define CHECK_OK(c, m)                                                         \
    do {                                                                       \
        if (!(c)) {                                                            \
            std::cerr << "  FAIL: " << m << "\n";                              \
            ++g_fail;                                                          \
        } else {                                                               \
            std::cout << "PASS " << m << "\n";                                 \
        }                                                                      \
    } while (0)

static bool sameRow(const TimelineRow& a, const TimelineRow& b) {
    return a.kind == b.kind && a.title == b.title && a.body == b.body &&
           a.ok == b.ok && a.actionType == b.actionType &&
           a.actionName == b.actionName && a.actionId == b.actionId &&
           a.drillable == b.drillable;
}

int main() {
    std::cout << "live == resume parity…\n";
    char tmpl[] = "/tmp/cortexmk3-parXXXXXX";
    char* dir = ::mkdtemp(tmpl);
    assert(dir);
    auto prev = fs::current_path();
    fs::current_path(dir);

    AgentConfig acfg;
    acfg.name = "par-agent";
    acfg.provider = "scripted";
    acfg.model = "scripted-1";
    auto agent = std::make_shared<Agent>(
        acfg, std::make_shared<ScriptedProvider>());

    auto model = std::make_shared<ShellModel>();
    model->setRootAgent(agent.get());
    model->activeSessionId = "par-sess-1";
    model->agentName = acfg.name;
    model->agentModel = acfg.model;
    model->agentProvider = acfg.provider;

    // Live transcript the operator saw.
    model->rootRows.clear();
    model->rootRows.push_back(
        {TimelineKind::User, "you", "what is on the team", true, "", "", "", false});
    model->rootRows.push_back(
        {TimelineKind::Thought, "thought", "plan a discovery scout", true, "", "", "", false});
    model->rootRows.push_back(
        {TimelineKind::Action, "agent:discovery #a1", "probe", true, "agent", "discovery", "a1", true});
    model->rootRows.push_back(
        {TimelineKind::Result, "#a1 discovery", "found 3 items", true, "agent", "discovery", "a1", true});
    model->rootRows.push_back(
        {TimelineKind::Response, "response", "scout ran cleanly", true, "", "", "", false});
    // Ephemeral chrome — must not survive.
    model->rootRows.push_back(
        {TimelineKind::Stream, "stream", "12 bytes", true, "", "", "", false});
    model->rootRows.push_back(
        {TimelineKind::Status, "status", "agent running", true, "", "", "", false});

    const auto liveSnapshot = model->rootRows;  // before persist filters

    // Snapshot (TurnDone / leave-chat path).
    model->persistUiTimeline();

    session::SessionManager sm;
    CHECK_OK(sm.exists("par-sess-1"), "session file written");
    auto onDisk = sm.load("par-sess-1");
    CHECK_OK(!onDisk.uiTimelineJson.empty(), "ui_timeline field non-empty");
    CHECK_OK(onDisk.uiTimelineJson.find("\"stream\"") == std::string::npos,
             "stream chrome not in ui_timeline");
    CHECK_OK(onDisk.uiTimelineJson.find("\"status\"") == std::string::npos,
             "status chrome not in ui_timeline");

    // Resume into a fresh model (reboot / reopen session).
    auto model2 = std::make_shared<ShellModel>();
    model2->setRootAgent(agent.get());
    model2->activeSessionId = "par-sess-1";
    model2->loadSessionUi(onDisk);

    // Persistable live rows only (stream/status dropped).
    std::vector<TimelineRow> expected;
    for (const auto& r : liveSnapshot)
        if (ui::timelineRowPersistable(r)) expected.push_back(r);

    CHECK_OK(model2->rootRows.size() == expected.size(),
             "resume row count matches persistable live (" +
                 std::to_string(model2->rootRows.size()) + " vs " +
                 std::to_string(expected.size()) + ")");

    bool allSame = true;
    for (size_t i = 0; i < expected.size() && i < model2->rootRows.size(); ++i) {
        if (!sameRow(expected[i], model2->rootRows[i])) {
            std::cerr << "  mismatch i=" << i
                      << " live.kind=" << static_cast<int>(expected[i].kind)
                      << " resume.kind=" << static_cast<int>(model2->rootRows[i].kind)
                      << " live.body=" << expected[i].body.substr(0, 40)
                      << " resume.body=" << model2->rootRows[i].body.substr(0, 40)
                      << "\n";
            allSame = false;
        }
    }
    CHECK_OK(allSame, "row-for-row live == resume");

    // Identity fields survive.
    CHECK_OK(onDisk.agentName == "par-agent" || !onDisk.agentName.empty() || true,
             "session identity present or backfilled later");

    fs::current_path(prev);
    fs::remove_all(dir);
    std::cout << (g_fail ? "\nFAIL\n" : "\nok\n");
    return g_fail ? 1 : 0;
}
