#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "src/ui/scenes/agent_scene.hpp"
#include "src/ui/scenes/main_scene.hpp"

#include <vector>

using namespace cortex::mk3;
using namespace cortex::mk3::ui;

namespace {
int failures = 0;

void check(bool condition, const std::string& name) {
    std::cout << "  " << name << "... " << (condition ? "PASS" : "FAIL") << "\n";
    if (!condition) ++failures;
}

inkcell::KeyEvent key(inkcell::KeyCode code, char ch = 0) {
    inkcell::KeyEvent event;
    event.code = code;
    event.ch = ch;
    return event;
}

void type(scenes::AgentScene& scene, const std::string& text) {
    for (char ch : text) scene.on_key(key(inkcell::KeyCode::Character, ch));
}

std::string surfaceText(const inkcell::Surface& surface) {
    std::string out;
    for (int y = 0; y < surface.height(); ++y) {
        for (int x = 0; x < surface.width(); ++x) out += surface.at({x, y}).glyph;
        out += '\n';
    }
    return out;
}

void test_dashboard_scene() {
    InkcellAppConfig cfg;
    cfg.provider = "openai-codex";
    cfg.model = "gpt-5.5";
    cfg.agentName = "builtin";
    cfg.toolCount = 3;
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    scenes::MainScene scene(cfg, bridge, model);
    scene.on_enter();
    for (const inkcell::Size size : {inkcell::Size{80, 24}, inkcell::Size{120, 34}, inkcell::Size{160, 44}}) {
        inkcell::Surface surface(size);
        scene.draw(surface);
        std::string rendered = surfaceText(surface);
        check(rendered.find("CORTEX MK3") != std::string::npos,
              "dashboard renders header at " + std::to_string(size.w) + "x" + std::to_string(size.h));
        // Narrow widths compact the dock; require the hub identity + at least
        // one full pill label that survives 80-col.
        check(rendered.find("Home") != std::string::npos ||
                  rendered.find("Sessions") != std::string::npos ||
                  rendered.find("Settings") != std::string::npos,
              "dashboard renders a hub pill at " + std::to_string(size.w) + "x" +
                  std::to_string(size.h));
        // Home absorbs former Harness/Runtime peers
        check(rendered.find("HARNESS") != std::string::npos ||
                  rendered.find("RUNTIME") != std::string::npos,
              "home surfaces harness/runtime at " + std::to_string(size.w) + "x" +
                  std::to_string(size.h));
    }

    scene.on_key(key(inkcell::KeyCode::Character, 's'));
    check(model->dashboard.section == model::DashboardSection::Sessions &&
              model->dashboard.focus == model::DashboardFocus::Content,
          "dashboard sessions shortcut focuses session inventory");

    // Enter on launchable agent queues hot-swap launch (not a dead notice).
    model->dashboard.select(model::DashboardSection::Manifests);
    model->dashboard.refreshManifests();
    bool queued = false;
    for (int i = 0; i < static_cast<int>(model->dashboard.manifests.size()); ++i) {
        model->dashboard.manifestIndex = i;
        const auto* m = model->dashboard.selectedManifest();
        if (!m || m->kind != "agent" || !m->launchable) continue;
        scene.on_key(key(inkcell::KeyCode::Enter));
        queued = !model->pendingLaunchManifest.empty() &&
                 model->pendingLaunchManifest == m->path;
        check(queued, "enter on agent queues pendingLaunchManifest");
        model->pendingLaunchManifest.clear();
        break;
    }
    check(!model->dashboard.manifests.empty(), "manifests available for launch test");

    // Enter on the Workflows pill opens the workflow page (run is ↵/r inside).
    model->dashboard.select(model::DashboardSection::Workflows);
    model->dashboard.refreshManifests();
    bool wfOpened = false;
    scene.on_key(key(inkcell::KeyCode::Enter));
    wfOpened = model->pendingRoute == PendingRoute::Workflow &&
               !model->activeWorkflowManifestPath.empty();
    check(wfOpened, "enter on workflow opens workflow page");
    check(!model->activeWorkflowName.empty(), "workflow page has a selected name");
    model->clearRoute();
    scene.on_key(key(inkcell::KeyCode::Character, 'c'));
    check(model->pendingRoute == PendingRoute::Agent, "dashboard chat shortcut requests chat route");
    model->clearRoute();
    scene.on_key(key(inkcell::KeyCode::Character, 'q'));
    check(model->pendingRoute == PendingRoute::Quit, "dashboard quit shortcut requests app exit");
}

void test_ask_choice_roundtrip() {
    InkcellAppConfig cfg;
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    scenes::AgentScene scene(cfg, bridge, model);
    scene.on_enter();

    Json::Value params;
    params["chainTitle"] = "Pick worker";
    Json::Value card;
    card["id"] = "worker";
    card["type"] = "choice";
    card["title"] = "Worker";
    card["options"].append("reader");
    card["options"].append("tester");
    params["cards"].append(card);

    auto future = std::async(std::launch::async, [&] { return bridge.requestAsk(params); });
    for (int i = 0; i < 100 && !bridge.askPending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    scene.update({}, {});
    check(model->askActive, "scene activates ask overlay from worker request");
    scene.on_key(key(inkcell::KeyCode::ArrowDown));
    scene.on_key(key(inkcell::KeyCode::Enter));
    scene.update({}, {});
    // Must not hang — finishAskCard has to completeAsk the bridge.
    Json::Value result = future.get();
    check(result["success"].asBool() && result["results"]["worker"].asString() == "tester",
          "scene returns selected ask choice to worker");
    check(!model->askActive && !bridge.askPending(),
          "ask overlay clears and bridge ask is no longer pending");
}

void test_ask_notes_only_auto_completes() {
    // Dialog with only note/info cards must not block the worker forever.
    InkcellAppConfig cfg;
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    scenes::AgentScene scene(cfg, bridge, model);
    scene.on_enter();

    Json::Value params;
    params["chainTitle"] = "Notice";
    Json::Value note;
    note["id"] = "n1";
    note["type"] = "note";
    note["title"] = "Heads up";
    note["message"] = "No input required";
    params["cards"].append(note);

    auto future = std::async(std::launch::async, [&] { return bridge.requestAsk(params); });
    for (int i = 0; i < 100 && !bridge.askPending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    scene.update({}, {});  // drain + settleAsk
    Json::Value result = future.get();
    check(result["success"].asBool(), "notes-only ask auto-completes successfully");
    check(!bridge.askPending(), "notes-only ask leaves bridge idle");
}

void test_slash_and_completion() {
    InkcellAppConfig cfg;
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    scenes::AgentScene scene(cfg, bridge, model);
    scene.on_enter();

    type(scene, "/help");
    scene.on_key(key(inkcell::KeyCode::Enter));
    check(!model->rootRows.empty() && model->rootRows.back().title == "commands",
          "slash help is intercepted locally");
    check(model->pendingSubmit.empty(), "slash help is not sent to agent");

    model->composer.value.clear();
    model->composer.cursor = 0;
    type(scene, "/debug");
    scene.on_key(key(inkcell::KeyCode::Tab));
    check(model->composer.value.rfind("/debugger", 0) == 0,
          "Tab completes dynamic prompt command");
    scene.on_key(key(inkcell::KeyCode::Enter));
    check(!model->composer.value.empty() && model->composer.value.find("debug") != std::string::npos,
          "dynamic command expands into reviewed composer text");
    check(model->pendingSubmit.empty(), "dynamic expansion is not auto-submitted");

    scene.on_key(key(inkcell::KeyCode::Escape));
    scene.on_key(key(inkcell::KeyCode::Character, '?'));
    check(model->helpVisible, "question mark opens help from transcript focus");
    scene.on_key(key(inkcell::KeyCode::Escape));
    check(!model->helpVisible, "Escape closes help overlay");
    theme::set(theme::Variant::Graphite);
    scene.on_key(key(inkcell::KeyCode::Character, 'T'));
    check(theme::activeVariant == theme::Variant::Neon, "uppercase T switches theme in transcript focus");
    theme::set(theme::Variant::Graphite);
    scene.on_key(key(inkcell::KeyCode::Character, 'm'));
    check(model->pendingRoute == PendingRoute::Main, "chat transcript shortcut requests dashboard route");
    model->clearRoute();
}

void test_ctrl_c_state() {
    InkcellAppConfig cfg;
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    scenes::AgentScene scene(cfg, bridge, model);
    // 1st Ctrl-C while running → cancel (pi UX), not quit.
    model->running = true;
    clearRunStop();
    scene.on_key(key(inkcell::KeyCode::CtrlC));
    check(!runIsActive() && model->status.find("cancelling") == 0,
          "Ctrl-C requests active turn cancellation");
    check(model->pendingRoute != PendingRoute::Quit, "1st Ctrl-C while running does not quit");
    // 2nd Ctrl-C when idle → quit.
    model->running = false;
    model->askActive = false;
    model->status = "idle";
    clearRunStop();
    model->clearRoute();
    scene.on_key(key(inkcell::KeyCode::CtrlC));
    check(model->pendingRoute == PendingRoute::Quit, "2nd Ctrl-C when idle requests quit");
    clearRunStop();
}

void test_esc_never_routes_main_or_quit() {
    InkcellAppConfig cfg;
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    scenes::AgentScene scene(cfg, bridge, model);
    model->composer.focused = true;
    model->timelineFocus = false;
    scene.on_key(key(inkcell::KeyCode::Escape));
    check(model->timelineFocus && !model->composer.focused,
          "Esc from composer focuses timeline");
    check(model->pendingRoute == PendingRoute::None, "Esc does not route to main");
    check(model->pendingRoute != PendingRoute::Quit, "Esc does not quit");
    scene.on_key(key(inkcell::KeyCode::Escape));
    check(model->composer.focused && !model->timelineFocus,
          "Esc from timeline returns to composer");
    check(model->pendingRoute == PendingRoute::None, "Esc still does not route to main");
}
void test_chat_scroll_keys() {
    // Regression for the "no way to actually scroll the history" complaint.
    // PageUp/PageDown/Home/End scroll from the COMPOSER (peek at history while
    // typing); in TIMELINE focus ArrowUp/Down scroll line-by-line (the prior
    // binding jumped between block markers and never free-scrolled).
    InkcellAppConfig cfg;
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    scenes::AgentScene scene(cfg, bridge, model);
    scene.on_enter();
    model->composer.focused = true;
    model->timelineFocus = false;

    std::vector<std::string> lines;
    for (int i = 0; i < 200; ++i) lines.push_back("line " + std::to_string(i));
    model->transcriptView.viewport_h = 10;
    model->transcriptView.set_lines(lines);
    check(model->transcriptView.stick_bottom, "long transcript sticks to bottom by default");
    int bottom = model->transcriptView.offset;
    check(bottom == 190, "stick-to-bottom offset is lines - viewport (190)");

    // PageUp from the COMPOSER scrolls up without leaving the composer.
    scene.on_key(key(inkcell::KeyCode::PageUp));
    check(model->transcriptView.offset < bottom && !model->transcriptView.stick_bottom,
          "PageUp scrolls transcript up from the composer and un-sticks");
    check(model->composer.focused, "PageUp keeps the composer focused");

    // PageDown scrolls back down to the bottom and re-sticks.
    scene.on_key(key(inkcell::KeyCode::PageDown));
    check(model->transcriptView.offset == bottom && model->transcriptView.stick_bottom,
          "PageDown returns to the bottom and re-sticks");

    // Home/End are composer line motions while focused; transcript jumps live
    // on timeline focus (tested below). PageUp already un-stuck; re-stick first.
    scene.on_key(key(inkcell::KeyCode::PageDown));
    check(model->transcriptView.stick_bottom && model->transcriptView.offset == bottom,
          "PageDown re-sticks to the bottom");

    // In TIMELINE focus: ArrowUp/Down scroll line-by-line. Esc enters timeline
    // focus but focusTimeline() rebuilds the view from the model transcript
    // (empty here), so re-seed the transcript before scrolling — none of the
    // scroll keys rebuild, so the seeded lines stay for the scroll assertions.
    scene.on_key(key(inkcell::KeyCode::Escape));  // composer -> timeline
    check(!model->composer.focused, "Esc leaves the composer for timeline focus");
    model->transcriptView.viewport_h = 10;
    model->transcriptView.set_lines(lines);
    scene.on_key(key(inkcell::KeyCode::Home));   // start from the top
    check(model->transcriptView.offset == 0, "Home in timeline jumps to top");
    scene.on_key(key(inkcell::KeyCode::ArrowDown));
    check(model->transcriptView.offset == 1, "ArrowDown scrolls one line in timeline focus");
    scene.on_key(key(inkcell::KeyCode::ArrowDown));
    check(model->transcriptView.offset == 2, "ArrowDown scrolls one line again");
    scene.on_key(key(inkcell::KeyCode::ArrowUp));
    check(model->transcriptView.offset == 1, "ArrowUp scrolls one line back");
}
}  // namespace

int main() {
    std::cout << "Chat/dashboard scene integration tests\n";
    test_dashboard_scene();
    test_ask_choice_roundtrip();
    test_ask_notes_only_auto_completes();
    test_slash_and_completion();
    test_ctrl_c_state();
    test_esc_never_routes_main_or_quit();
    test_chat_scroll_keys();
    std::cout << "\n" << (failures == 0 ? "all passed" : "failures: " + std::to_string(failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
