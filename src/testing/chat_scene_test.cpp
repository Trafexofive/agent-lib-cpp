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
        check(rendered.find("CORTEX MK3  /  DASHBOARD") != std::string::npos,
              "dashboard renders header at " + std::to_string(size.w) + "x" + std::to_string(size.h));
        check(rendered.find("Overview") != std::string::npos &&
                  rendered.find("Sessions") != std::string::npos &&
                  rendered.find("Harness") != std::string::npos &&
                  rendered.find("Runtime") != std::string::npos,
              "dashboard renders sections at " + std::to_string(size.w) + "x" + std::to_string(size.h));
    }

    scene.on_key(key(inkcell::KeyCode::Character, 's'));
    check(model->dashboard.section == model::DashboardSection::Sessions &&
              model->dashboard.focus == model::DashboardFocus::Content,
          "dashboard sessions shortcut focuses session inventory");
    scene.on_key(key(inkcell::KeyCode::Character, 'c'));
    check(model->pendingRoute == "agent", "dashboard chat shortcut requests chat route");
    model->pendingRoute.clear();
    scene.on_key(key(inkcell::KeyCode::Character, 'q'));
    check(model->pendingRoute == "quit", "dashboard quit shortcut requests app exit");
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
    Json::Value result = future.get();
    check(result["success"].asBool() && result["results"]["worker"].asString() == "tester",
          "scene returns selected ask choice to worker");
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
    check(model->composer.value == "/debugger ", "Tab completes dynamic prompt command");
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
    check(model->pendingRoute == "main", "chat transcript shortcut requests dashboard route");
    model->pendingRoute.clear();
}

void test_ctrl_j_k_history_navigation() {
    // Ctrl-J / Ctrl-K scroll the transcript by one line in BOTH the
    // timeline focus and the composer focus, and the Ctrl modifier
    // means the keystroke never reaches the composer's text widget
    // (so typing isn't disrupted). This is the "history/context
    // navigation that disables input" the operator asked for.
    InkcellAppConfig cfg;
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    scenes::AgentScene scene(cfg, bridge, model);
    scene.on_enter();
    model->composer.focused = true;
    model->timelineFocus = false;

    std::vector<std::string> lines;
    for (int i = 0; i < 100; ++i) lines.push_back("line " + std::to_string(i));
    model->transcriptView.viewport_h = 10;
    model->transcriptView.set_lines(lines);
    int bottom = model->transcriptView.offset;  // sticks to bottom
    check(bottom == 90, "long transcript sticks to bottom (offset=lines-viewport)");

    // Ctrl-J from the COMPOSER at the bottom: offset stays at max and
    // re-sticks (no overflow, scroll_by at max arms stick_bottom).
    inkcell::KeyEvent ctrlJ;
    ctrlJ.code = inkcell::KeyCode::Character;
    ctrlJ.ch = 'j';
    ctrlJ.modifiers = inkcell::ModCtrl;
    scene.on_key(ctrlJ);
    check(model->transcriptView.offset == 90 && model->transcriptView.stick_bottom,
          "Ctrl-J at the bottom keeps the transcript stuck (no overflow)");

    // Now in TIMELINE focus: Ctrl-K scrolls up one line and un-sticks.
    model->timelineFocus = true;
    model->transcriptView.viewport_h = 10;
    model->transcriptView.set_lines(lines);
    inkcell::KeyEvent ctrlK;
    ctrlK.code = inkcell::KeyCode::Character;
    ctrlK.ch = 'k';
    ctrlK.modifiers = inkcell::ModCtrl;
    scene.on_key(ctrlK);
    check(model->transcriptView.offset == 89 && !model->transcriptView.stick_bottom,
          "Ctrl-K in timeline focus scrolls up one line and un-sticks");

    // Ctrl-J in timeline scrolls back down by one.
    scene.on_key(ctrlJ);
    check(model->transcriptView.offset == 90,
          "Ctrl-J in timeline scrolls down one line");

    // Confirm Ctrl-J does NOT reach the composer: the composer's value
    // must be unchanged (the Ctrl modifier means the text widget never
    // sees the keystroke, so 'j' isn't appended to the input).
    check(model->composer.value.empty(),
          "Ctrl-J does not insert 'j' into the composer (input disabled for the binding)");
}

