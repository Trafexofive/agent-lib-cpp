#pragma once
// Terminal sprite substrate — grid of glyph+style cells, independent of Surface.
// Shaders bake here; blit composites onto inkcell::Surface.

#include <cstdint>
#include <string>
#include <vector>

#include "inkcell/style.hpp"

namespace cortex::mk3::ui::gfx {

struct CellPx {
    std::string glyph = " ";  // UTF-8, usually 1 display column
    inkcell::Style style;
    uint8_t alpha = 255;  // 0 = transparent (skip blit), 255 = opaque
};

struct CellBuffer {
    int w = 0;
    int h = 0;
    std::vector<CellPx> cells;

    CellBuffer() = default;
    CellBuffer(int width, int height) { resize(width, height); }

    void resize(int width, int height) {
        w = width < 0 ? 0 : width;
        h = height < 0 ? 0 : height;
        cells.assign(static_cast<size_t>(w * h), CellPx{});
    }

    bool empty() const { return w <= 0 || h <= 0 || cells.empty(); }

    CellPx& at(int x, int y) { return cells[static_cast<size_t>(y * w + x)]; }
    const CellPx& at(int x, int y) const { return cells[static_cast<size_t>(y * w + x)]; }

    bool inBounds(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
};

// Cache key for a baked frame.
struct BakeKey {
    std::string shaderId;
    int w = 0;
    int h = 0;
    int timeBucket = 0;   // quantized animation phase
    int variant = 0;      // theme / seed / quality
    uint32_t uniforms = 0;

    bool operator==(const BakeKey& o) const {
        return shaderId == o.shaderId && w == o.w && h == o.h && timeBucket == o.timeBucket &&
               variant == o.variant && uniforms == o.uniforms;
    }
};

struct BakeKeyHash {
    size_t operator()(const BakeKey& k) const {
        size_t h = std::hash<std::string>{}(k.shaderId);
        h ^= (static_cast<size_t>(k.w) * 73856093u);
        h ^= (static_cast<size_t>(k.h) * 19349663u);
        h ^= (static_cast<size_t>(k.timeBucket) * 83492791u);
        h ^= (static_cast<size_t>(k.variant) * 2654435761u);
        h ^= (static_cast<size_t>(k.uniforms) * 2246822519u);
        return h;
    }
};

}  // namespace cortex::mk3::ui::gfx
