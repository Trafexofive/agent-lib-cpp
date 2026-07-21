#pragma once
// Blit CellBuffer → inkcell::Surface as a sprite.
// alpha=0 skips; alpha<128 can dim/mix depending on mode.

#include <algorithm>

#include "inkcell/surface.hpp"
#include "src/ui/gfx/cell_buffer.hpp"

namespace cortex::mk3::ui::gfx {

enum class BlitMode {
    Opaque,       // write all alpha>0 cells as-is
    Transparent,  // skip alpha==0 only
    Underlay,     // only write where dest still looks empty-ish (space glyph) — soft bg
    MultiplyDim,  // write but force dim on low alpha
};

inline void blit(inkcell::Surface& dest, const CellBuffer& src, int dstX, int dstY,
                 BlitMode mode = BlitMode::Transparent, inkcell::Rect clip = {}) {
    if (src.empty()) return;
    if (clip.w <= 0 || clip.h <= 0) clip = dest.bounds();

    for (int y = 0; y < src.h; ++y) {
        int dy = dstY + y;
        if (dy < clip.y || dy >= clip.bottom()) continue;
        for (int x = 0; x < src.w; ++x) {
            int dx = dstX + x;
            if (dx < clip.x || dx >= clip.right()) continue;
            const CellPx& px = src.at(x, y);
            if (mode != BlitMode::Opaque && px.alpha == 0) continue;

            if (mode == BlitMode::Underlay) {
                // Don't stomp non-space content — background only
                const auto& cell = dest.cell({dx, dy});
                if (cell.glyph != " " && cell.glyph != "" && cell.glyph != "\0") {
                    // still allow very faint underlay on empty-looking cells only
                    continue;
                }
            }

            inkcell::Style st = px.style;
            if (mode == BlitMode::MultiplyDim || px.alpha < 180) {
                if (px.alpha < 90) st.dim = true;
            }
            dest.put({dx, dy}, px.glyph.empty() ? " " : px.glyph, st);
        }
    }
}

// Tile a smaller sprite across a rect (cached once, blitted many).
inline void blitTiled(inkcell::Surface& dest, const CellBuffer& tile, inkcell::Rect area,
                      BlitMode mode = BlitMode::Underlay) {
    if (tile.empty() || area.empty()) return;
    for (int y = area.y; y < area.bottom(); y += tile.h)
        for (int x = area.x; x < area.right(); x += tile.w)
            blit(dest, tile, x, y, mode, area);
}

}  // namespace cortex::mk3::ui::gfx
