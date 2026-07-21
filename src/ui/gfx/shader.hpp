#pragma once
// Cell-shader contract: pure function of (x,y,t,uniforms) → CellPx.
// No GPU. Precompute → cache → blit as sprites. That's the whole pipeline.

#include <cstdint>
#include <functional>
#include <string>

#include "src/ui/gfx/cell_buffer.hpp"

namespace cortex::mk3::ui::gfx {

struct ShaderEnv {
    int w = 0;
    int h = 0;
    float t = 0.f;          // seconds or normalized phase
    int timeBucket = 0;     // discrete cache slice
    uint32_t uniforms = 0;  // packed knobs (density, seed, mode…)
    int variant = 0;        // 0 graphite, 1 neon
};

// Shade one cell. May leave alpha=0 for transparent.
using ShadeFn = std::function<CellPx(int x, int y, const ShaderEnv& env)>;

struct Shader {
    std::string id;
    ShadeFn shade;
    // How many buckets per "cycle" for animation caching. 1 = static.
    int buckets = 1;
    // Bucket duration ms (for time → bucket).
    int bucketMs = 80;
};

inline CellBuffer bake(const Shader& shader, const ShaderEnv& env) {
    CellBuffer buf(env.w, env.h);
    if (buf.empty() || !shader.shade) return buf;
    for (int y = 0; y < env.h; ++y)
        for (int x = 0; x < env.w; ++x)
            buf.at(x, y) = shader.shade(x, y, env);
    return buf;
}

}  // namespace cortex::mk3::ui::gfx
