#pragma once
// Filter / tag chips — cortex-local frontend widgets.

#include <algorithm>
#include <string>
#include <vector>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::components {

struct Chip {
    std::string id;
    std::string label;
    int count = -1;  // -1 = hide count
    bool active = false;
};

// Horizontal chip strip. Returns rows consumed.
inline int drawChipStrip(inkcell::Surface& s, inkcell::Rect frame, const std::vector<Chip>& chips,
                         int* outEndY = nullptr) {
    int x = frame.x;
    int y = frame.y;
    int rows = 1;
    for (const auto& c : chips) {
        std::string body = c.label;
        if (c.count >= 0) body += " " + std::to_string(c.count);
        std::string cell = c.active ? ("[" + body + "]") : (" " + body + " ");
        int w = inkcell::text::display_width(cell);
        if (x + w > frame.right() && x > frame.x) {
            // wrap
            ++y;
            ++rows;
            x = frame.x;
            if (y >= frame.bottom()) break;
        }
        if (y >= frame.bottom()) break;
        auto st = theme::muted();
        if (c.active) {
            s.fill({x, y, w, 1}, " ", theme::panel_3());
            st = theme::cyan();
            st.bold = true;
        } else {
            st.italic = true;  // idle chips whisper
        }
        s.text({x, y}, cell, st);
        x += w + 1;
    }
    if (outEndY) *outEndY = y + 1;
    return rows;
}

inline void drawTagChips(inkcell::Surface& s, int x, int y, int maxW,
                         const std::vector<std::string>& tags, size_t maxN = 8) {
    int cx = x;
    size_t n = 0;
    for (const auto& t : tags) {
        if (n >= maxN) {
            s.text({cx, y}, "…", theme::dim());
            break;
        }
        std::string cell = "#" + t;
        int w = inkcell::text::display_width(cell);
        if (cx + w > x + maxW) break;
        auto ts = theme::violet_soft();
        ts.italic = true;
        s.text({cx, y}, cell, ts);
        cx += w + 1;
        ++n;
    }
}

// Soft elevated card background for selected rows (2-line cards on wide).
inline void drawCardRow(inkcell::Surface& s, inkcell::Rect r, bool selected, bool active) {
    auto bg = selected ? theme::panel_3() : theme::panel_bg();
    s.fill(r, " ", bg);
    if (selected) {
        for (int i = 0; i < r.h; ++i)
            s.text({r.x, r.y + i}, "▌", theme::footer_accent_focus());
    } else if (active) {
        s.text({r.x, r.y}, "●", theme::green());
    }
}

}  // namespace cortex::mk3::ui::components
