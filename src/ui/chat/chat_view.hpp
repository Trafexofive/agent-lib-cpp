#pragma once
// Inkcell chat surface ported from the ReplSession composition contract:
// transcript viewport + truthful status line + prompt line. No per-row boxes,
// no scene-specific business logic, no dependency on src/tui.

#include <algorithm>
#include <string>
#include <vector>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::chat {

struct ChatLine {
    std::string text;
    bool selected = false;
};

struct ChatSurfaceModel {
    std::string title = "CORTEX MK3";
    std::string path;
    std::string provider;
    std::string model;
    std::string sessionId;
    std::string status = "idle";
    std::string mode = "FULL";
    bool running = false;
    bool failed = false;
    bool inputFocused = true;
    bool historyFocused = false;
    bool showThoughts = false;
    bool showRaw = false;
    int pendingOps = 0;
    int actionCount = 0;
    int resultCount = 0;
    int tokenBytes = 0;
    int scrollOffset = 0;
    bool followBottom = true;
    std::vector<std::string> transcript;
    std::string input;
    int inputCursor = 0;
    std::string hint;
};

inline std::string suffix(const std::string& id) {
    if (id.empty()) return "no-session";
    return id.size() > 8 ? id.substr(id.size() - 8) : id;
}

inline inkcell::Style lineStyle(const std::string& line, bool selected) {
    if (selected) return theme::selected_style();
    if (line.rfind("> ", 0) == 0) return theme::green();
    if (line.rfind("  >", 0) == 0) return theme::green();
    if (line.find("✗") != std::string::npos || line.find("error") != std::string::npos) return theme::red();
    if (line.find("◆") != std::string::npos || line.find("action") != std::string::npos) return theme::amber();
    if (line.find("✓") != std::string::npos || line.find("final") != std::string::npos) return theme::text();
    if (line.find("thought") != std::string::npos) return theme::dim();
    return theme::text();
}

inline void drawStatusLine(inkcell::Surface& surface, inkcell::Rect row, const ChatSurfaceModel& m) {
    auto st = m.failed ? theme::red() : m.running ? theme::green() : theme::dim();
    std::string left = std::string(m.running ? "●" : m.failed ? "✗" : "○") + " " + m.status;
    left += "  " + (m.provider.empty() ? "provider?" : m.provider) + "/" + (m.model.empty() ? "default" : m.model);
    left += "  mode:" + m.mode;
    std::string right = "pending " + std::to_string(m.pendingOps) + " · actions " + std::to_string(m.actionCount) +
                        " · results " + std::to_string(m.resultCount) + " · bytes " + std::to_string(m.tokenBytes);
    surface.text({row.x, row.y}, inkcell::text::truncate(left, std::max(0, row.w - static_cast<int>(right.size()) - 2)), st);
    surface.text({std::max(row.x, row.right() - static_cast<int>(right.size())), row.y}, right, theme::dim());
}

inline void drawPromptLine(inkcell::Surface& surface, inkcell::Rect row, const ChatSurfaceModel& m) {
    std::string prompt = m.inputFocused ? "> " : "  ";
    std::string input;
    if (m.running) {
        input = "agent running…";
    } else if (m.input.empty()) {
        input = "message";
        if (m.inputFocused) input += "█";
    } else {
        input = m.input;
        if (m.inputFocused) {
            int cursor = std::max(0, std::min(m.inputCursor, static_cast<int>(input.size())));
            input.insert(static_cast<size_t>(cursor), "█");
        }
    }
    surface.text({row.x, row.y}, inkcell::text::truncate(prompt + input, row.w),
                 m.inputFocused ? theme::bright() : theme::dim());
}

inline void drawHeader(inkcell::Surface& surface, inkcell::Rect frame, const ChatSurfaceModel& m) {
    surface.text({frame.x, frame.y}, m.title, theme::cyan());
    std::string right = "session:…" + suffix(m.sessionId);
    surface.text({std::max(frame.x, frame.right() - static_cast<int>(right.size())), frame.y}, right, theme::dim());
    std::string path = m.path.empty() ? "root" : m.path;
    surface.text({frame.x, frame.y + 1}, inkcell::text::truncate(path, frame.w), theme::dim());
}

