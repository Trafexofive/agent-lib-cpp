#pragma once
// Live workflow run overlay / docked panel.
// Header + rail + event tick strip. AAA density, no filler chrome.

#include <algorithm>
#include <cstdio>
#include <string>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/components/chrome.hpp"
#include "src/ui/components/workflow_rail.hpp"
#include "src/ui/model/workflow_run_model.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::components {

inline inkcell::Style runStatusChipStyle(model::RunStatus st) {
    using model::RunStatus;
    switch (st) {
        case RunStatus::Starting:
        case RunStatus::Running:
            return theme::cyan();
        case RunStatus::Hitl:
            return theme::amber();
        case RunStatus::Succeeded:
            return theme::green();
        case RunStatus::Failed:
            return theme::red();
        case RunStatus::Cancelled:
            return theme::amber_soft();
        case RunStatus::Idle:
        default:
            return theme::dim();
    }
}

inline std::string formatRunElapsed(double ms) {
    char buf[32];
    if (ms < 1000.0)
        std::snprintf(buf, sizeof(buf), "%.0fms", ms);
    else
        std::snprintf(buf, sizeof(buf), "%.1fs", ms / 1000.0);
    return buf;
}

// Full live panel into `r`. Returns false if nothing to draw (idle empty).
inline bool drawWorkflowRunPanel(inkcell::Surface& s, inkcell::Rect r,
                                 const model::WorkflowRunState& run, float tSec = 0.f) {
    if (r.w < 20 || r.h < 4) return false;
    if (run.status == model::RunStatus::Idle && run.steps.empty()) return false;

    // Well
    s.fill(r, " ", theme::panel_bg());
    // Top accent hair
    hairline(s, r.x, r.y, r.w, theme::cyan());

    int y = r.y + 1;
    int x = r.x + 1;
    int w = r.w - 2;

    // Header: name · runId · elapsed · STATUS
    {
        std::string left = run.name.empty() ? "workflow" : run.name;
        if (!run.runId.empty()) left += "  ·  " + run.runId;
        auto stChip = std::string(model::runStatusLabel(run.status));
        std::string right = formatRunElapsed(run.elapsedMs) + "  " + stChip;

        s.text({x, y}, inkcell::text::truncate(left, std::max(8, w - 16)), theme::bright());
        int rw = inkcell::text::display_width(right);
        auto rst = runStatusChipStyle(run.status);
        rst.bold = true;
        s.text({std::max(x, r.right() - rw - 2), y},
               inkcell::text::truncate(right, w), rst);
        ++y;
    }

    if (!run.lastError.empty() && (run.status == model::RunStatus::Failed ||
                                   run.status == model::RunStatus::Cancelled)) {
        if (y < r.bottom()) {
            s.text({x, y++},
                   inkcell::text::truncate(std::string("▸ ") + run.lastError, w),
                   theme::red());
        }
    }

    if (y >= r.bottom()) return true;

    // Split: rail (top) + events (bottom strip)
    int eventH = std::min(4, std::max(2, r.h / 5));
    int railH = std::max(1, r.bottom() - y - eventH - 1);
    inkcell::Rect rail{x, y, w, railH};
    drawWorkflowRail(s, rail, run.steps, run.selectedStep, run.currentIdx, tSec);
    y = rail.bottom();

    // Event tick strip
    if (y < r.bottom() && eventH > 0) {
        hairline(s, x, y, w, theme::dim());
        ++y;
        int start = 0;
        int n = static_cast<int>(run.events.size());
        int vis = std::max(1, r.bottom() - y);
        if (n > vis) start = n - vis;
        for (int i = start; i < n && y < r.bottom(); ++i) {
            const auto& ev = run.events[static_cast<size_t>(i)];
            auto st = theme::dim();
            if (ev.kind.find("fail") != std::string::npos)
                st = theme::red();
            else if (ev.kind == "step.ok" || ev.kind == "done")
                st = theme::green_soft();
            else if (ev.kind == "hitl" || ev.kind == "step.enter")
                st = theme::cyan();
            else if (ev.kind == "checkpoint" || ev.kind.find("skip") != std::string::npos)
                st = theme::amber_soft();
            std::string line = ev.kind;
            if (!ev.text.empty()) line += "  " + ev.text;
            s.text({x, y++}, inkcell::text::truncate(line, w), st);
        }
    }
    return true;
}

// Compact status chip for list rows / home KPI.
inline std::string runStatusBrief(const model::WorkflowRunState& run) {
    if (run.status == model::RunStatus::Idle) return {};
    std::string s = run.name.empty() ? "wf" : run.name;
    s += " · ";
    s += model::runStatusLabel(run.status);
    if (run.currentIdx >= 0 && run.currentIdx < static_cast<int>(run.steps.size()))
        s += " · " + run.steps[static_cast<size_t>(run.currentIdx)].id;
    return s;
}

}  // namespace cortex::mk3::ui::components
