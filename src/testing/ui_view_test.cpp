#include <iostream>
#include <string>
#include <vector>

#include "inkcell/surface.hpp"
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
}  // namespace

int main() {
    std::cout << "UI view tests\n";
    test_empty_state();
    test_selected_block_cues();
    test_drillable_tag();
    std::cout << "\n" << (failures == 0 ? "all passed" : "failures: " + std::to_string(failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
