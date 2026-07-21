#pragma once
// Software fragment fields → half-block cells (▀).
// NOT GLSL. Continuous f(x,y,t) → 2× vertical samples → cached blit.
// Multiple named fields; cycle like theme from Settings.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

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
        return static_cast<uint8_t>(x + static_cast<int>((y - x) * t));
    };
    return {L(a.r, b.r), L(a.g, b.g), L(a.b, b.b)};
}

inline float clampf(float v, float a, float b) {
    return v < a ? a : (v > b ? b : v);
}

inline float hash21(float x, float y) {
    float n = std::sin(x * 127.1f + y * 311.7f) * 43758.5453f;
    return n - std::floor(n);
}

// ── Field functions (0..1) ───────────────────────────────────────────
inline float fieldPlasma(float u, float v, float t) {
    float x = u * 6.2831853f, y = v * 6.2831853f;
    float p = std::sin(x * 1.1f + t * 1.3f);
    p += std::sin(y * 1.4f - t * 0.9f);
    p += std::sin((x + y) * 0.7f + t * 0.6f);
    float cx1 = 0.35f + 0.15f * std::sin(t * 0.7f);
    float cy1 = 0.40f + 0.12f * std::cos(t * 0.55f);
    float cx2 = 0.70f + 0.12f * std::cos(t * 0.5f);
    float cy2 = 0.60f + 0.14f * std::sin(t * 0.65f);
    float d1 = std::sqrt((u - cx1) * (u - cx1) + (v - cy1) * (v - cy1));
    float d2 = std::sqrt((u - cx2) * (u - cx2) * 0.85f + (v - cy2) * (v - cy2));
    p += std::sin(d1 * 28.f - t * 4.5f) * 0.85f;
    p += std::sin(d2 * 22.f - t * 3.2f) * 0.70f;
    return 0.5f + 0.5f * std::tanh(p * 0.35f);
}

inline float fieldVortex(float u, float v, float t) {
    float dx = u - 0.5f, dy = v - 0.52f;
    float r = std::sqrt(dx * dx + dy * dy) + 1e-4f;
    float ang = std::atan2(dy, dx) + t * 0.8f + 1.8f / (r + 0.15f);
    float arms = std::sin(ang * 3.f + r * 18.f - t * 2.f);
    float core = std::exp(-r * 4.5f);
    return clampf(0.35f + 0.45f * arms * (1.f - r) + core * 0.5f, 0.f, 1.f);
}

inline float fieldMatrix(float u, float v, float t) {
    float col = std::floor(u * 48.f);
    float speed = 0.35f + hash21(col, 0.f) * 0.9f;
    float head = std::fmod(t * speed + hash21(col, 1.f) * 4.f, 1.4f) - 0.2f;
    float trail = head - v;
    float on = 0.f;
    if (trail > 0.f && trail < 0.55f) {
        on = (1.f - trail / 0.55f);
        on *= on;
        // glyph-ish flicker
        on *= 0.55f + 0.45f * hash21(col, std::floor(v * 40.f + t * 8.f));
    }
    float bg = 0.08f + 0.04f * hash21(u * 10.f, v * 10.f);
    return clampf(bg + on * 0.95f, 0.f, 1.f);
}

inline float fieldAurora(float u, float v, float t) {
    float band = std::sin(u * 3.5f + t * 0.6f) * 0.12f +
                 std::sin(u * 7.f - t * 0.4f) * 0.05f;
    float y0 = 0.35f + band;
    float d = std::abs(v - y0);
    float curtain = std::exp(-d * d * 55.f);
    float shimmer = 0.5f + 0.5f * std::sin(u * 20.f - t * 3.f + v * 8.f);
    float wash = std::exp(-(v - 0.2f) * (v - 0.2f) * 6.f) * 0.25f;
    return clampf(curtain * (0.55f + 0.45f * shimmer) + wash, 0.f, 1.f);
}

