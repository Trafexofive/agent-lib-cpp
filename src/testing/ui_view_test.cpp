#include <iostream>
#include <string>
#include <vector>

#include "inkcell/surface.hpp"
#include "src/ui/chat/chat_view.hpp"
#include "src/ui/views/timeline_view.hpp"

using namespace cortex::mk3::ui;
using namespace cortex::mk3::ui::model;
using namespace cortex::mk3::ui::views;

namespace {
int failures = 0;

void check(bool cond, const std::string& name) {
    if (cond) std::cout << "  " << name << "... PASS\n";
    else {
        std::cout << "  " << name << "... FAIL\n";
        ++failures;
    }
}

std::string rowText(const inkcell::Surface& s, int y) {
    std::string out;
    for (int x = 0; x < s.width(); ++x) out += s.at({x, y}).glyph;
    return out;
}

bool containsRow(const inkcell::Surface& s, const std::string& needle) {
    for (int y = 0; y < s.height(); ++y)
        if (rowText(s, y).find(needle) != std::string::npos) return true;
    return false;
}

TimelineBlock block(BlockKind kind, BlockStatus status, std::string title, std::string summary) {
    TimelineBlock b;
    b.kind = kind;
    b.status = status;
    b.title = std::move(title);
    b.summary = std::move(summary);
    b.hasDetail = true;
    return b;
}

void test_empty_state() {
    inkcell::Surface s({100, 24});
    s.clear(theme::base_bg());
    TimelineViewModel model;
    drawTimeline(s, {2, 2, 96, 10}, model);
    check(containsRow(s, "No turns yet"), "empty state message rendered");
    check(containsRow(s, "Enter sends"), "empty state key hint rendered");
    check(rowText(s, 2).substr(0, 2) == "  ", "empty state preserves left edge inset");
}

void test_selected_block_cues() {
    inkcell::Surface s({100, 24});
    s.clear(theme::base_bg());
    TimelineViewModel model;
    model.focused = true;
    model.selectedIndex = 1;
    model.blocks.push_back(block(BlockKind::Action, BlockStatus::Pending, "tool:fs_read #a1", "reading"));
    model.blocks.back().tags = {"tool", "sync"};
    model.blocks.push_back(block(BlockKind::Result, BlockStatus::Ok, "result #a1 fs_read", "read 42 bytes"));
    model.blocks.back().tags = {"ok", "fs_read"};
    drawTimeline(s, {2, 2, 96, 12}, model);
    check(containsRow(s, "◐ tool:fs_read #a1  [pending]"), "pending action glyph/status rendered");
    check(containsRow(s, "> ✓ result #a1 fs_read  [ok]"), "selected result has marker and glyph");
    check(containsRow(s, "│ read 42 bytes"), "selected body continuation marker rendered");
    check(containsRow(s, "#ok #fs_read"), "tags rendered");
}

void test_drillable_tag() {
    inkcell::Surface s({100, 24});
    s.clear(theme::base_bg());
    TimelineBlock b = block(BlockKind::Action, BlockStatus::Pending, "agent:reader #r1", "inspect");
    b.drillable = true;
    b.related.label = "reader";
    b.tags = {"agent"};
    TimelineViewModel model;
    model.blocks.push_back(b);
    drawTimeline(s, {2, 2, 96, 8}, model);
    check(containsRow(s, "↳ reader"), "drillable child target rendered");
}

void test_chat_transcript_wraps_long_lines() {
    std::vector<std::string> source = {
        "  This response is deliberately longer than the available transcript width and must wrap.",
    };
    auto lines = chat::wrapTranscript(source, 32);
    check(lines.size() >= 3, "chat transcript wraps long response");
    bool preservedIndent = true;
    for (const auto& line : lines) preservedIndent = preservedIndent && line.rfind("  ", 0) == 0;
    check(preservedIndent, "chat transcript preserves indentation while wrapping");

    std::string token(90, 'x');
    auto tokenLines = chat::wrapTranscript({"    " + token}, 24);
    std::string reconstructed;
    for (const auto& line : tokenLines) reconstructed += line.substr(std::min<size_t>(4, line.size()));
    check(reconstructed == token, "chat wrapping never truncates long tokens");

    auto semantic = chat::wrapTranscript({"  AGENT  reader  #ping  ↳"}, 40);
    check(semantic.size() == 1 && semantic[0] == "  AGENT  reader  #ping  ↳",
          "chat wrapping preserves semantic header spacing");

    auto code = chat::wrapTranscript({"    ```cpp", "    int  x = 1;", "    ```"}, 40);
    check(code.size() == 3 && code[0].find("┌─ cpp") != std::string::npos &&
              code[1].find("│ int  x = 1;") != std::string::npos &&
              code[2].find("└─") != std::string::npos,
          "chat code fences preserve code whitespace");
}

void test_chat_prompt_cursor_position() {
    inkcell::Surface s({40, 8});
    chat::ChatSurfaceModel model;
    model.input = "abcd";
    model.inputCursor = 2;
    model.inputFocused = true;
    chat::drawChatSurface(s, {0, 0, 40, 8}, model);
    check(containsRow(s, "› ab█cd"), "chat prompt renders cursor at model position");
}

void test_chat_help_and_theme() {
    inkcell::Surface s({100, 30});
    theme::set(theme::Variant::Graphite);
    check(std::string(theme::name()) == "graphite", "graphite is the default chat theme");
    chat::drawHelpOverlay(s, {2, 2, 96, 26});
    check(containsRow(s, "CHAT HELP"), "chat help overlay renders on demand");
    check(containsRow(s, "T           switch graphite / neon"),
          "chat help documents theme switching");
    theme::toggle();
    check(std::string(theme::name()) == "neon", "chat theme toggles to neon");
    theme::set(theme::Variant::Graphite);
}

void test_ask_dialog_overlay() {
    inkcell::Surface s({100, 30});
    Json::Value params;
    params["chainTitle"] = "Select worker";
    Json::Value card;
    card["id"] = "worker";
    card["type"] = "choice";
    card["title"] = "Worker";
    card["message"] = "Choose one worker.";
    card["options"].append("reader");
    card["options"].append("tester");
    params["cards"].append(card);
    auto state = chat::parseDialogState(params);
    chat::drawAskDialog(s, {2, 2, 96, 26}, state, "", {});
    check(containsRow(s, "Select worker"), "ask overlay renders chain title");
    check(containsRow(s, "> reader"), "ask overlay renders selected choice");
    check(containsRow(s, "Enter choose"), "ask overlay renders interaction hint");
}
}  // namespace

int main() {
    std::cout << "UI view tests\n";
    test_empty_state();
    test_selected_block_cues();
    test_drillable_tag();
    test_chat_transcript_wraps_long_lines();
    test_chat_prompt_cursor_position();
    test_chat_help_and_theme();
    test_ask_dialog_overlay();
    std::cout << "\n" << (failures == 0 ? "all passed" : "failures: " + std::to_string(failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
