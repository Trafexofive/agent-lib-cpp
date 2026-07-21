#pragma once
// Floating bottom pill — one content row, rounded ends, sliding active thumb.
// Cortex-local. Details matter: even gaps, no status-line cosplay, dock focus ring.

#include <algorithm>
#include <cmath>
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

inline float easeInOut(float t) {
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    return t < 0.5f ? 4.f * t * t * t : 1.f - std::pow(-2.f * t + 2.f, 3.f) / 2.f;
}

// Returns rows consumed (2: hairline gap row optional + pill row). We use 2:
//   y-1: thin float shadow
//   y  : pill body
inline int drawPillDock(inkcell::Surface& s, int pageX, int pageW, int bottomY,
                        const std::vector<PillItem>& items, int current, int previous, float animT,
                        bool dockFocused) {
    if (items.empty() || pageW < 24) return 0;

    // Segment strings — tight, consistent padding
    std::vector<std::string> labels;
    std::vector<int> widths;
    int inner = 0;
    for (const auto& it : items) {
        // " Home " — key shown only when dock focused (discoverability on demand)
        std::string lab = dockFocused ? (" " + it.key + " " + it.label + " ")
                                      : ("  " + it.label + "  ");
        labels.push_back(lab);
        int w = inkcell::text::display_width(lab);
        widths.push_back(w);
        inner += w;
    }

    const int side = 2;  // endcap padding inside
    int boxW = std::min(pageW - 4, inner + side * 2);
    int boxX = pageX + std::max(0, (pageW - boxW) / 2);
    int y = bottomY;

    // Colors
    const bool neon = theme::activeVariant == theme::Variant::Neon;
    inkcell::Color shellBg =
        neon ? inkcell::Color::rgb(14, 18, 28) : inkcell::Color::rgb(30, 30, 30);
    inkcell::Color shellBd =
        dockFocused
            ? (neon ? inkcell::Color::rgb(90, 220, 255) : inkcell::Color::rgb(160, 170, 175))
            : (neon ? inkcell::Color::rgb(40, 52, 72) : inkcell::Color::rgb(58, 58, 58));
    inkcell::Color idleFg =
        neon ? inkcell::Color::rgb(120, 132, 156) : inkcell::Color::rgb(130, 130, 130);
    inkcell::Color thumbBg =
        neon ? inkcell::Color::rgb(90, 220, 255) : inkcell::Color::rgb(220, 220, 220);
    inkcell::Color thumbFg =
        neon ? inkcell::Color::rgb(8, 14, 20) : inkcell::Color::rgb(20, 20, 20);

    auto shell = inkcell::Style::normal().with_bg(shellBg).with_fg(shellBd);
    auto idle = inkcell::Style::normal().with_bg(shellBg).with_fg(idleFg);
    auto thumb = inkcell::Style::normal().with_bg(thumbBg).with_fg(thumbFg);
    thumb.bold = true;

    // Soft shadow row above (depth without a second chrome line of text)
    if (y - 1 >= 0) {
        auto sh = inkcell::Style::normal()
                      .with_bg(theme::color(inkcell::Color::rgb(12, 12, 12),
                                            inkcell::Color::rgb(4, 6, 10)))
                      .with_fg(theme::color(inkcell::Color::rgb(12, 12, 12),
                                            inkcell::Color::rgb(4, 6, 10)));
        // shorter than pill
        int sw = std::max(8, boxW - 4);
        int sx = boxX + (boxW - sw) / 2;
        s.fill({sx, y - 1, sw, 1}, " ", sh);
    }

    // Pill body
    s.fill({boxX, y, boxW, 1}, " ", shell);
    // Caps — rounded feel
    s.text({boxX, y}, "", shell);  // may missing; overwrite with (
    s.text({boxX, y}, "(", shell.with_fg(shellBd));
    s.text({boxX + boxW - 1, y}, ")", shell.with_fg(shellBd));

    // Geometry for segments (centered)
    struct G {
        int x0, w;
    };
    std::vector<G> geom;
    int x = boxX + side;
    if (inner < boxW - side * 2) x = boxX + (boxW - inner) / 2;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        geom.push_back({x, widths[static_cast<size_t>(i)]});
        x += widths[static_cast<size_t>(i)];
    }

    // Sliding thumb
    float e = easeInOut(animT);
    int ti = std::max(0, std::min(current, static_cast<int>(geom.size()) - 1));
    int thumbX = geom[static_cast<size_t>(ti)].x0;
    int thumbW = geom[static_cast<size_t>(ti)].w;
    if (previous >= 0 && previous < static_cast<int>(geom.size()) && animT < 1.f) {
        int px = geom[static_cast<size_t>(previous)].x0;
        int pw = geom[static_cast<size_t>(previous)].w;
        thumbX = static_cast<int>(std::lround(px + (thumbX - px) * e));
        thumbW = static_cast<int>(std::lround(pw + (thumbW - pw) * e));
    }
    // Clamp
    if (thumbX < boxX + 1) thumbX = boxX + 1;
    if (thumbX + thumbW > boxX + boxW - 1) thumbW = std::max(1, boxX + boxW - 1 - thumbX);

    s.fill({thumbX, y, thumbW, 1}, " ", thumb);

    // Labels
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const auto& g = geom[static_cast<size_t>(i)];
        bool on = (g.x0 + g.w / 2) >= thumbX && (g.x0 + g.w / 2) < thumbX + thumbW;
        auto st = on ? thumb : idle;
        if (!on && i == current) {
            st.bold = true;
            st.fg = neon ? inkcell::Color::rgb(200, 220, 255) : inkcell::Color::rgb(210, 210, 210);
        }
        s.text({g.x0, y}, inkcell::text::truncate(labels[static_cast<size_t>(i)], g.w), st);
    }

    // Restore caps over label bleed
    s.text({boxX, y}, "(", shell.with_fg(shellBd));
    s.text({boxX + boxW - 1, y}, ")", shell.with_fg(shellBd));

    // Focus tick under pill when dock focused (one cell, center of thumb)
    if (dockFocused && y + 1 < s.bounds().bottom()) {
        // can't draw below bottomY usually — skip if no room
    }

    return 2;
}

}  // namespace cortex::mk3::ui::components
