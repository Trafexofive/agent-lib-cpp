#pragma once
// Curved card swipe — soft, phased motion (not rigid lockstep).
// X uses smooth ease-out, Y uses a delayed sine arc, alpha eases separately.
// Incoming lags slightly so the stack reads as two cards, not a hard cut.

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::components {

// ── Easing ───────────────────────────────────────────────────────────
inline float clamp01(float t) { return t < 0.f ? 0.f : (t > 1.f ? 1.f : t); }

inline float smootherstep(float t) {
    t = clamp01(t);
    // Ken Perlin's smootherstep — flatter ends, silkier mid
    return t * t * t * (t * (t * 6.f - 15.f) + 10.f);
}

inline float easeOutQuint(float t) {
    t = clamp01(t);
    float u = 1.f - t;
    return 1.f - u * u * u * u * u;
}

inline float easeInOutCubic(float t) {
    t = clamp01(t);
    return t < 0.5f ? 4.f * t * t * t : 1.f - std::pow(-2.f * t + 2.f, 3.f) / 2.f;
}

// Map global t into a delayed local 0..1 window.
inline float phase(float t, float delay, float span = 1.f) {
    if (span <= 0.f) return 1.f;
    return clamp01((t - delay) / span);
}

struct CardPose {
    int x = 0;
    int y = 0;
    float alpha = 1.f;
};

// dir +1 = next (incoming from right); -1 = prev (incoming from left).
// t 0..1 wall-clock normalized.
inline CardPose outgoingPose(inkcell::Rect clip, int dir, float t) {
    // Exit starts immediately, finishes a hair early so incoming owns the end.
    float tx = smootherstep(phase(t, 0.f, 0.88f));
    float ty = smootherstep(phase(t, 0.02f, 0.75f));
    float ta = easeOutQuint(phase(t, 0.f, 0.70f));

    int travel = std::max(8, clip.w + 4);
    // Soft arc: peaks mid-flight, amplitude scales with card height
    float arc = std::sin(3.14159265f * ty);
    int arcAmp = std::max(1, std::min(4, clip.h / 5));

    CardPose p;
    p.x = clip.x - dir * static_cast<int>(std::lround(tx * travel));
    // Slight overshoot on the arc (float then settle via ty window)
    p.y = clip.y - static_cast<int>(std::lround(arc * arcAmp * (1.f - 0.15f * tx)));
    p.alpha = 1.f - ta;
    return p;
}

inline CardPose incomingPose(inkcell::Rect clip, int dir, float t) {
    // Incoming delayed — reads as follow-through, not rigid swap
    float tx = smootherstep(phase(t, 0.10f, 0.90f));
    float ty = smootherstep(phase(t, 0.14f, 0.82f));
    float ta = easeOutQuint(phase(t, 0.08f, 0.85f));

    int travel = std::max(8, clip.w + 4);
    float remain = 1.f - tx;
    // Arc settles: high on entry, damps as it lands
    float arc = std::sin(3.14159265f * (1.f - ty) * 0.92f);
    int arcAmp = std::max(1, std::min(4, clip.h / 5));

    CardPose p;
    p.x = clip.x + dir * static_cast<int>(std::lround(remain * travel));
    p.y = clip.y - static_cast<int>(std::lround(arc * arcAmp * remain));
    p.alpha = 0.25f + 0.75f * ta;
    return p;
}

inline void drawSwipedCard(
    inkcell::Surface& s, inkcell::Rect clip, CardPose pose, int cardW, int cardH, float accentHue,
    const std::function<void(inkcell::Surface&, inkcell::Rect, float)>& body) {
    cardW = std::min(cardW, clip.w);
    cardH = std::min(cardH, clip.h);
    if (cardW < 8 || cardH < 4) return;

    inkcell::Rect card{pose.x, pose.y, cardW, cardH};
    if (card.right() <= clip.x - 2 || card.x >= clip.right() + 2) return;
    if (card.bottom() <= clip.y - 2 || card.y >= clip.bottom() + 2) return;

    auto base = theme::panel_2();
    bool ghost = pose.alpha < 0.55f;
    bool soft = pose.alpha < 0.80f;

    // Shadow only when card has presence — softens the “sticker” feel
    if (pose.alpha > 0.35f) {
        auto sh = inkcell::Style::normal()
                      .with_bg(theme::color(inkcell::Color::rgb(6, 6, 8), inkcell::Color::rgb(2, 3, 6)))
                      .with_fg(theme::color(inkcell::Color::rgb(6, 6, 8), inkcell::Color::rgb(2, 3, 6)));
        int shOff = pose.alpha > 0.7f ? 1 : 0;
        for (int y = std::max(card.y + shOff, clip.y);
             y < std::min(card.bottom() + shOff, clip.bottom()); ++y) {
            int x0 = std::max(card.x + shOff, clip.x);
            int x1 = std::min(card.right() + shOff, clip.right());
            if (x1 > x0) s.fill({x0, y, x1 - x0, 1}, " ", sh);
        }
    }

    for (int y = std::max(card.y, clip.y); y < std::min(card.bottom(), clip.bottom()); ++y) {
        int x0 = std::max(card.x, clip.x);
        int x1 = std::min(card.right(), clip.right());
        if (x1 > x0) s.fill({x0, y, x1 - x0, 1}, " ", base);
    }

    inkcell::Color bd =
        accentHue > 0.5f
            ? theme::color(inkcell::Color::rgb(120, 105, 155), inkcell::Color::rgb(140, 120, 210))
            : theme::color(inkcell::Color::rgb(70, 100, 110), inkcell::Color::rgb(50, 120, 150));
    if (ghost)
        bd = theme::color(inkcell::Color::rgb(50, 50, 58), inkcell::Color::rgb(36, 46, 64));
    else if (soft)
        bd = theme::color(inkcell::Color::rgb(58, 70, 78), inkcell::Color::rgb(45, 70, 95));

    auto border = base.with_fg(bd);
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

    if (pose.alpha > 0.45f) {
        auto ac = (accentHue > 0.5f ? theme::violet_soft() : theme::cyan_soft()).with_bg(base.bg);
        if (soft) ac.dim = true;
        int y = card.y + 1;
        if (y >= clip.y && y < clip.bottom()) {
            for (int x = card.x + 2; x < card.right() - 2; ++x)
                if (x >= clip.x && x < clip.right()) s.text({x, y}, "─", ac);
        }
    }

    inkcell::Rect inner{card.x + 2, card.y + 2, std::max(1, card.w - 4), std::max(1, card.h - 4)};
    int iy0 = std::max(inner.y, clip.y);
    int iy1 = std::min(inner.bottom(), clip.bottom());
    int ix0 = std::max(inner.x, clip.x);
    int ix1 = std::min(inner.right(), clip.right());
    if (iy1 > iy0 && ix1 > ix0)
        body(s, {ix0, iy0, ix1 - ix0, iy1 - iy0}, pose.alpha);
}

// Soft list nudge — ease out then settle (no hard snap).
inline int listNudgeX(int dir, float t, int maxNudge = 4) {
    // Bell curve: rises quick, eases to 0 by end
    float e = smootherstep(clamp01(t));
    float wave = std::sin(3.14159265f * e) * (1.f - 0.35f * e);
    return dir * static_cast<int>(std::lround(wave * maxNudge));
}

}  // namespace cortex::mk3::ui::components
