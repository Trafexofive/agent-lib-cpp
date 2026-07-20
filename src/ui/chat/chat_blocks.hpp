#pragma once
// Semantic chat block classification and palette primitives.

#include <algorithm>
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
    if (value.rfind("PARENT", 0) == 0) return ChatBlockKind::User;  // parent-agent mission in child chat
    if (!agentName.empty() && value.rfind(agentName, 0) == 0) return ChatBlockKind::Assistant;
    if (value.rfind("CORTEX", 0) == 0) return ChatBlockKind::Assistant;
    if (value.rfind("AGENT", 0) == 0) return ChatBlockKind::Agent;
    if (value.rfind("✓ RESULT", 0) == 0) return ChatBlockKind::ResultOk;
    if (value.rfind("✗ RESULT", 0) == 0) return ChatBlockKind::ResultError;
    if (value.rfind("✗ ERROR", 0) == 0) return ChatBlockKind::Error;
    if (value.rfind("THOUGHT", 0) == 0) return ChatBlockKind::Thought;
    if (value.rfind("RAW", 0) == 0) return ChatBlockKind::Raw;
    if (value.rfind("TOOL", 0) == 0) {
        if (value.find("  read") != std::string::npos || value.find("  grep") != std::string::npos ||
            value.find("  list") != std::string::npos || value.find("  json") != std::string::npos ||
            value.find("  web_fetch") != std::string::npos)
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

inline inkcell::Color blockBackground(ChatBlockKind kind, bool selected = false) {
    using inkcell::Color;
    Color graphite;
    Color neon;
    switch (kind) {
        case ChatBlockKind::User: graphite = Color::rgb(25, 32, 28); neon = Color::rgb(8, 35, 31); break;
        case ChatBlockKind::Assistant: graphite = Color::rgb(23, 28, 35); neon = Color::rgb(7, 25, 42); break;
        case ChatBlockKind::Agent: graphite = Color::rgb(31, 26, 38); neon = Color::rgb(30, 13, 48); break;
        case ChatBlockKind::ToolRead: graphite = Color::rgb(22, 30, 35); neon = Color::rgb(5, 29, 43); break;
        case ChatBlockKind::ToolExec: graphite = Color::rgb(35, 29, 21); neon = Color::rgb(48, 27, 4); break;
        case ChatBlockKind::ToolWrite: graphite = Color::rgb(37, 25, 23); neon = Color::rgb(48, 13, 15); break;
        case ChatBlockKind::ToolAsk: graphite = Color::rgb(31, 26, 38); neon = Color::rgb(35, 10, 45); break;
        case ChatBlockKind::ToolOther: graphite = Color::rgb(31, 30, 25); neon = Color::rgb(37, 31, 5); break;
        case ChatBlockKind::ResultOk: graphite = Color::rgb(23, 32, 25); neon = Color::rgb(7, 36, 17); break;
        case ChatBlockKind::ResultError:
        case ChatBlockKind::Error: graphite = Color::rgb(38, 23, 24); neon = Color::rgb(48, 8, 14); break;
        case ChatBlockKind::Thought: graphite = Color::rgb(25, 25, 25); neon = Color::rgb(14, 18, 27); break;
        case ChatBlockKind::Raw: graphite = Color::rgb(19, 19, 19); neon = Color::rgb(4, 8, 14); break;
        case ChatBlockKind::Notice: graphite = Color::rgb(28, 28, 28); neon = Color::rgb(12, 18, 28); break;
        case ChatBlockKind::None: return theme::panel_bg().bg;
    }
    if (selected) {
        graphite = Color::rgb(std::min(255, graphite.r + 10), std::min(255, graphite.g + 10), std::min(255, graphite.b + 10));
        neon = Color::rgb(std::min(255, neon.r + 12), std::min(255, neon.g + 12), std::min(255, neon.b + 12));
    }
    return theme::color(graphite, neon);
}

inline inkcell::Style blockStyle(ChatBlockKind kind, bool header, bool selected = false) {
    auto style = inkcell::Style::normal().with_bg(blockBackground(kind, selected));
    switch (kind) {
        case ChatBlockKind::User:
        case ChatBlockKind::ResultOk: style.fg = theme::green().fg; break;
        case ChatBlockKind::Assistant:
        case ChatBlockKind::ToolRead: style.fg = theme::cyan().fg; break;
        case ChatBlockKind::Agent:
        case ChatBlockKind::ToolAsk: style.fg = theme::color(inkcell::Color::rgb(180, 155, 203), inkcell::Color::rgb(219, 130, 255)); break;
        case ChatBlockKind::ToolExec:
        case ChatBlockKind::ToolOther: style.fg = theme::amber().fg; break;
        case ChatBlockKind::ToolWrite:
        case ChatBlockKind::ResultError:
        case ChatBlockKind::Error: style.fg = theme::red().fg; break;
        case ChatBlockKind::Thought:
        case ChatBlockKind::Raw:
        case ChatBlockKind::Notice: style.fg = theme::dim().fg; break;
        case ChatBlockKind::None: style.fg = theme::text().fg; break;
    }
    if (!header && kind != ChatBlockKind::Thought && kind != ChatBlockKind::Raw)
        style.fg = theme::text().fg;
    style.bold = header;
    style.dim = kind == ChatBlockKind::Thought || kind == ChatBlockKind::Raw;
    return style;
}

}  // namespace cortex::mk3::ui::chat
