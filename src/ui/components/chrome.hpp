#pragma once
// Cortex-local chrome primitives built on inkcell Surface only.
// Keep reusable widgets here — never upstream into inkcell.

#include <algorithm>
#include <string>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/assets/glyphs.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::components {

inline void fillRect(inkcell::Surface& s, inkcell::Rect r, inkcell::Style st) {
    if (!r.empty()) s.fill(r, " ", st);
}

// Elevated panel surface (footer-like lift without a full box border).
inline void elevatedFill(inkcell::Surface& s, inkcell::Rect r, bool focus = false) {
    fillRect(s, r, focus ? theme::footer_bg_focus() : theme::footer_bg());
}

inline void accentBar(inkcell::Surface& s, int x, int y, int h, inkcell::Style st) {
    for (int i = 0; i < h; ++i)
        s.text({x, y + i}, assets::ACCENT_BAR, st);
}

inline void hairline(inkcell::Surface& s, int x, int y, int w, inkcell::Style st = theme::dim()) {
    if (w <= 0) return;
    std::string rule;
    rule.reserve(static_cast<size_t>(w) * 3);
    for (int i = 0; i < w; ++i) rule += "─";
    s.text({x, y}, inkcell::text::truncate(rule, w), st);
}

// Two-tone header strip.
inline void headerStrip(inkcell::Surface& s, inkcell::Rect r, const std::string& left,
                        const std::string& right) {
    fillRect(s, r, theme::panel_2());
    accentBar(s, r.x, r.y, r.h, theme::footer_accent_focus());
    s.text({r.x + 2, r.y}, inkcell::text::truncate(left, std::max(1, r.w - 4)), theme::bright());
    if (!right.empty()) {
        if (r.h > 1)
            s.text({r.x + 2, r.y + 1}, inkcell::text::truncate(right, r.w - 3), theme::dim());
        else {
            int rw = inkcell::text::display_width(right);
            s.text({std::max(r.x + 2, r.right() - rw - 1), r.y},
                   inkcell::text::truncate(right, r.w - 3), theme::dim());
        }
    }
}

inline void sectionHead(inkcell::Surface& s, inkcell::Rect r, const std::string& title,
                        const std::string& subtitle = {}) {
    s.text({r.x, r.y}, inkcell::text::truncate(title, r.w), theme::bright());
    if (!subtitle.empty() && r.h > 1)
        s.text({r.x, r.y + 1}, inkcell::text::truncate(subtitle, r.w), theme::italic_dim());
}

// Focusable list row with kind chip.
inline void listRow(inkcell::Surface& s, inkcell::Rect r, const std::string& text, bool selected,
                    bool active = false) {
    auto bg = selected ? theme::panel_3() : theme::panel_bg();
    fillRect(s, r, bg);
    if (selected) accentBar(s, r.x, r.y, 1, theme::footer_accent_focus());
    std::string mark = active ? std::string(assets::ACTIVE) + " " : std::string("  ");
    auto st = selected ? theme::selected_style() : theme::text();
    s.text({r.x + (selected ? 2 : 1), r.y},
           inkcell::text::truncate(mark + text, std::max(1, r.w - 3)), st);
}

inline void kindChip(inkcell::Surface& s, int x, int y, const std::string& kind, bool selected) {
    std::string tag = std::string("[") + assets::kindTag(kind) + "]";
    auto st = theme::kindAccent(kind, selected);
    if (!selected) st.dim = false;  // keep kind hue even when idle
    if (selected) st.bold = true;
    s.text({x, y}, tag, st);
}

inline void footerBar(inkcell::Surface& s, inkcell::Rect r, const std::string& left,
                      const std::string& right) {
    elevatedFill(s, r, false);
    accentBar(s, r.x, r.y, r.h, theme::footer_accent_idle());
    s.text({r.x + 2, r.y}, inkcell::text::truncate(left, std::max(1, r.w / 2)), theme::footer_dim());
    if (!right.empty()) {
        int rw = inkcell::text::display_width(right);
        s.text({std::max(r.x + 2, r.right() - rw - 1), r.y},
               inkcell::text::truncate(right, r.w - 3), theme::footer_dim());
    }
}

inline void fieldLine(inkcell::Surface& s, int x, int y, int w, const std::string& key,
                      const std::string& value) {
    std::string k = key;
    while (inkcell::text::display_width(k) < 12) k.push_back(' ');
    // Keys italic+dim, values plain text — hierarchy without shouting
    s.text({x, y}, inkcell::text::truncate(k, 12), theme::italic_dim());
    s.text({x + 13, y}, inkcell::text::truncate(value, std::max(1, w - 13)), theme::text());
}

}  // namespace cortex::mk3::ui::components
