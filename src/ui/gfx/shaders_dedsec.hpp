#pragma once
// DedSec-flavored cell shaders — circuit grid, scan rain, node pulse.
// Designed to bake once per (size, bucket, theme) and blit cheap.

#include <chrono>
#include <cmath>
#include <cstdint>

#include "src/ui/gfx/cache.hpp"
#include "src/ui/gfx/shader.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::gfx {

inline uint32_t hash2(int x, int y, uint32_t seed = 0) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u +
                 seed * 362437u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

inline float hash01(int x, int y, uint32_t seed = 0) {
    return (hash2(x, y, seed) & 0xFFFF) / 65535.f;
}

// Palette for graphite / neon
inline inkcell::Color dedBg(int variant) {
    return variant ? inkcell::Color::rgb(4, 6, 10) : inkcell::Color::rgb(10, 10, 12);
}
inline inkcell::Color dedGrid(int variant) {
    return variant ? inkcell::Color::rgb(20, 48, 62) : inkcell::Color::rgb(32, 36, 42);
}
inline inkcell::Color dedAccent(int variant) {
    return variant ? inkcell::Color::rgb(90, 220, 255) : inkcell::Color::rgb(90, 140, 155);
}
inline inkcell::Color dedAccent2(int variant) {
    return variant ? inkcell::Color::rgb(101, 227, 154) : inkcell::Color::rgb(120, 160, 130);
}
inline inkcell::Color dedWarn(int variant) {
    return variant ? inkcell::Color::rgb(255, 107, 122) : inkcell::Color::rgb(180, 100, 100);
}
inline inkcell::Color dedViolet(int variant) {
    return variant ? inkcell::Color::rgb(160, 130, 255) : inkcell::Color::rgb(120, 110, 150);
}

// Full-screen DedSec scrim: dark base + circuit + scan + sparse glyphs.
inline Shader shaderDedSecScrim() {
    Shader s;
    s.id = "dedsec.scrim";
    s.buckets = 12;  // slow animation cycle
    s.bucketMs = 90;
    s.shade = [](int x, int y, const ShaderEnv& env) -> CellPx {
        CellPx px;
        const int v = env.variant;
        const float phase = env.timeBucket / static_cast<float>(std::max(1, 12));
        const uint32_t seed = env.uniforms ? env.uniforms : 0xDED5ECu;  // ok hex

        // Base void
        px.style.bg = dedBg(v);
        px.style.fg = dedGrid(v);
        px.glyph = " ";
        px.alpha = 220;

        // Circuit grid every 4×2
        bool gx = (x % 4) == 0;
        bool gy = (y % 2) == 0;
        if (gx && gy) {
            px.glyph = "·";
            px.style.fg = dedGrid(v);
            px.alpha = 200;
        } else if (gx) {
            px.glyph = "│";
            px.style.fg = dedGrid(v);
            px.style.dim = true;
            px.alpha = 160;
        } else if (gy && (x % 8) == 2) {
            px.glyph = "─";
            px.style.fg = dedGrid(v);
            px.style.dim = true;
            px.alpha = 140;
        }

        // Scan band + soft radial ripple from drifting center
        int scanY = static_cast<int>((phase * (env.h + 6))) % (env.h + 6) - 2;
        int dist = std::abs(y - scanY);
        if (dist <= 1) {
            px.glyph = dist == 0 ? "═" : "─";
            px.style.fg = dedAccent(v);
            px.style.bold = dist == 0;
            px.alpha = dist == 0 ? 255 : 180;
        }
        {
            float cx = env.w * (0.5f + 0.18f * std::sin(phase * 6.2831853f));
            float cy = env.h * (0.45f + 0.15f * std::cos(phase * 5.1f));
            float rr = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy) * 2.2f);
            float wave = std::sin(rr * 0.65f - phase * 12.f);
            if (wave > 0.72f && px.alpha < 230) {
                px.glyph = wave > 0.9f ? "∙" : "·";
                px.style.fg = dedAccent2(v);
                px.alpha = static_cast<uint8_t>(160 + wave * 70.f);
            }
        }

        // Sparse hex / glitch glyphs
        float n = hash01(x, y, seed + env.timeBucket);
        if (n > 0.992f) {
            static const char* glyphs[] = {"⌬", "⟩", "⟨", "∙", "⌖", "¤", "//", "0", "1", "▒"};
            px.glyph = glyphs[hash2(x, y, seed) % 10];
            px.style.fg = (n > 0.997f) ? dedAccent2(v) : dedAccent(v);
            px.style.bold = n > 0.998f;
            px.alpha = 230;
        } else if (n > 0.985f && (x + y + env.timeBucket) % 5 == 0) {
            px.glyph = "░";
            px.style.fg = dedViolet(v);
            px.style.dim = true;
            px.alpha = 150;
        }

        // Corner vignette — darker edges, keep center for modal readability later
        float cx = (x + 0.5f) / std::max(1, env.w) - 0.5f;
        float cy = (y + 0.5f) / std::max(1, env.h) - 0.5f;
        float r = std::sqrt(cx * cx + cy * cy);
        if (r > 0.55f) {
            px.alpha = static_cast<uint8_t>(std::min(255.f, px.alpha + (r - 0.55f) * 120.f));
            px.style.dim = true;
        }

        return px;
    };
    return s;
}

