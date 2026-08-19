#pragma once
// Semantic chat block classification and palette.
// Each kind has a distinct hue family: rail + header + body + wash.
// Not a gray stack with one accent.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::chat {

enum class ChatBlockKind {
    None,
    User,
    Assistant,
    Agent,
    ToolRead,
    ToolExec,
    ToolWrite,
    ToolAsk,
    ToolOther,
    ResultOk,
    ResultError,
    Error,
    Thought,
    Raw,
    Notice,
};

// Per-kind chroma. Graphite keeps readable mid tones; neon punches.
struct KindPalette {
    inkcell::Color railG, railN;       // left gutter
    inkcell::Color headG, headN;       // header fg
    inkcell::Color bodyG, bodyN;       // body fg
    inkcell::Color washG, washN;       // row bg
    inkcell::Color washHiG, washHiN;   // selected / header wash lift
};

inline KindPalette kindPalette(ChatBlockKind kind) {
    using C = inkcell::Color;
    switch (kind) {
        case ChatBlockKind::User:
            // Mint — operator voice
            return {C::rgb(72, 168, 132), C::rgb(64, 230, 170),
                    C::rgb(150, 220, 180), C::rgb(120, 255, 190),
                    C::rgb(168, 198, 178), C::rgb(150, 220, 185),
                    C::rgb(18, 32, 26), C::rgb(6, 28, 22),
                    C::rgb(28, 48, 38), C::rgb(10, 42, 34)};
        case ChatBlockKind::Assistant:
            // Ice cyan — main agent prose
            return {C::rgb(78, 168, 198), C::rgb(70, 220, 255),
                    C::rgb(150, 210, 230), C::rgb(130, 240, 255),
                    C::rgb(175, 200, 215), C::rgb(165, 220, 240),
                    C::rgb(16, 26, 36), C::rgb(5, 18, 34),
                    C::rgb(24, 40, 54), C::rgb(8, 30, 52)};
        case ChatBlockKind::Agent:
            // Violet — child / sub-agent well
            return {C::rgb(168, 120, 210), C::rgb(210, 120, 255),
                    C::rgb(200, 165, 230), C::rgb(230, 160, 255),
                    C::rgb(185, 165, 205), C::rgb(200, 170, 230),
                    C::rgb(30, 22, 40), C::rgb(28, 10, 48),
                    C::rgb(44, 32, 58), C::rgb(42, 16, 68)};
        case ChatBlockKind::ToolRead:
            // Steel blue — read/list/grep
            return {C::rgb(88, 148, 188), C::rgb(80, 190, 255),
                    C::rgb(140, 190, 220), C::rgb(120, 210, 255),
                    C::rgb(150, 175, 195), C::rgb(140, 185, 220),
                    C::rgb(16, 28, 38), C::rgb(4, 22, 40),
                    C::rgb(24, 42, 56), C::rgb(8, 34, 58)};
        case ChatBlockKind::ToolExec:
            // Amber — exec/shell/sleep
            return {C::rgb(210, 150, 70), C::rgb(255, 175, 60),
                    C::rgb(230, 185, 110), C::rgb(255, 200, 100),
                    C::rgb(195, 170, 130), C::rgb(220, 180, 110),
                    C::rgb(36, 28, 16), C::rgb(42, 24, 4),
                    C::rgb(52, 40, 22), C::rgb(58, 34, 8)};
        case ChatBlockKind::ToolWrite:
            // Coral — write/mutate
            return {C::rgb(210, 110, 100), C::rgb(255, 110, 120),
                    C::rgb(230, 150, 140), C::rgb(255, 145, 150),
                    C::rgb(200, 155, 150), C::rgb(220, 155, 160),
                    C::rgb(38, 22, 22), C::rgb(44, 10, 16),
                    C::rgb(54, 32, 32), C::rgb(62, 16, 24)};
        case ChatBlockKind::ToolAsk:
            // Magenta — ask_tool
            return {C::rgb(190, 110, 185), C::rgb(240, 120, 230),
                    C::rgb(220, 160, 210), C::rgb(250, 150, 240),
                    C::rgb(190, 160, 185), C::rgb(215, 160, 210),
                    C::rgb(34, 22, 36), C::rgb(36, 8, 42),
                    C::rgb(48, 32, 52), C::rgb(52, 14, 60)};
        case ChatBlockKind::ToolOther:
            // Gold — other tools
            return {C::rgb(190, 170, 80), C::rgb(230, 210, 70),
                    C::rgb(215, 200, 120), C::rgb(245, 230, 100),
                    C::rgb(185, 175, 130), C::rgb(210, 200, 120),
                    C::rgb(32, 30, 18), C::rgb(34, 30, 6),
                    C::rgb(46, 44, 26), C::rgb(48, 42, 10)};
        case ChatBlockKind::ResultOk:
            // Green seal
            return {C::rgb(90, 175, 120), C::rgb(80, 230, 140),
                    C::rgb(140, 205, 160), C::rgb(120, 245, 170),
                    C::rgb(155, 185, 165), C::rgb(145, 205, 170),
                    C::rgb(16, 32, 22), C::rgb(4, 32, 16),
                    C::rgb(24, 46, 32), C::rgb(8, 48, 26)};
        case ChatBlockKind::ResultError:
        case ChatBlockKind::Error:
            // Red alarm
            return {C::rgb(220, 90, 95), C::rgb(255, 90, 110),
                    C::rgb(235, 140, 145), C::rgb(255, 140, 150),
                    C::rgb(205, 155, 155), C::rgb(230, 155, 160),
                    C::rgb(40, 18, 20), C::rgb(48, 6, 14),
                    C::rgb(56, 26, 28), C::rgb(66, 10, 20)};
        case ChatBlockKind::Thought:
            // Soft slate-violet — internal, never competes with reply
            return {C::rgb(100, 105, 140), C::rgb(110, 120, 180),
                    C::rgb(145, 148, 175), C::rgb(150, 155, 200),
                    C::rgb(125, 128, 150), C::rgb(130, 135, 175),
                    C::rgb(20, 20, 28), C::rgb(10, 12, 24),
                    C::rgb(28, 28, 40), C::rgb(16, 18, 36)};
        case ChatBlockKind::Raw:
            // Dim steel
            return {C::rgb(90, 100, 115), C::rgb(80, 100, 140),
                    C::rgb(130, 140, 155), C::rgb(120, 140, 180),
                    C::rgb(115, 120, 135), C::rgb(110, 125, 160),
                    C::rgb(14, 16, 20), C::rgb(4, 8, 16),
                    C::rgb(22, 24, 30), C::rgb(8, 14, 26)};
        case ChatBlockKind::Notice:
            // Warm gray / status
            return {C::rgb(150, 140, 100), C::rgb(180, 160, 90),
                    C::rgb(185, 175, 140), C::rgb(210, 190, 120),
                    C::rgb(160, 155, 135), C::rgb(175, 165, 130),
                    C::rgb(26, 24, 20), C::rgb(18, 16, 10),
                    C::rgb(36, 34, 28), C::rgb(28, 24, 14)};
        case ChatBlockKind::None:
        default:
            return {C::rgb(70, 70, 78), C::rgb(50, 60, 80),
                    C::rgb(210, 210, 216), C::rgb(215, 222, 234),
                    C::rgb(180, 180, 188), C::rgb(185, 195, 210),
                    C::rgb(20, 20, 23), C::rgb(8, 11, 18),
                    C::rgb(28, 28, 34), C::rgb(14, 20, 32)};
    }
}