inline float fieldCheckerWave(float u, float v, float t) {
    float warp = 0.08f * std::sin(v * 10.f + t) + 0.06f * std::cos(u * 9.f - t * 0.7f);
    float cx = std::floor((u + warp) * 14.f);
    float cy = std::floor((v + warp * 0.7f) * 10.f);
    float cell = std::fmod(cx + cy, 2.f);
    float pulse = 0.5f + 0.5f * std::sin(t * 2.f + cx * 0.4f - cy * 0.3f);
    return cell > 0.5f ? 0.25f + 0.35f * pulse : 0.55f + 0.35f * (1.f - pulse);
}

inline float fieldStarfield(float u, float v, float t) {
    // nebula base
    float n = hash21(std::floor(u * 30.f), std::floor(v * 18.f));
    float neb = 0.12f + 0.18f * hash21(u * 3.f + t * 0.05f, v * 3.f);
    neb += 0.1f * std::sin(u * 5.f + t * 0.2f) * std::cos(v * 4.f - t * 0.15f);
    // stars
    float sx = std::floor(u * 64.f), sy = std::floor(v * 36.f);
    float star = hash21(sx, sy);
    float tw = 0.f;
    if (star > 0.97f) {
        tw = 0.5f + 0.5f * std::sin(t * (4.f + star * 8.f) + sx);
        tw *= (star - 0.97f) / 0.03f;
    }
    return clampf(neb + tw, 0.f, 1.f);
}

inline float fieldHexPulse(float u, float v, float t) {
    // axial hex-ish distance
    float x = (u - 0.5f) * 1.15f, y = (v - 0.5f);
    float q = x * 1.1547f;  // 2/√3
    float r = y - x * 0.5f;
    float hq = std::floor(q + 0.5f);
    float hr = std::floor(r + 0.5f);
    float dq = q - hq, dr = r - hr;
    float edge = std::max(std::abs(dq), std::max(std::abs(dr), std::abs(dq + dr)));
    float ring = std::sin(edge * 22.f - t * 3.f);
    float cell = hash21(hq, hr);
    return clampf(0.3f + 0.35f * ring + 0.2f * cell * std::sin(t + hq), 0.f, 1.f);
}

using FieldFn = float (*)(float, float, float);

struct FieldShaderInfo {
    const char* id;
    const char* name;
    FieldFn fn;
};

inline const std::vector<FieldShaderInfo>& fieldShaders() {
    static const std::vector<FieldShaderInfo> k = {
        {"plasma", "plasma ripple", fieldPlasma},
        {"vortex", "vortex", fieldVortex},
        {"matrix", "matrix rain", fieldMatrix},
        {"aurora", "aurora", fieldAurora},
        {"checker", "checker wave", fieldCheckerWave},
        {"stars", "starfield", fieldStarfield},
        {"hex", "hex pulse", fieldHexPulse},
    };
    return k;
}

inline int& activeFieldIndex() {
    static int idx = 0;
    return idx;
}

// When false: no field bake — callers use solid theme base_bg.
inline bool& fieldEnabledFlag() {
    static bool on = true;
    return on;
}
inline bool fieldEnabled() { return fieldEnabledFlag(); }
inline void setFieldEnabled(bool on) { fieldEnabledFlag() = on; }
inline void toggleFieldEnabled() { fieldEnabledFlag() = !fieldEnabledFlag(); }

inline int fieldCount() { return static_cast<int>(fieldShaders().size()); }

inline const FieldShaderInfo& activeField() {
    const auto& all = fieldShaders();
    int i = activeFieldIndex();
    if (i < 0 || i >= static_cast<int>(all.size())) i = 0;
    return all[static_cast<size_t>(i)];
}

inline const char* activeFieldName() { return activeField().name; }
inline const char* activeFieldId() { return activeField().id; }

inline void setFieldIndex(int i) {
    int n = fieldCount();
    if (n <= 0) return;
    i %= n;
    if (i < 0) i += n;
    activeFieldIndex() = i;
}

inline void cycleField(int delta = 1) { setFieldIndex(activeFieldIndex() + delta); }