void test_submit_locks_to_bottom() {
    // Live-correctness: when the operator submits a new prompt, the
    // transcript must drop back to bottom and lock, even if they had
    // scrolled up with Ctrl-K to read history. Otherwise the new turn
    // streams in below the viewport and the operator sees nothing until
    // they manually press End. submitComposer() pins stick_bottom=true
    // and scroll_to_end() at the start of every new turn.
    InkcellAppConfig cfg;
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    scenes::AgentScene scene(cfg, bridge, model);
    scene.on_enter();

    // Simulate the operator scrolling up to read history: 100-line
    // transcript, viewport 10, scrolled to the middle, un-stuck.
    std::vector<std::string> lines;
    for (int i = 0; i < 100; ++i) lines.push_back("line " + std::to_string(i));
    model->transcriptView.viewport_h = 10;
    model->transcriptView.set_lines(lines);
    model->transcriptView.stick_bottom = false;
    model->transcriptView.scroll_by(-30);  // scrolled up 30 lines
    int scrolledOffset = model->transcriptView.offset;
    check(!model->transcriptView.stick_bottom,
          "precondition: transcript is un-stuck after Ctrl-K");
    check(scrolledOffset < 90,
          "precondition: transcript is scrolled up (offset < bottom)");

    // Type a prompt and submit.
    model->composer.value = "hello";
    model->composer.cursor = 5;
    bool submitted = model->submitComposer();
    check(submitted, "submitComposer accepts a non-empty prompt");
    check(model->transcriptView.stick_bottom,
          "submitComposer locks stick_bottom=true (follows new turn)");
    // The User row was pushed; it must be in the root timeline.
    check(!model->rootRows.empty() && model->rootRows.back().title == "you",
          "submitComposer pushes a User row (title='you') at the bottom");
    // The viewport must be at the bottom of the (rebuilt) transcript so the
    // streaming response is visible from the first token. max_off for a
    // single User row (header + body) with viewport 10 is 0 (clamped) —
    // the key contract is stick_bottom=true, which scroll_to_end pinned.
    check(model->transcriptView.stick_bottom,
          "submitComposer drops the viewport to the bottom (stick_bottom=true)");
}