// Hub wallpaper — drifting ripple rings + sparse nodes (underlay-safe).
inline Shader shaderHubWallpaper() {
    Shader s;
    s.id = "hub.wallpaper.ripple";
    s.buckets = 24;  // smoother loop
    s.bucketMs = 70;
    s.shade = [](int x, int y, const ShaderEnv& env) -> CellPx {
        CellPx px;
        const int v = env.variant;
        px.alpha = 0;
        px.style.bg = theme::base_bg().bg;
        px.style.fg = dedGrid(v);

        const float nx = (x + 0.5f) / std::max(1, env.w);
        const float ny = (y + 0.5f) / std::max(1, env.h);
        // Dual foci drift on slow Lissajous paths — loopy, not a single bullseye
        const float tau = env.timeBucket / 24.f * 6.2831853f;
        const float cx1 = 0.35f + 0.20f * std::sin(tau * 0.7f);
        const float cy1 = 0.45f + 0.18f * std::cos(tau * 0.55f);
        const float cx2 = 0.68f + 0.16f * std::cos(tau * 0.45f);
        const float cy2 = 0.55f + 0.20f * std::sin(tau * 0.65f);

        auto ring = [&](float cx, float cy, float speed, float tight) -> float {
            float dx = (nx - cx) * env.w * 0.55f;  // aspect-ish
            float dy = (ny - cy) * env.h;
            float r = std::sqrt(dx * dx + dy * dy);
            // traveling wave: crest every ~5 cells
            float wave = std::sin(r * tight - tau * speed);
            // soft envelope so only crest lights up
            float crest = std::max(0.f, wave);
            crest = crest * crest;
            // fade with distance
            float fall = 1.f / (1.f + r * 0.08f);
            return crest * fall;
        };

        float e1 = ring(cx1, cy1, 1.6f, 0.55f);
        float e2 = ring(cx2, cy2, 1.25f, 0.48f);
        float e = std::max(e1, e2 * 0.85f);

        if (e > 0.35f) {
            // crest glyphs by intensity
            if (e > 0.78f) {
                px.glyph = (static_cast<int>(e * 20) & 1) ? "∙" : "○";
                px.style.fg = dedAccent(v);
                px.style.bold = e > 0.9f;
                px.alpha = static_cast<uint8_t>(90 + e * 100.f);
            } else if (e > 0.55f) {
                px.glyph = "·";
                px.style.fg = dedAccent(v);
                px.style.dim = true;
                px.alpha = static_cast<uint8_t>(60 + e * 80.f);
            } else {
                px.glyph = "·";
                px.style.fg = dedGrid(v);
                px.style.dim = true;
                px.alpha = static_cast<uint8_t>(40 + e * 50.f);
            }
        }

        // Sparse static grit so empty regions aren't dead
        float n = hash01(x, y, 0xA11B0u);
        if (px.alpha == 0 && (x % 18) == 0 && (y % 7) == 0) {
            px.glyph = "·";
            px.style.fg = dedGrid(v);
            px.style.dim = true;
            px.alpha = 70;
        } else if (px.alpha == 0 && n > 0.996f) {
            px.glyph = "┊";
            px.style.fg = dedViolet(v);
            px.style.dim = true;
            px.alpha = 55;
        }
        return px;
    };
    return s;
}

