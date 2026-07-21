#pragma once
// Floating bottom pill dock — frontend-grade terminal chrome.
// Cortex-local. Uses inkcell Surface.box (rounded) + per-segment fills.
//
// Visual model (3 rows tall):
//   [spacer float]
//   ╭── Overview ── Sessions ── Manifests ── ... ──╮   ← rounded capsule
//   │              ████ active slider ████          │   ← interior track + thumb
//   ╰──────────────────────────────────────────────╯

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::components {

struct PillItem {
    std::string key;    // single glyph shortcut
    std::string label;
};

inline float easeOutCubic(float t) {
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    float u = 1.f - t;
    return 1.f - u * u * u;
}

inline float easeInOut(float t) {
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    return t < 0.5f ? 4.f * t * t * t : 1.f - std::pow(-2.f * t + 2.f, 3.f) / 2.f;
}

// Interpolate RGB
inline inkcell::Color lerpColor(inkcell::Color a, inkcell::Color b, float t) {
    t = std::max(0.f, std::min(1.f, t));
    auto L = [&](uint8_t x, uint8_t y) -> uint8_t {
        return static_cast<uint8_t>(x + (y - x) * t);
    };
    return inkcell::Color::rgb(L(a.r, b.r), L(a.g, b.g), L(a.b, b.b));
}

struct PillTheme {
    inkcell::Color shellBg;
    inkcell::Color shellFg;
    inkcell::Color border;
    inkcell::Color idleFg;
    inkcell::Color activeFg;
    inkcell::Color activeBg;
    inkcell::Color thumb;
    inkcell::Color track;
};

inline PillTheme pillTheme() {
    // Graphite default; neon lifts cyan/green.
    if (theme::activeVariant == theme::Variant::Neon) {
        return PillTheme{
            inkcell::Color::rgb(12, 16, 26),   // shellBg
            inkcell::Color::rgb(180, 195, 220),
            inkcell::Color::rgb(40, 56, 84),   // border
            inkcell::Color::rgb(110, 124, 150),
            inkcell::Color::rgb(10, 18, 24),   // activeFg on bright thumb
            inkcell::Color::rgb(90, 220, 255), // activeBg
            inkcell::Color::rgb(90, 220, 255), // thumb
            inkcell::Color::rgb(22, 30, 46),   // track
        };
    }
    return PillTheme{
        inkcell::Color::rgb(28, 28, 28),
        inkcell::Color::rgb(200, 200, 200),
        inkcell::Color::rgb(58, 58, 58),
        inkcell::Color::rgb(120, 120, 120),
        inkcell::Color::rgb(18, 18, 18),
        inkcell::Color::rgb(210, 210, 210),
        inkcell::Color::rgb(175, 185, 190),
        inkcell::Color::rgb(36, 36, 36),
    };
}

// Measure segment widths (key + label).
inline std::vector<int> measureSegs(const std::vector<PillItem>& items) {
    std::vector<int> w;
    w.reserve(items.size());
    for (const auto& it : items) {
        // "  o  Overview  "
        std::string s = "  " + it.key + "  " + it.label + "  ";
        w.push_back(inkcell::text::display_width(s));
    }
    return w;
}

// Returns height consumed (always 3 when drawn).
inline int drawPillDock(inkcell::Surface& s, int pageX, int pageW, int bottomY,
                        const std::vector<PillItem>& items, int current, int previous,
                        float animT) {
    if (items.empty() || pageW < 30) return 0;

    const PillTheme T = pillTheme();
    const auto segs = measureSegs(items);
    int inner = 0;
    for (int w : segs) inner += w;
    // gaps between segs already baked into padding; no extra

    const int hPad = 1;   // inside border
    const int boxH = 3;   // border + content + border  → actually box is 3 rows: top border, content, bottom border
    // We want: top border row with labels INSIDE on middle... surface.box draws border on edge.
    // Layout:
    //   y0: ╭──── labels live on y1 ────╮
    //   y1: │  seg seg seg              │
    //   y2: ╰──── thumb track ──────────╯  wait thumb on y2 inside?

    // Better 3-row interior:
    // Use a 4-row dock: float gap is external.
    // y0: top border
    // y1: labels  
    // y2: track + sliding thumb
    // y3: bottom border
    // That's 4. User wanted long pill - 3 total is tighter:
    // y0: ╭ labels ╮  with labels on same row as border corners (custom draw)
    // y1: │ thumb  │
    // y2: ╰────────╯

    const int dockH = 3;
    int boxW = std::min(pageW - 2, inner + 2 + hPad * 2);
    // If too wide for terminal, shrink by dropping keys first... keep labels
    if (boxW < 20) boxW = std::min(pageW - 2, 20);
    int boxX = pageX + std::max(0, (pageW - boxW) / 2);
    int boxY = bottomY - (dockH - 1);  // bottomY is last row

    inkcell::Rect box{boxX, boxY, boxW, dockH};

    // Drop shadow (right+down 1) — subtle depth
    if (box.right() < pageX + pageW && box.bottom() <= bottomY + 1) {
        auto sh = inkcell::Style::normal()
                      .with_bg(theme::color(inkcell::Color::rgb(10, 10, 10),
                                            inkcell::Color::rgb(2, 4, 8)))
                      .with_fg(theme::color(inkcell::Color::rgb(10, 10, 10),
                                            inkcell::Color::rgb(2, 4, 8)));
        // bottom shadow line
        if (box.bottom() < s.bounds().bottom())
            s.fill({box.x + 1, box.bottom(), box.w - 1, 1}, " ", sh);
    }

    // Shell fill
    auto shell = inkcell::Style::normal().with_bg(T.shellBg).with_fg(T.border);
    s.fill(box, " ", shell);

    // Rounded border
    s.box(box, inkcell::BorderStyle::Rounded, shell.with_fg(T.border));

    // Content area inside border
    int cx0 = box.x + 1;
    int cyLabel = box.y + 0;  // labels share top row with corners — put on y+1 if h>=3
    // With h=3: row0=top border, row1=content, row2=bottom border
    // Put labels on row1, thumb as underline glyphs on row1 bottom... can't.
    // Put labels on row1 and draw thumb as block characters replacing bottom border segment.

    cyLabel = box.y + 1;
    int contentW = box.w - 2;
    int x = cx0;

    // Compute segment x ranges
    struct SegGeom {
        int x0, x1, w;
    };
    std::vector<SegGeom> geom;
    geom.reserve(items.size());
    int cursor = cx0;
    // Center the group inside content
    int used = 0;
    for (int w : segs) used += w;
    if (used < contentW) cursor = cx0 + (contentW - used) / 2;

    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        SegGeom g;
        g.x0 = cursor;
        g.w = segs[static_cast<size_t>(i)];
        g.x1 = cursor + g.w;
        geom.push_back(g);
        cursor += g.w;
    }

    // Animated thumb position (lerp previous → current)
    float e = easeInOut(animT);
    int thumbX0 = geom[static_cast<size_t>(std::max(0, current))].x0;
    int thumbW = geom[static_cast<size_t>(std::max(0, current))].w;
    if (previous >= 0 && previous < static_cast<int>(geom.size()) && animT < 1.f) {
        int px = geom[static_cast<size_t>(previous)].x0;
        int pw = geom[static_cast<size_t>(previous)].w;
        thumbX0 = static_cast<int>(std::lround(px + (thumbX0 - px) * e));
        thumbW = static_cast<int>(std::lround(pw + (thumbW - pw) * e));
    }
    // Clamp thumb inside content
    if (thumbX0 < cx0) thumbX0 = cx0;
    if (thumbX0 + thumbW > cx0 + contentW) thumbW = std::max(1, cx0 + contentW - thumbX0);

    // Draw sliding thumb behind labels on content row
    auto thumbSt = inkcell::Style::normal().with_bg(T.activeBg).with_fg(T.activeFg);
    s.fill({thumbX0, cyLabel, thumbW, 1}, " ", thumbSt);

    // Soft track remnant on bottom border under thumb (active indicator)
    auto trackSt = inkcell::Style::normal().with_bg(T.shellBg).with_fg(T.thumb);
    // Overwrite bottom border center under thumb with heavy underscore glow
    if (box.h >= 3) {
        std::string bar;
        for (int i = 0; i < thumbW; ++i) bar += "▀";
        s.text({thumbX0, box.y + 2}, bar, trackSt.with_fg(T.thumb).with_bg(T.shellBg));
        // restore corners if overwritten
        s.text({box.x, box.y + 2}, "╰", shell.with_fg(T.border));
        s.text({box.right() - 1, box.y + 2}, "╯", shell.with_fg(T.border));
    }

    // Labels
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const auto& it = items[static_cast<size_t>(i)];
        const auto& g = geom[static_cast<size_t>(i)];
        bool onThumb = (g.x0 + g.w / 2) >= thumbX0 && (g.x0 + g.w / 2) < thumbX0 + thumbW;
        float hi = 0.f;
        if (i == current) hi = 0.35f + 0.65f * e;
        else if (i == previous && animT < 1.f) hi = 0.55f * (1.f - e);

        std::string label = "  " + it.key + "  " + it.label + "  ";
        inkcell::Style st;
        if (onThumb || hi > 0.5f) {
            st = thumbSt;
            st.bold = true;
        } else {
            st = inkcell::Style::normal().with_bg(T.shellBg).with_fg(T.idleFg);
            if (hi > 0.15f) {
                // fading segment
                st.fg = lerpColor(T.idleFg, T.activeBg, hi);
                st.bold = true;
            }
        }
        // clip label to segment width
        s.text({g.x0, cyLabel}, inkcell::text::truncate(label, g.w), st);
    }

    // Re-draw left/right border on content row (labels may have spilled)
    s.text({box.x, cyLabel}, "│", shell.with_fg(T.border));
    s.text({box.right() - 1, cyLabel}, "│", shell.with_fg(T.border));
    // top corners + edge
    s.text({box.x, box.y}, "╭", shell.with_fg(T.border));
    s.text({box.right() - 1, box.y}, "╮", shell.with_fg(T.border));
    for (int xh = box.x + 1; xh < box.right() - 1; ++xh) {
        // don't stomp top if we want clean line
        s.put({xh, box.y}, "─", shell.with_fg(T.border));
    }

    (void)x;
    return dockH;
}

// Compact status above the pill (one row).
inline void drawStatusRail(inkcell::Surface& s, int pageX, int pageW, int y,
                           const std::string& left, const std::string& right) {
    auto st = theme::dim();
    s.text({pageX, y}, inkcell::text::truncate(left, std::max(8, pageW * 2 / 3)), st);
    if (!right.empty()) {
        int rw = inkcell::text::display_width(right);
        s.text({std::max(pageX, pageX + pageW - rw), y},
               inkcell::text::truncate(right, pageW / 3), st);
    }
}

}  // namespace cortex::mk3::ui::components