inline std::string stripSelectionMarker(const std::string& line) {
    size_t first = line.find_first_not_of(' ');
    std::string value = first == std::string::npos ? std::string() : line.substr(first);
    const std::string selected = "› ";
    if (value.rfind(selected, 0) == 0) value.erase(0, selected.size());
    return value;
}

// Classify a semantic chat block header. `agentName` is the real assistant
// display name (replaces the CORTEX sentinel) — Assistant kind matches it
// first, then falls back to "CORTEX" for standalone tests that never wire it.
inline ChatBlockKind classifyChatBlock(const std::string& header,
                                       const std::string& agentName = {}) {
    std::string value = stripSelectionMarker(header);
    if (value.rfind("YOU", 0) == 0) return ChatBlockKind::User;
    if (value.rfind("PARENT", 0) == 0) return ChatBlockKind::User;
    if (!agentName.empty() && value.rfind(agentName, 0) == 0) return ChatBlockKind::Assistant;
    if (value.rfind("CORTEX", 0) == 0) return ChatBlockKind::Assistant;
    if (value.rfind("AGENT", 0) == 0) return ChatBlockKind::Agent;
    if (value.rfind("┌ ", 0) == 0 || value.rfind("│", 0) == 0 || value.rfind("└ ", 0) == 0)
        return ChatBlockKind::Agent;
    if (value.rfind("✓ RESULT", 0) == 0) return ChatBlockKind::ResultOk;
    if (value.rfind("✗ RESULT", 0) == 0) return ChatBlockKind::ResultError;
    if (value.rfind("✗ ERROR", 0) == 0) return ChatBlockKind::Error;
    if (value.rfind("THOUGHT", 0) == 0) return ChatBlockKind::Thought;
    if (value.rfind("RAW", 0) == 0) return ChatBlockKind::Raw;
    if (value.rfind("STATUS", 0) == 0 || value.rfind("⚠ LIMIT", 0) == 0 ||
        value.rfind("▣ FINALIZE", 0) == 0 || value.rfind("NOTICE", 0) == 0)
        return ChatBlockKind::Notice;
    if (value.rfind("TOOL", 0) == 0) {
        if (value.find("  read") != std::string::npos || value.find("  grep") != std::string::npos ||
            value.find("  list") != std::string::npos || value.find("  json") != std::string::npos ||
            value.find("  web_fetch") != std::string::npos || value.find("  fs_read") != std::string::npos)
            return ChatBlockKind::ToolRead;
        if (value.find("  exec") != std::string::npos || value.find("  sleep") != std::string::npos)
            return ChatBlockKind::ToolExec;
        if (value.find("  write") != std::string::npos || value.find("  fs_write") != std::string::npos ||
            value.find("  artifact") != std::string::npos)
            return ChatBlockKind::ToolWrite;
        if (value.find("  ask") != std::string::npos) return ChatBlockKind::ToolAsk;
        return ChatBlockKind::ToolOther;
    }
    if (!value.empty() && value[0] != ' ' && value.rfind("│ ", 0) != 0)
        return ChatBlockKind::Notice;
    return ChatBlockKind::None;
}

