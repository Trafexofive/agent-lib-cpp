#pragma once
// Software fragment field → half-block cells (▀).
// This is NOT GLSL. It *is* continuous f(x,y,t) sampled into a pixel grid,
// then quantized to terminal half-blocks (2 vertical samples per cell).
// Cache by (id,w,h,bucket,variant).

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "inkcell/surface.hpp"
#include "src/ui/gfx/blit.hpp"
#include "src/ui/gfx/cache.hpp"
#include "src/ui/gfx/shader.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::gfx {

struct Rgb8 {
    uint8_t r, g, b;
};

inline Rgb8 lerpRgb(Rgb8 a, Rgb8 b, float t) {
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    auto L = [&](uint8_t x, uint8_t y) {
        return static_cast<uint8_t>(x + (y - x) * t);
    };
    return {L(a.r, b.r), L(a.g, b.g), L(a.b, b.b)};
}

// Classic plasma + dual ripple. Returns 0..1 intensity.
inline float fieldPlasmaRipple(float u, float v, float t) {
    // u,v in 0..1
    float x = u * 6.2831853f;
    float y = v * 6.2831853f;
    float p = std::sin(x * 1.1f + t * 1.3f);
    p += std::sin(y * 1.4f - t * 0.9f);
    p += std::sin((x + y) * 0.7f + t * 0.6f);
    // ripples
    float cx1 = 0.35f + 0.15f * std::sin(t * 0.7f);
    float cy1 = 0.40f + 0.12f * std::cos(t * 0.55f);
    float cx2 = 0.70f + 0.12f * std::cos(t * 0.5f);
    float cy2 = 0.60f + 0.14f * std::sin(t * 0.65f);
    float d1 = std::sqrt((u - cx1) * (u - cx1) + (v - cy1) * (v - cy1));
    float d2 = std::sqrt((u - cx2) * (u - cx2) * 0.85f + (v - cy2) * (v - cy2));
    p += std::sin(d1 * 28.f - t * 4.5f) * 0.85f;
    p += std::sin(d2 * 22.f - t * 3.2f) * 0.70f;
    // normalize rough range ~[-4,4] → [0,1]
    return 0.5f + 0.5f * std::tanh(p * 0.35f);
}

inline Rgb8 fieldColor(float intensity, int variant) {
    // graphite: deep void → steel cyan → soft violet
    // neon: black → hot cyan → green spit
    if (variant) {
        Rgb8 a{4, 8, 14};
        Rgb8 b{20, 90, 120};
        Rgb8 c{90, 220, 255};
        Rgb8 d{101, 227, 154};
        if (intensity < 0.45f) return lerpRgb(a, b, intensity / 0.45f);
        if (intensity < 0.75f) return lerpRgb(b, c, (intensity - 0.45f) / 0.30f);
        return lerpRgb(c, d, (intensity - 0.75f) / 0.25f);
    }
    Rgb8 a{12, 12, 14};
    Rgb8 b{28, 36, 42};
    Rgb8 c{70, 110, 125};
    Rgb8 d{120, 110, 150};
    if (intensity < 0.40f) return lerpRgb(a, b, intensity / 0.40f);
    if (intensity < 0.72f) return lerpRgb(b, c, (intensity - 0.40f) / 0.32f);
    return lerpRgb(c, d, (intensity - 0.72f) / 0.28f);
}

// Bake half-block framebuffer of the field.
inline Shader shaderFieldPlasma() {
    Shader s;
    s.id = "field.plasma_ripple";
    s.buckets = 32;
    s.bucketMs = 50;
    s.shade = [](int x, int y, const ShaderEnv& env) -> CellPx {
        CellPx px;
        const float t = env.timeBucket / 32.f * 6.2831853f;
        const float u = (x + 0.5f) / std::max(1, env.w);
        // two vertical samples for ▀
        const float v0 = (y * 2 + 0.5f) / std::max(1, env.h * 2);
        const float v1 = (y * 2 + 1.5f) / std::max(1, env.h * 2);
        float i0 = fieldPlasmaRipple(u, v0, t);
        float i1 = fieldPlasmaRipple(u, v1, t + 0.15f);
        Rgb8 c0 = fieldColor(i0, env.variant);
        Rgb8 c1 = fieldColor(i1, env.variant);
        px.glyph = "▀";
        px.style.fg = inkcell::Color::rgb(c0.r, c0.g, c0.b);
        px.style.bg = inkcell::Color::rgb(c1.r, c1.g, c1.b);
        px.alpha = 255;
        return px;
    };
    return s;
}

inline const CellBuffer& bakeFieldPlasma(int w, int h, int variant, float seconds) {
    static Shader sh = shaderFieldPlasma();
    ShaderEnv env;
    env.w = std::max(1, w);
    env.h = std::max(1, h);
    env.variant = variant;
    env.t = seconds;
    env.timeBucket =
        sh.bucketMs > 0 ? static_cast<int>(seconds * 1000.f / sh.bucketMs) % sh.buckets : 0;
    return globalShaderCache().getOrBake(sh, env);
}

// Blit field as full background (opaque).
inline void drawFieldBg(inkcell::Surface& s, inkcell::Rect area, int variant, float seconds) {
    if (area.w <= 0 || area.h <= 0) return;
    const auto& frame = bakeFieldPlasma(area.w, area.h, variant, seconds);
    blit(s, frame, area.x, area.y, BlitMode::Opaque, area);
}

// Open-top panel: sides + bottom only — top edge is open to the field bg.
inline void drawOpenTopPanel(inkcell::Surface& s, inkcell::Rect r, inkcell::Style fill,
                             inkcell::Style border) {
    if (r.w < 2 || r.h < 2) return;
    s.fill(r, " ", fill);
    // sides
    for (int y = r.y; y < r.bottom(); ++y) {
        s.text({r.x, y}, "│", border);
        s.text({r.right() - 1, y}, "│", border);
    }
    // bottom corners + edge
    s.text({r.x, r.bottom() - 1}, "╰", border);
    s.text({r.right() - 1, r.bottom() - 1}, "╯", border);
    for (int x = r.x + 1; x < r.right() - 1; ++x)
        s.text({x, r.bottom() - 1}, "─", border);
    // no top border — field bleeds in
}

}  // namespace cortex::mk3::ui::gfx