void test_jk_block_navigation_with_streaming() {
    // Regression for the delta-rebuildViews j/k bug. The transcript has
    // multiple focusable blocks; j/k in timeline focus must move the '›'
    // marker block-by-block, and a streaming body update on the last row
    // (tail-replace) must NOT jump the marker off the selected block. The
    // delta strategy preserves the stable prefix, but the focusable-block
    // math inside the delta paths was broken: the prefix kept its old
    // '›' from the prior build and the newly-selected block didn't get
    // re-emitted with '›'. The fix tracks appliedSelectedBlock /
    // appliedTimelineFocus / appliedShowThoughts / appliedShowRaw and
    // forces a full re-emit when any of them diverge from the current
    // state, so all '›' markers are refreshed.
    InkcellAppConfig cfg;
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    scenes::AgentScene scene(cfg, bridge, model);
    scene.on_enter();
    model->timelineFocus = true;

    // Seed 5 focusable User blocks (each: header + 1-line body + blank = 3 lines).
    for (int i = 0; i < 5; ++i) {
        TimelineRow row;
        row.kind = TimelineKind::User;
        row.title = "you";
        row.body = "block " + std::to_string(i);
        model->rootRows.push_back(std::move(row));
    }
    model->rebuildViews();

    check(model->blockRowIndex.size() == 5, "5 focusable blocks seeded into blockRowIndex");
    check(model->selectedBlock == 0, "selectedBlock starts at 0 (first focusable)");
    check(model->transcriptView.lines[0].rfind("› ", 0) == 0,
          "initial '›' is on block 0 header (line 0)");
    check(model->transcriptView.lines[3].rfind("  ", 0) == 0,
          "block 1 header has no '›' initially");

    // Press j: selectDelta(+1) -> selectedBlock=1. The '›' must move
    // off block 0 and onto block 1.
    model->selectDelta(1);
    check(model->selectedBlock == 1, "j moves selectedBlock from 0 to 1");
    check(model->transcriptView.lines[0].rfind("  ", 0) == 0,
          "block 0 loses '›' after j");
    check(model->transcriptView.lines[3].rfind("› ", 0) == 0,
          "block 1 gains '›' after j");
    check(model->transcriptView.lines[6].rfind("  ", 0) == 0,
          "block 2 still has no '›' after j");

    // Press j twice more: selectedBlock=3. The '›' must move to block 3.
    model->selectDelta(1);
    model->selectDelta(1);
    check(model->selectedBlock == 3, "j j moves selectedBlock to 3");
    check(model->transcriptView.lines[9].rfind("› ", 0) == 0,
          "block 3 header has '›' after j j");
    check(model->transcriptView.lines[3].rfind("  ", 0) == 0,
          "block 1 no longer has '›'");

    // Press k: selectDelta(-1) -> selectedBlock=2.
    model->selectDelta(-1);
    check(model->selectedBlock == 2, "k moves selectedBlock from 3 to 2");
    check(model->transcriptView.lines[6].rfind("› ", 0) == 0,
          "block 2 gains '›' after k");
    check(model->transcriptView.lines[9].rfind("  ", 0) == 0,
          "block 3 loses '›' after k");

    // Simulate a streaming body update on the last row (row 4, Response-style
    // body that grows token-by-token). The tail-replace path re-emits only
    // the last row; selectedBlock=2 must stay on block 2 and the '›' on
    // block 4 (the last) must NOT appear.
    model->rootRows.back().body = "block 4 updated body (streaming token 1)";
    model->rebuildViews();
    check(model->selectedBlock == 2,
          "selectedBlock unchanged after tail-replace body update");
    check(model->transcriptView.lines[6].rfind("› ", 0) == 0,
          "'›' still on block 2 after tail-replace (selection stable)");
    check(model->transcriptView.lines[12].rfind("  ", 0) == 0,
          "block 4 (last) has no '›' after tail-replace (selectedBlock=2)");

    // Another tail-replace: body grows further. Selection still stable.
    model->rootRows.back().body = "block 4 updated body (streaming token 2 — longer)";
    model->rebuildViews();
    check(model->selectedBlock == 2,
          "selectedBlock unchanged after second tail-replace");
    check(model->transcriptView.lines[6].rfind("› ", 0) == 0,
          "'›' still on block 2 after second tail-replace");

    // j to the last block: selectedBlock=4. Now another tail-replace:
    // '›' should stay on block 4 (the last row, re-emitted correctly).
    model->selectDelta(2);
    check(model->selectedBlock == 4, "j j moves selectedBlock to 4 (last block)");
    check(model->transcriptView.lines[12].rfind("› ", 0) == 0,
          "block 4 has '›' after j j");
    model->rootRows.back().body = "block 4 updated body (streaming token 3)";
    model->rebuildViews();
    check(model->selectedBlock == 4,
          "selectedBlock unchanged on last block after tail-replace");
    check(model->transcriptView.lines[12].rfind("› ", 0) == 0,
          "'›' still on block 4 (last) after tail-replace");

    // Append a new focusable row (deltaAppend path). The new row joins
    // the end; with selectedBlock=4 (still pointing at the OLD last
    // block), the marker should remain on the now-second-to-last block,
    // and the new last block has no '›'. (In running mode, pushRow
    // would bump selectedBlock to the new last; here we test the
    // deltaAppend directly without running so we can assert the
    // selection moves correctly to a non-last target.)
    TimelineRow newRow;
    newRow.kind = TimelineKind::User;
    newRow.title = "you";
    newRow.body = "block 5";
    model->rootRows.push_back(std::move(newRow));
    model->rebuildViews();
    check(model->blockRowIndex.size() == 6, "deltaAppend added 6th focusable block");
    check(model->selectedBlock == 4, "selectedBlock still 4 after deltaAppend (old last is now second-to-last)");
    check(model->transcriptView.lines[12].rfind("› ", 0) == 0,
          "'›' still on old last (block 4) after deltaAppend");
    check(model->transcriptView.lines[15].rfind("  ", 0) == 0,
          "new last (block 5) has no '›' (selectedBlock=4)");
}

void test_ctrl_c_state() {
    InkcellAppConfig cfg;
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    scenes::AgentScene scene(cfg, bridge, model);
    model->running = true;
    g_running = true;
    scene.on_key(key(inkcell::KeyCode::CtrlC));
    check(!g_running && model->status == "cancelling", "Ctrl-C requests active turn cancellation");
    g_running = true;
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

    // Home/End jump to top/bottom.
    scene.on_key(key(inkcell::KeyCode::Home));
    check(model->transcriptView.offset == 0 && !model->transcriptView.stick_bottom,
          "Home jumps to the top of the transcript");
    scene.on_key(key(inkcell::KeyCode::End));
    check(model->transcriptView.stick_bottom && model->transcriptView.offset == bottom,
          "End re-sticks to the bottom");

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
    test_slash_and_completion();
    test_ctrl_c_state();
    test_chat_scroll_keys();
    test_ctrl_j_k_history_navigation();
    test_submit_locks_to_bottom();
    test_jk_block_navigation_with_streaming();
    std::cout << "\n" << (failures == 0 ? "all passed" : "failures: " + std::to_string(failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