inline std::vector<std::string> wrapTranscript(const std::vector<std::string>& source, int width) {
    std::vector<std::string> out;
    width = std::max(1, width);
    for (const auto& original : source) {
        if (original.empty()) {
            out.push_back("");
            continue;
        }
        size_t indentSize = 0;
        while (indentSize < original.size() && original[indentSize] == ' ' && indentSize < 6) ++indentSize;
        std::string indent(indentSize, ' ');
        std::string content = original.substr(indentSize);
        auto wrapped = inkcell::text::wrap_words(content, std::max(1, width - static_cast<int>(indentSize)));
        if (wrapped.empty()) out.push_back(indent);
        else for (const auto& line : wrapped) out.push_back(indent + line);
    }
    return out;
}

inline void drawTranscript(inkcell::Surface& surface, inkcell::Rect body, const ChatSurfaceModel& m) {
    if (body.w <= 0 || body.h <= 0) return;
    auto displayLines = wrapTranscript(m.transcript, std::max(1, body.w - 1));
    int total = static_cast<int>(displayLines.size());
    if (total <= 0) {
        int y = body.y + std::max(0, body.h / 2 - 1);
        surface.text({body.x, y}, "No turns yet. Type a prompt and press Enter.", theme::dim());
        surface.text({body.x, y + 1}, "Esc history · Enter send · /help commands · Ctrl-C cancel", theme::dim());
        return;
    }
    int maxOffset = std::max(0, total - body.h);
    int offset = m.followBottom ? maxOffset : std::max(0, std::min(m.scrollOffset, maxOffset));
    int visible = std::min(body.h, total - offset);
    int firstY = body.y + std::max(0, body.h - visible);  // ReplSession-style bottom anchoring.
    for (int y = 0; y < visible; ++y) {
        int idx = offset + y;
        const auto& line = displayLines[static_cast<size_t>(idx)];
        bool selected = m.historyFocused && line.rfind("> ", 0) == 0;
        surface.text({body.x, firstY + y}, inkcell::text::fit_left(line, body.w), lineStyle(line, selected));
    }
    if (total > body.h && body.w > 4) {
        int thumb = std::max(1, body.h * body.h / total);
        int thumbY = (offset * std::max(1, body.h - thumb)) / std::max(1, maxOffset);
        for (int y = 0; y < body.h; ++y) {
            surface.put({body.right() - 1, body.y + y}, (y >= thumbY && y < thumbY + thumb) ? "│" : "┆", theme::dim());
        }
    }
}

inline void drawChatSurface(inkcell::Surface& surface, inkcell::Rect frame, const ChatSurfaceModel& m) {
    surface.clear(theme::base_bg());
    if (frame.w <= 0 || frame.h <= 0) return;
    // One flat page, no nested boxes. Leave the app-level page inset to caller.
    drawHeader(surface, frame, m);
    int statusY = frame.bottom() - 3;
    int promptY = frame.bottom() - 1;
    inkcell::Rect body{frame.x, frame.y + 3, frame.w, std::max(1, statusY - (frame.y + 3) - 1)};
    drawTranscript(surface, body, m);
    surface.hline({frame.x, statusY - 1}, frame.w, "─", theme::dim());
    drawStatusLine(surface, {frame.x, statusY, frame.w, 1}, m);
    std::string hint = m.hint.empty() ? "Enter send · Esc history · i composer · t thoughts · r raw · q quit" : m.hint;
    surface.text({frame.x, statusY + 1}, inkcell::text::truncate(hint, frame.w), theme::dim());
    drawPromptLine(surface, {frame.x, promptY, frame.w, 1}, m);
}

}  // namespace cortex::mk3::ui::chat
