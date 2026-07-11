#pragma once
// Fixture-ready timeline block renderer. Pure view: reads model, mutates Surface only.

#include <algorithm>
#include <string>
#include <vector>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/layout/sbtui_layout.hpp"
#include "src/ui/model/timeline_model.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::views {

struct TimelineViewModel {
    std::vector<model::TimelineBlock> blocks;
    int selectedIndex = 0;
    bool focused = true;
    bool loading = false;
    std::string emptyMessage = "No turns yet. Type a prompt to begin.";
};

inline const char* blockGlyph(model::BlockKind kind, model::BlockStatus status) {
    if (status == model::BlockStatus::Error) return "✗";
    if (status == model::BlockStatus::Pending || status == model::BlockStatus::Partial) return "◐";
    switch (kind) {
        case model::BlockKind::UserPrompt: return ">";
        case model::BlockKind::Thought: return "·";
        case model::BlockKind::Action: return "◆";
        case model::BlockKind::Result: return "✓";
        case model::BlockKind::Response: return "▸";
        case model::BlockKind::Final: return "■";
        case model::BlockKind::Status: return "○";
        case model::BlockKind::Error: return "✗";
    }
    return "○";
}

inline std::string tagLine(const model::TimelineBlock& block) {
    std::string out;
    for (size_t i = 0; i < block.tags.size(); ++i) {
        if (i) out += " ";
        out += "#" + block.tags[i];
    }
    if (block.drillable) {
        if (!out.empty()) out += " ";
        out += "↳ " + (block.related.label.empty() ? block.actionName : block.related.label);
    }
    return out;
}

inline void drawEmpty(inkcell::Surface& surface, inkcell::Rect frame, const TimelineViewModel& model) {
    layout::flat_panel(surface, frame, theme::panel_bg());
    int y = frame.y + std::max(1, frame.h / 2 - 1);
    surface.text({frame.x + 2, y}, inkcell::text::truncate(model.loading ? "◐ Waiting for agent…" : model.emptyMessage,
                                                            std::max(0, frame.w - 4)),
                 model.loading ? theme::amber() : theme::dim());
    if (!model.loading)
        surface.text({frame.x + 2, y + 1}, inkcell::text::truncate("Enter sends · Esc focuses history · ? help",
                                                                    std::max(0, frame.w - 4)),
                     theme::dim());
}

inline void drawTimelineBlock(inkcell::Surface& surface, inkcell::Rect row, const model::TimelineBlock& block,
                              bool selected, bool focused) {
    auto bg = selected ? theme::panel_3() : theme::panel_2();
    layout::flat_panel(surface, row, bg);

    auto primary = selected && focused ? theme::selected_style() : selected ? theme::bright() : theme::text();
    auto secondary = selected ? theme::text() : theme::dim();

    std::string marker = selected && focused ? "> " : selected ? "  " : "  ";
    std::string head = marker + blockGlyph(block.kind, block.status) + std::string(" ") + block.title;
    std::string status = std::string(model::blockStatusName(block.status));
    if (!status.empty() && status != "idle") head += "  [" + status + "]";
    surface.text({row.x + 1, row.y}, inkcell::text::truncate(head, std::max(0, row.w - 2)), primary);

    if (row.h > 1) {
        std::string summaryPrefix = selected && focused ? "│ " : "  ";
        surface.text({row.x + 1, row.y + 1},
                     inkcell::text::truncate(summaryPrefix + block.summary, std::max(0, row.w - 2)), secondary);
    }
    if (row.h > 2) {
        std::string tags = tagLine(block);
        if (!tags.empty()) surface.text({row.x + 3, row.y + 2}, inkcell::text::truncate(tags, std::max(0, row.w - 4)),
                                        theme::dim());
    }
}

inline void drawTimeline(inkcell::Surface& surface, inkcell::Rect frame, const TimelineViewModel& model) {
    if (frame.w <= 0 || frame.h <= 0) return;
    if (model.blocks.empty()) {
        drawEmpty(surface, frame, model);
        return;
    }

    layout::flat_panel(surface, frame, theme::panel_bg());
    int y = frame.y;
    int selected = std::max(0, std::min(model.selectedIndex, static_cast<int>(model.blocks.size()) - 1));
    const int blockStride = 4;  // 3-row block + 1-row gutter
    const int visibleBlocks = std::max(1, (frame.h + 1) / blockStride);
    int start = 0;
    if (static_cast<int>(model.blocks.size()) > visibleBlocks) {
        if (model.focused) {
            start = std::max(0, std::min(selected - visibleBlocks / 2,
                                         static_cast<int>(model.blocks.size()) - visibleBlocks));
        } else {
            // Follow-live/default snapshot behavior: newest blocks are visible.
            start = std::max(0, static_cast<int>(model.blocks.size()) - visibleBlocks);
        }
    }
    for (int i = start; i < static_cast<int>(model.blocks.size()) && y < frame.bottom(); ++i) {
        int h = std::min(3, frame.bottom() - y);
        drawTimelineBlock(surface, {frame.x, y, frame.w, h}, model.blocks[static_cast<size_t>(i)],
                          i == selected, model.focused);
        y += h + 1;
    }
}

}  // namespace cortex::mk3::ui::views