// Small sprite stamp — e.g. 12×5 badge / glitch tag.
inline Shader shaderGlitchBadge() {
    Shader s;
    s.id = "sprite.glitch_badge";
    s.buckets = 6;
    s.bucketMs = 70;
    s.shade = [](int x, int y, const ShaderEnv& env) -> CellPx {
        CellPx px;
        const int v = env.variant;
        px.style.bg = dedBg(v);
        if (x == 0 || y == 0 || x == env.w - 1 || y == env.h - 1) {
            px.glyph = (x == 0 && y == 0)                           ? "╭"
                       : (x == env.w - 1 && y == 0)                 ? "╮"
                       : (x == 0 && y == env.h - 1)                 ? "╰"
                       : (x == env.w - 1 && y == env.h - 1)         ? "╯"
                       : (y == 0 || y == env.h - 1)                 ? "─"
                                                                    : "│";
            px.style.fg = dedAccent(v);
            px.alpha = 255;
            return px;
        }
        float n = hash01(x + env.timeBucket, y, 0xBAD6Eu);
        if (n > 0.7f) {
            px.glyph = (n > 0.9f) ? "▓" : "░";
            px.style.fg = n > 0.85f ? dedWarn(v) : dedAccent(v);
            px.alpha = 220;
        } else {
            px.glyph = " ";
            px.alpha = 180;
            px.style.bg = theme::color(inkcell::Color::rgb(18, 18, 22), inkcell::Color::rgb(8, 12, 20));
        }
        return px;
    };
    return s;
}

// Helpers to pull a cached frame quickly.
inline const CellBuffer& bakeDedSecScrim(int w, int h, int variant, float seconds) {
    static Shader sh = shaderDedSecScrim();
    ShaderEnv env;
    env.w = w;
    env.h = h;
    env.variant = variant;
    env.t = seconds;
    env.timeBucket = sh.bucketMs > 0
                         ? static_cast<int>(seconds * 1000.f / sh.bucketMs) % std::max(1, sh.buckets)
                         : 0;
    env.uniforms = 0xDED5ECu;
    return globalShaderCache().getOrBake(sh, env);
}

inline const CellBuffer& bakeHubWallpaper(int w, int h, int variant, float seconds) {
    static Shader sh = shaderHubWallpaper();
    ShaderEnv env;
    env.w = w;
    env.h = h;
    env.variant = variant;
    env.t = seconds;
    env.timeBucket = sh.bucketMs > 0
                         ? static_cast<int>(seconds * 1000.f / sh.bucketMs) % std::max(1, sh.buckets)
                         : 0;
    return globalShaderCache().getOrBake(sh, env);
}

inline const CellBuffer& bakeGlitchBadge(int variant, float seconds) {
    static Shader sh = shaderGlitchBadge();
    ShaderEnv env;
    env.w = 14;
    env.h = 3;
    env.variant = variant;
    env.t = seconds;
    env.timeBucket = sh.bucketMs > 0
                         ? static_cast<int>(seconds * 1000.f / sh.bucketMs) % std::max(1, sh.buckets)
                         : 0;
    return globalShaderCache().getOrBake(sh, env);
}

inline float nowSeconds() {
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    return std::chrono::duration<float>(clock::now() - t0).count();
}

inline int themeVariantIndex() {
    return theme::activeVariant == theme::Variant::Neon ? 1 : 0;
}

}  // namespace cortex::mk3::ui::gfx
