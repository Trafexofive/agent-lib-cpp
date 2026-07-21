#pragma once
// Bottom-center hovering pill navigation — cortex-local frontend chrome.
// Smooth highlight via ease-out phase (dashboard.navAnimT). Idle tick drives frames.

#include <algorithm>
#include <string>
#include <vector>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::components {

struct PillItem {
    std::string key;
    std::string label;
};

inline float easeOutCubic(float t) {
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    float u = 1.f - t;
    return 1.f - u * u * u;
}

inline float pillHighlight(int index, int current, int previous, float t) {
    float e = easeOutCubic(t);
    if (index == current) return 0.30f + 0.70f * e;
    if (index == previous && t < 1.f) return 0.70f * (1.f - e);
    return 0.f;
}

inline inkcell::Style pillSurface(float hi) {
    int lift = static_cast<int>(22.f * hi);
    int g = std::min(62, 22 + lift);
    return inkcell::Style::normal().with_bg(
        theme::color(inkcell::Color::rgb(g, g, g),
                     inkcell::Color::rgb(8 + lift, 12 + lift / 2, 20 + lift)));
}

inline inkcell::Style pillFg(float hi) {
    auto st = pillSurface(hi);
    if (hi > 0.55f) {
        st.fg = theme::color(inkcell::Color::rgb(248, 248, 248),
                             inkcell::Color::rgb(101, 227, 154));
        st.bold = true;
    } else if (hi > 0.18f) {
        st.fg = theme::color(inkcell::Color::rgb(225, 225, 225),
                             inkcell::Color::rgb(200, 220, 255));
        st.bold = true;
    } else {
        st.fg = theme::color(inkcell::Color::rgb(130, 130, 130),
                             inkcell::Color::rgb(110, 122, 148));
        st.dim = true;
    }
    return st;
}

// Two-row floating dock: hairline glow + pill body. Returns total height used (2).
inline int drawPillDock(inkcell::Surface& s, int pageX, int pageW, int bottomY,
                        const std::vector<PillItem>& items, int current, int previous, float animT) {
    if (items.empty() || pageW < 24) return 0;

    std::vector<std::string> segs;
    int contentW = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        std::string seg = " " + items[i].key + " " + items[i].label + " ";
        segs.push_back(seg);
        contentW += inkcell::text::display_width(seg);
        if (i + 1 < items.size()) contentW += 1;
    }

    const int pad = 3;
    int pillW = std::min(pageW - 2, contentW + pad * 2 + 2);
    int pillX = pageX + std::max(0, (pageW - pillW) / 2);
    int pillY = bottomY;          // main pill row
    int glowY = bottomY - 1;      // floating hairline above

    // Soft floating shadow / glow bar (shorter than pill, centered, intensity follows anim)
    float peak = pillHighlight(current, current, previous, animT);
    int glowW = std::max(8, static_cast<int>(pillW * (0.35f + 0.45f * peak)));
    int glowX = pillX + (pillW - glowW) / 2;
    std::string glow;
    for (int i = 0; i < glowW; ++i) glow += "─";
    auto gst = theme::cyan();
    gst.dim = peak < 0.7f;
    gst.bold = peak >= 0.7f;
    // Track sliding glow toward active segment
    {
        // approximate active segment center
        int acc = pillX + pad + 1;
        int activeCenter = acc;
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            int sw = inkcell::text::display_width(segs[static_cast<size_t>(i)]);
            if (i == current) {
                activeCenter = acc + sw / 2;
                break;
            }
            acc += sw + 1;
        }
        // blend previous→current centers
        int prevCenter = activeCenter;
        if (previous >= 0 && previous < static_cast<int>(items.size()) && animT < 1.f) {
            acc = pillX + pad + 1;
            for (int i = 0; i < static_cast<int>(items.size()); ++i) {
                int sw = inkcell::text::display_width(segs[static_cast<size_t>(i)]);
                if (i == previous) {
                    prevCenter = acc + sw / 2;
                    break;
                }
                acc += sw + 1;
            }
            float e = easeOutCubic(animT);
            activeCenter = static_cast<int>(prevCenter + (activeCenter - prevCenter) * e);
        }
        glowX = std::max(pillX + 2, std::min(pillX + pillW - glowW - 2, activeCenter - glowW / 2));
    }
    s.text({glowX, glowY}, inkcell::text::truncate(glow, glowW), gst);

    // Pill body
    inkcell::Rect pill{pillX, pillY, pillW, 1};
    s.fill(pill, " ", pillSurface(0.12f + 0.15f * peak));
    s.text({pill.x, pill.y}, "", pillFg(0.25f));  // may fall back ugly; use ( if needed
    // Safer ASCII caps — powerline glyphs often missing
    s.text({pill.x, pill.y}, "(", pillFg(0.3f));
    s.text({pill.right() - 1, pill.y}, ")", pillFg(0.3f));

    int x = pill.x + pad;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        float hi = pillHighlight(i, current, previous, animT);
        const std::string& seg = segs[static_cast<size_t>(i)];
        int sw = inkcell::text::display_width(seg);
        if (x + sw >= pill.right() - 1) break;
        if (hi > 0.15f) s.fill({x, pill.y, sw, 1}, " ", pillSurface(hi));
        s.text({x, pill.y}, seg, pillFg(hi));
        x += sw;
        if (i + 1 < static_cast<int>(items.size()) && x < pill.right() - 1) {
            s.text({x, pill.y}, "│", pillFg(0.05f));
            ++x;
        }
    }
    return 2;
}

inline void drawStatusRail(inkcell::Surface& s, int pageX, int pageW, int y,
                           const std::string& left, const std::string& mid,
                           const std::string& right) {
    // Centered three-part meta above the dock.
    std::string L = left, M = mid, R = right;
    auto trunc = [&](std::string& v, int budget) {
        if (inkcell::text::display_width(v) > budget) v = inkcell::text::truncate(v, budget);
    };
    int third = std::max(8, pageW / 3 - 1);
    trunc(L, third);
    trunc(M, third);
    trunc(R, third);
    s.text({pageX, y}, L, theme::dim());
    int mw = inkcell::text::display_width(M);
    s.text({pageX + std::max(0, (pageW - mw) / 2), y}, M, theme::dim());
    int rw = inkcell::text::display_width(R);
    s.text({std::max(pageX, pageX + pageW - rw), y}, R, theme::dim());
}

}  // namespace cortex::mk3::ui::components
