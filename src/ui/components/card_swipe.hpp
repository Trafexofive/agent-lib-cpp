#pragma once
// Curved card swipe transition for Manifest hub detail.
// Two cards: outgoing arcs off along a sine path, incoming arcs on from the
// opposite side. Terminal-grade "frontend" motion without real bezier fill.

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::components {

inline float easeOutCubic(float t) {
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    float u = 1.f - t;
    return 1.f - u * u * u;
}

inline float easeInOutCubic(float t) {
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    return t < 0.5f ? 4.f * t * t * t : 1.f - std::pow(-2.f * t + 2.f, 3.f) / 2.f;
}

// Smoothstep for soft ends on the arc.
inline float smoothstep(float t) {
    t = std::max(0.f, std::min(1.f, t));
    return t * t * (3.f - 2.f * t);
}

struct CardPose {
    int x = 0;
    int y = 0;
    float alpha = 1.f;  // 0..1 — drives dim/bold, not real alpha
};

// dir: +1 means navigating "next" (outgoing exits left? or right?)
// Convention: next (j) → incoming from right, outgoing exits left (like page turn).
// prev (k) → incoming from left, outgoing exits right.
// t: 0 = start of swipe, 1 = settled on new card.
inline CardPose outgoingPose(inkcell::Rect clip, int dir, float t) {
    float e = easeInOutCubic(t);
    float arc = std::sin(3.14159265f * smoothstep(t));  // 0→1→0 bump
    int travel = std::max(6, clip.w + 2);
    CardPose p;
    // Outgoing moves opposite to incoming arrival side
    p.x = clip.x - dir * static_cast<int>(std::lround(e * travel));
    p.y = clip.y - static_cast<int>(std::lround(arc * std::min(3.f, clip.h * 0.12f)));
    p.alpha = 1.f - e;
    return p;
}

inline CardPose incomingPose(inkcell::Rect clip, int dir, float t) {
    float e = easeOutCubic(t);
    float arc = std::sin(3.14159265f * smoothstep(1.f - t));  // starts high-ish, settles
    // At t=0 incoming is off-screen on the dir side; at t=1 at rest
    int travel = std::max(6, clip.w + 2);
    float remain = 1.f - e;
    CardPose p;
    p.x = clip.x + dir * static_cast<int>(std::lround(remain * travel));
    p.y = clip.y - static_cast<int>(std::lround(arc * std::min(3.f, clip.h * 0.12f)));
    p.alpha = 0.35f + 0.65f * e;
    return p;
}

// Draw a rounded card shell at pose, clipped loosely to `clip` bounds.
// body(s, innerRect, alpha) paints content inside the card chrome.
inline void drawSwipedCard(inkcell::Surface& s, inkcell::Rect clip, CardPose pose, int cardW,
                           int cardH, float accentHue /*0=cyan 1=violet*/,
                           const std::function<void(inkcell::Surface&, inkcell::Rect, float)>& body) {
    cardW = std::min(cardW, clip.w);
    cardH = std::min(cardH, clip.h);
    if (cardW < 8 || cardH < 4) return;

    inkcell::Rect card{pose.x, pose.y, cardW, cardH};

    // Skip if completely outside clip (rough)
    if (card.right() <= clip.x - 2 || card.x >= clip.right() + 2) return;
    if (card.bottom() <= clip.y - 2 || card.y >= clip.bottom() + 2) return;

    // Intersect draw region with surface
    auto base = theme::panel_2();
    // Fade via dim when alpha low
    bool ghost = pose.alpha < 0.55f;

    // Shadow under card (offset +1,+1) — only when mostly visible
    if (pose.alpha > 0.4f) {
        auto sh = inkcell::Style::normal()
                      .with_bg(theme::color(inkcell::Color::rgb(6, 6, 8), inkcell::Color::rgb(2, 3, 6)))
                      .with_fg(theme::color(inkcell::Color::rgb(6, 6, 8), inkcell::Color::rgb(2, 3, 6)));
        inkcell::Rect shadow{card.x + 1, card.y + 1, card.w, card.h};
        // clip fill manually by only filling overlapping rows with clip
        for (int y = std::max(shadow.y, clip.y); y < std::min(shadow.bottom(), clip.bottom()); ++y) {
            int x0 = std::max(shadow.x, clip.x);
            int x1 = std::min(shadow.right(), clip.right());
            if (x1 > x0) s.fill({x0, y, x1 - x0, 1}, " ", sh);
        }
    }

    // Card body fill (clipped)
    for (int y = std::max(card.y, clip.y); y < std::min(card.bottom(), clip.bottom()); ++y) {
        int x0 = std::max(card.x, clip.x);
        int x1 = std::min(card.right(), clip.right());
        if (x1 > x0) s.fill({x0, y, x1 - x0, 1}, " ", base);
    }

    // Border color by accent + ghost
    inkcell::Color bd =
        accentHue > 0.5f
            ? theme::color(inkcell::Color::rgb(120, 105, 155), inkcell::Color::rgb(140, 120, 210))
            : theme::color(inkcell::Color::rgb(70, 100, 110), inkcell::Color::rgb(50, 120, 150));
    if (ghost)
        bd = theme::color(inkcell::Color::rgb(55, 55, 62), inkcell::Color::rgb(40, 50, 70));

    auto border = base.with_fg(bd);
    // Draw rounded corners only if those cells are inside clip
    auto putIf = [&](int x, int y, const char* g) {
        if (x >= clip.x && x < clip.right() && y >= clip.y && y < clip.bottom())
            s.text({x, y}, g, border);
    };
    putIf(card.x, card.y, "╭");
    putIf(card.right() - 1, card.y, "╮");
    putIf(card.x, card.bottom() - 1, "╰");
    putIf(card.right() - 1, card.bottom() - 1, "╯");
    for (int x = card.x + 1; x < card.right() - 1; ++x) {
        putIf(x, card.y, "─");
        putIf(x, card.bottom() - 1, "─");
    }
    for (int y = card.y + 1; y < card.bottom() - 1; ++y) {
        putIf(card.x, y, "│");
        putIf(card.right() - 1, y, "│");
    }

    // Accent top hairline inside card
    if (pose.alpha > 0.5f) {
        auto ac = accentHue > 0.5f ? theme::violet_soft() : theme::cyan_soft();
        ac = ac.with_bg(base.bg);
        int y = card.y + 1;
        if (y >= clip.y && y < clip.bottom()) {
            for (int x = card.x + 2; x < card.right() - 2; ++x)
                if (x >= clip.x && x < clip.right()) s.text({x, y}, "─", ac);
        }
    }

    inkcell::Rect inner{card.x + 2, card.y + 2, std::max(1, card.w - 4), std::max(1, card.h - 4)};
    // Further clip inner to clip rect for body painter
    int iy0 = std::max(inner.y, clip.y);
    int iy1 = std::min(inner.bottom(), clip.bottom());
    int ix0 = std::max(inner.x, clip.x);
    int ix1 = std::min(inner.right(), clip.right());
    if (iy1 > iy0 && ix1 > ix0) {
        inkcell::Rect visible{ix0, iy0, ix1 - ix0, iy1 - iy0};
        body(s, visible, pose.alpha);
    }
}

// List-row nudge during swipe (selected row slides a few cells with arc).
inline int listNudgeX(int dir, float t, int maxNudge = 3) {
    float e = easeOutCubic(std::min(1.f, t * 1.4f));
    // settle back: overshoot then rest — use sin for curve
    float wave = std::sin(3.14159265f * e);
    return dir * static_cast<int>(std::lround(wave * maxNudge));
}

}  // namespace cortex::mk3::ui::components
