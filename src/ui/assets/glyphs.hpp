#pragma once
// Cortex-local UI glyphs and static copy.
// NOT part of inkcell — inkcell stays a primitive terminal kit.
// Assets/components for MK3 live under src/ui/{assets,components}/.

#include <string>

namespace cortex::mk3::ui::assets {

// ── Layout marks ──────────────────────────────────────────────────────
inline constexpr const char* ACCENT_BAR = "▌";
inline constexpr const char* RULE = "─";
inline constexpr const char* BULLET = "·";
inline constexpr const char* SELECT = "›";
inline constexpr const char* ACTIVE = "●";
inline constexpr const char* IDLE = "○";
inline constexpr const char* OK = "✓";
inline constexpr const char* ERR = "✗";
inline constexpr const char* DRILL = "↳";
inline constexpr const char* FOLDER = "▸";

// ── Manifest kind tags (fixed-width-ish for list columns) ─────────────
inline const char* kindTag(const std::string& kind) {
    if (kind == "agent") return "AGT";
    if (kind == "tool") return "TOL";
    if (kind == "feed") return "FED";
    if (kind == "relic") return "RLC";
    if (kind == "workflow") return "WFL";
    if (kind == "skill") return "SKL";
    if (kind == "harness") return "HRN";
    if (kind == "prompt") return "PMT";
    if (kind == "system") return "SYS";
    if (kind == "persona") return "PER";
    return "MAN";
}

inline const char* kindLabel(const std::string& kind) {
    if (kind == "agent") return "agent";
    if (kind == "tool") return "tool";
    if (kind == "feed") return "feed";
    if (kind == "relic") return "relic";
    if (kind == "workflow") return "workflow";
    if (kind == "skill") return "skill";
    if (kind == "harness") return "harness";
    if (kind == "prompt") return "prompt";
    return kind.c_str();
}

// ── Dashboard chrome copy ─────────────────────────────────────────────
inline constexpr const char* DASH_TITLE = "CORTEX MK3";
inline constexpr const char* DASH_SUB = "control hub";
inline constexpr const char* MANIFESTS_EMPTY =
    "No manifests under manifests/ — PROD registry is empty.";
inline constexpr const char* MANIFESTS_HINT =
    "manifests/ = PROD · config/ = DEV/MVP · Enter = detail · a = refresh";

}  // namespace cortex::mk3::ui::assets