inline Rgb8 fieldColor(float intensity, int variant, int shaderIdx) {
    // Per-shader tint bias
    if (variant) {
        Rgb8 a{4, 8, 14};
        Rgb8 b{20, 90, 120};
        Rgb8 c{90, 220, 255};
        Rgb8 d{101, 227, 154};
        if (shaderIdx == 2) {  // matrix → green
            b = {10, 60, 30};
            c = {40, 200, 90};
            d = {180, 255, 160};
        } else if (shaderIdx == 3) {  // aurora
            b = {30, 40, 90};
            c = {80, 220, 180};
            d = {200, 120, 255};
        } else if (shaderIdx == 5) {  // stars
            b = {20, 24, 50};
            c = {120, 140, 220};
            d = {255, 240, 200};
        }
        if (intensity < 0.45f) return lerpRgb(a, b, intensity / 0.45f);
        if (intensity < 0.75f) return lerpRgb(b, c, (intensity - 0.45f) / 0.30f);
        return lerpRgb(c, d, (intensity - 0.75f) / 0.25f);
    }
    Rgb8 a{12, 12, 14};
    Rgb8 b{28, 36, 42};
    Rgb8 c{70, 110, 125};
    Rgb8 d{120, 110, 150};
    if (shaderIdx == 2) {
        b = {24, 40, 28};
        c = {60, 120, 80};
        d = {140, 180, 140};
    } else if (shaderIdx == 3) {
        b = {30, 34, 48};
        c = {80, 130, 120};
        d = {140, 120, 160};
    } else if (shaderIdx == 5) {
        b = {22, 24, 36};
        c = {90, 100, 140};
        d = {180, 175, 160};
    }
    if (intensity < 0.40f) return lerpRgb(a, b, intensity / 0.40f);
    if (intensity < 0.72f) return lerpRgb(b, c, (intensity - 0.40f) / 0.32f);
    return lerpRgb(c, d, (intensity - 0.72f) / 0.28f);
}

inline Shader makeActiveFieldShader() {
    const int idx = activeFieldIndex();
    const auto& info = activeField();
    Shader s;
    s.id = std::string("field.") + info.id;
    s.buckets = 32;
    s.bucketMs = 50;
    FieldFn fn = info.fn;
    s.shade = [fn, idx](int x, int y, const ShaderEnv& env) -> CellPx {
        CellPx px;
        const float t = env.timeBucket / 32.f * 6.2831853f;
        const float u = (x + 0.5f) / std::max(1, env.w);
        const float v0 = (y * 2 + 0.5f) / std::max(1, env.h * 2);
        const float v1 = (y * 2 + 1.5f) / std::max(1, env.h * 2);
        float i0 = fn(u, v0, t);
        float i1 = fn(u, v1, t + 0.12f);
        Rgb8 c0 = fieldColor(i0, env.variant, idx);
        Rgb8 c1 = fieldColor(i1, env.variant, idx);
        px.glyph = "▀";
        px.style.fg = inkcell::Color::rgb(c0.r, c0.g, c0.b);
        px.style.bg = inkcell::Color::rgb(c1.r, c1.g, c1.b);
        px.alpha = 255;
        return px;
    };
    return s;
}

inline const CellBuffer& bakeActiveField(int w, int h, int variant, float seconds) {
    // Rebuild shader descriptor when index changes (id changes → cache miss OK)
    Shader sh = makeActiveFieldShader();
    ShaderEnv env;
    env.w = std::max(1, w);
    env.h = std::max(1, h);
    env.variant = variant;
    env.t = seconds;
    env.timeBucket =
        sh.bucketMs > 0 ? static_cast<int>(seconds * 1000.f / sh.bucketMs) % sh.buckets : 0;
    env.uniforms = static_cast<uint32_t>(activeFieldIndex());
    return globalShaderCache().getOrBake(sh, env);
}

inline void drawFieldBg(inkcell::Surface& s, inkcell::Rect area, int variant, float seconds) {
    if (area.w <= 0 || area.h <= 0) return;
    if (!fieldEnabled()) {
        s.fill(area, " ", theme::base_bg());
        return;
    }
    const auto& frame = bakeActiveField(area.w, area.h, variant, seconds);
    blit(s, frame, area.x, area.y, BlitMode::Opaque, area);
}

// Borderless panel — fill only, no ─/│ chrome (field shows at edges).
inline void drawBorderlessPanel(inkcell::Surface& s, inkcell::Rect r, inkcell::Style fill) {
    if (r.w < 1 || r.h < 1) return;
    s.fill(r, " ", fill);
}

}  // namespace cortex::mk3::ui::gfx
