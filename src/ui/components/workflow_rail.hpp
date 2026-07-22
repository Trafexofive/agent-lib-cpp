#pragma once
// Vertical workflow step rail — zen status rail for hub detail + live run.
// Cell-space only; pulse via precomputed phase (no per-cell sin storms).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/model/workflow_run_model.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::components {

inline std::string formatStepMs(double ms) {
    if (ms <= 0.0) return {};
    char buf[24];
    if (ms < 1000.0) {
        std::snprintf(buf, sizeof(buf), "%.0fms", ms);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1fs", ms / 1000.0);
    }
    return buf;
}

inline inkcell::Style stepStatusStyle(model::StepStatus st, bool selected, bool pulseOn) {
    using model::StepStatus;
    inkcell::Style s = theme::muted();
    switch (st) {
        case StepStatus::Pending:
            s = selected ? theme::text() : theme::dim();
            break;
        case StepStatus::Running:
            s = pulseOn ? theme::cyan() : theme::amber();
            s.bold = true;
            break;
        case StepStatus::Ok:
            s = theme::green();
            break;
        case StepStatus::Fail:
            s = theme::red();
            s.bold = true;
            break;
        case StepStatus::Skip:
            s = theme::italic_dim();
            break;
    }
    if (selected && st != StepStatus::Running) s.bold = true;
    return s;
}

// Draw vertical step rail. Returns next y.
// tSec drives running-step pulse (pass 0 for static).
inline int drawWorkflowRail(inkcell::Surface& s, inkcell::Rect r,
                            const std::vector<model::WorkflowStepView>& steps, int selected,
                            int currentIdx, float tSec = 0.f) {
    if (r.w < 8 || r.h < 1 || steps.empty()) return r.y;

    // Pulse phase: ~1.6s breath
    const float phase = tSec > 0.f ? std::fmod(tSec * 0.625f, 1.f) : 0.f;
    const bool pulseOn = phase < 0.55f;

    int y = r.y;
    const int n = static_cast<int>(steps.size());
    const int maxRows = r.h;
    int start = 0;
    if (n > maxRows) {
        int focus = selected >= 0 ? selected : (currentIdx >= 0 ? currentIdx : 0);
        start = std::max(0, std::min(focus - maxRows / 3, n - maxRows));
    }

    for (int i = start; i < n && y < r.bottom(); ++i) {
        const auto& st = steps[static_cast<size_t>(i)];
        const bool sel = (i == selected);
        const bool cur = (i == currentIdx);

        auto bg = sel ? theme::panel_3() : theme::panel_bg();
        s.fill({r.x, y, r.w, 1}, " ", bg);

        if (sel) s.text({r.x, y}, "▌", theme::cyan().with_bg(bg.bg));

        // Connector glyph between steps (flow on current edge)
        std::string g = model::stepStatusGlyph(st.status);
        auto gst = stepStatusStyle(st.status, sel || cur, pulseOn).with_bg(bg.bg);
        s.text({r.x + 2, y}, g, gst);

        // Index
        char idxBuf[8];
        std::snprintf(idxBuf, sizeof(idxBuf), "%02d", i + 1);
        auto idxSt = (sel ? theme::bright() : theme::dim()).with_bg(bg.bg);
        s.text({r.x + 4, y}, idxBuf, idxSt);

        // Name + type chip
        std::string label = st.name.empty() ? st.id : st.name;
        int nameX = r.x + 7;
        int rightReserve = 14;  // status + timing
        int nameW = std::max(6, r.w - (nameX - r.x) - rightReserve);
        auto nameSt = (sel ? theme::bright() : theme::text()).with_bg(bg.bg);
        if (cur && st.status == model::StepStatus::Running) nameSt = gst;
        s.text({nameX, y}, inkcell::text::truncate(label, nameW), nameSt);

        // Status label right-aligned-ish
        std::string right = model::stepStatusLabel(st.status);
        std::string ms = formatStepMs(st.ms);
        if (!ms.empty()) right += "  " + ms;
        int rw = inkcell::text::display_width(right);
        int rx = std::max(nameX + nameW + 1, r.right() - rw - 1);
        auto rst = stepStatusStyle(st.status, sel, pulseOn).with_bg(bg.bg);
        if (st.status == model::StepStatus::Pending) rst = theme::dim().with_bg(bg.bg);
        s.text({rx, y}, inkcell::text::truncate(right, r.right() - rx), rst);

        ++y;
    }
    return y;
}

// Compact topology one-liner: "6 steps · 2 branches · HITL · ckpt"
inline std::string topologyLine(const model::WorkflowTopology& t) {
    std::string s = std::to_string(t.stepCount) + " steps";
    if (t.branchCount > 0) s += " · " + std::to_string(t.branchCount) + " branches";
    if (t.hasHitl) s += " · HITL";
    if (t.hasCheckpoint) s += " · ckpt";
    return s;
}

// Selected step inspector block. Returns next y.
inline int drawStepInspector(inkcell::Surface& s, inkcell::Rect r,
                             const model::WorkflowStepView& st) {
    if (r.h < 1 || r.w < 12) return r.y;
    int y = r.y;
    auto head = theme::violet_soft();
    s.text({r.x, y++}, "STEP", head);
    if (y >= r.bottom()) return y;

    auto line = [&](const char* k, const std::string& v) {
        if (y >= r.bottom() || v.empty()) return;
        std::string key = k;
        while (inkcell::text::display_width(key) < 10) key.push_back(' ');
        s.text({r.x, y}, inkcell::text::truncate(key, 10), theme::italic_dim());
        s.text({r.x + 11, y}, inkcell::text::truncate(v, std::max(1, r.w - 11)), theme::text());
        ++y;
    };

    line("id", st.id);
    line("type", st.type);
    line("ref", st.ref);
    line("status", model::stepStatusLabel(st.status));
    if (st.ms > 0.0) line("elapsed", formatStepMs(st.ms));
    if (!st.summary.empty()) line("io", st.summary);
    if (st.human) line("flags", "HITL");
    if (st.checkpoint) line("flags", "checkpoint");
    return y;
}

}  // namespace cortex::mk3::ui::components