inline inkcell::Color blockBackground(ChatBlockKind kind, bool selected = false,
                                      uint64_t nowMs = 0, bool header = false) {
    auto p = kindPalette(kind);
    auto wash = theme::color(p.washG, p.washN);
    if (header) {
        auto hi = theme::color(p.washHiG, p.washHiN);
        // Blend header wash slightly brighter than body.
        wash = inkcell::Color::rgb(
            std::min(255, (wash.r * 2 + hi.r) / 3),
            std::min(255, (wash.g * 2 + hi.g) / 3),
            std::min(255, (wash.b * 2 + hi.b) / 3));
    }
    if (selected) {
        const double phase = (nowMs % 1600) / 1600.0 * 6.283185307179586;
        const double breath = 0.5 + 0.5 * std::sin(phase);
        const int lift = 18 + static_cast<int>(12.0 * breath);
        wash = inkcell::Color::rgb(std::min(255, wash.r + lift),
                                   std::min(255, wash.g + lift + 2),
                                   std::min(255, wash.b + lift + 4));
    }
    if (kind == ChatBlockKind::None) return theme::panel_bg().bg;
    return wash;
}

// Kind-colored gutter. Headers get full rail; body gets a quieter tick.
inline inkcell::Style blockRailStyle(ChatBlockKind kind, bool header, bool selected = false,
                                     uint64_t nowMs = 0) {
    auto p = kindPalette(kind);
    auto bg = blockBackground(kind, selected, nowMs, header);
    auto st = inkcell::Style::normal().with_bg(bg);
    st.fg = theme::color(p.railG, p.railN);
    if (header || selected) st.bold = true;
    if (!header && !selected) st.dim = true;
    if (selected) {
        const double phase = (nowMs % 1600) / 1600.0 * 6.283185307179586;
        st.dim = (0.5 + 0.5 * std::sin(phase)) < 0.45;
    }
    return st;
}

inline const char* blockRailGlyph(ChatBlockKind kind, bool header, bool selected) {
    if (selected) return "▌";
    if (header) {
        switch (kind) {
            case ChatBlockKind::User: return "▌";
            case ChatBlockKind::Thought: return "┊";
            case ChatBlockKind::ResultOk: return "▌";
            case ChatBlockKind::ResultError:
            case ChatBlockKind::Error: return "▌";
            case ChatBlockKind::Agent: return "┃";
            default: return "▎";
        }
    }
    // Body continuum — soft kind tick, not empty gutter
    switch (kind) {
        case ChatBlockKind::Thought: return " ";
        case ChatBlockKind::Raw: return " ";
        case ChatBlockKind::None: return " ";
        default: return "│";
    }
}

inline inkcell::Style blockStyle(ChatBlockKind kind, bool header, bool selected = false,
                                 uint64_t nowMs = 0) {
    auto p = kindPalette(kind);
    auto style = inkcell::Style::normal().with_bg(blockBackground(kind, selected, nowMs, header));

    if (header) {
        style.fg = theme::color(p.headG, p.headN);
        style.bold = true;
        style.dim = false;
        if (kind == ChatBlockKind::Thought) style.italic = true;
    } else {
        style.fg = theme::color(p.bodyG, p.bodyN);
        style.bold = false;
        // Thought/raw stay quiet; body of tools stays readable mid-chroma
        style.dim = (kind == ChatBlockKind::Thought || kind == ChatBlockKind::Raw);
        style.italic = (kind == ChatBlockKind::Thought);
    }

    if (selected && header) {
        // Keep kind hue on selected headers — don't bleach to white
        style.fg = theme::color(
            inkcell::Color::rgb(std::min(255, p.headG.r + 30),
                                std::min(255, p.headG.g + 30),
                                std::min(255, p.headG.b + 30)),
            inkcell::Color::rgb(std::min(255, p.headN.r + 20),
                                std::min(255, p.headN.g + 20),
                                std::min(255, p.headN.b + 20)));
        style.bold = true;
        style.dim = false;
    }
    return style;
}

}  // namespace cortex::mk3::ui::chat
