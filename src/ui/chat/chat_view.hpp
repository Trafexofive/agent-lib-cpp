#pragma once
// Inkcell chat surface ported from the ReplSession composition contract:
// transcript viewport + truthful status line + prompt line. No per-row boxes,
// no scene-specific business logic, no dependency on src/tui.

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/chat/ask_dialog_model.hpp"
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
    left += "  mode:" + m.mode + "  theme:" + theme::name();
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
    if (total <= 0) return;
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

inline void drawHelpOverlay(inkcell::Surface& surface, inkcell::Rect page) {
    int width = std::max(44, std::min(page.w - 4, 76));
    int height = std::max(16, std::min(page.h - 4, 22));
    inkcell::Rect frame{page.x + (page.w - width) / 2, page.y + (page.h - height) / 2, width, height};
    surface.fill(frame, " ", theme::panel_2());
    surface.box(frame, inkcell::BorderStyle::Square, theme::dim());
    int x = frame.x + 2;
    int y = frame.y + 1;
    int inner = frame.w - 4;
    surface.text({x, y++}, "CHAT HELP", theme::bright());
    surface.text({x, y++}, std::string("theme: ") + theme::name(), theme::dim());
    ++y;
    const std::vector<std::string> lines = {
        "Enter       send prompt",
        "Up / Down   prompt history (composer)",
        "Esc         focus transcript / return to composer",
        "j / k       select transcript blocks",
        "Enter       open selected sub-agent",
        "t / r       toggle thoughts / raw",
        "T           switch graphite / neon",
        "Tab         complete slash command",
        "Ctrl-C      cancel active turn",
        "q           quit (outside composer)",
        "/help       command catalog",
    };
    for (const auto& line : lines) {
        if (y >= frame.bottom() - 2) break;
        surface.text({x, y++}, inkcell::text::truncate(line, inner), theme::text());
    }
    surface.text({x, frame.bottom() - 2}, "? or Esc close", theme::dim());
}

inline void drawAskDialog(inkcell::Surface& surface, inkcell::Rect page, const DialogState& state,
                          const std::string& input, const std::set<int>& multiSelected) {
    const DialogCard* card = state.current();
    if (!card) return;
    int width = std::max(40, std::min(page.w - 4, 92));
    int height = std::max(12, std::min(page.h - 4, 24));
    inkcell::Rect frame{page.x + (page.w - width) / 2, page.y + (page.h - height) / 2, width, height};
    surface.fill(frame, " ", theme::panel_2());
    surface.box(frame, inkcell::BorderStyle::Square, theme::cyan());
    int x = frame.x + 2;
    int y = frame.y + 1;
    int inner = frame.w - 4;
    surface.text({x, y++}, inkcell::text::truncate(state.chainTitle, inner), theme::cyan());
    surface.text({x, y++}, inkcell::text::truncate("card " + std::to_string(state.index + 1) + "/" +
                                                       std::to_string(state.cards.size()) + " · " + card->type,
                                                   inner), theme::dim());
    ++y;
    surface.text({x, y++}, inkcell::text::truncate(card->title.empty() ? card->id : card->title, inner), theme::bright());
    for (const auto& line : inkcell::text::wrap_words(card->message, inner)) {
        if (y >= frame.bottom() - 5) break;
        surface.text({x, y++}, line, theme::text());
    }
    if (!card->help.empty() && y < frame.bottom() - 5)
        surface.text({x, y++}, inkcell::text::truncate(card->help, inner), theme::dim());

    if (card->type == "choice" || card->type == "multi_choice" || card->type == "ranker") {
        ++y;
        for (int i = 0; i < static_cast<int>(card->options.size()) && y < frame.bottom() - 3; ++i) {
            const auto& option = card->options[static_cast<size_t>(i)];
            bool selected = i == state.selectedOption;
            bool checked = multiSelected.count(i) > 0;
            std::string marker = selected ? "> " : "  ";
            if (card->type == "multi_choice") marker += checked ? "[x] " : "[ ] ";
            surface.text({x, y++}, inkcell::text::truncate(marker + option.label, inner),
                         option.disabled ? theme::dim() : selected ? theme::selected_style() : theme::text());
        }
    } else {
        ++y;
        std::string shown = card->type == "secret" ? std::string(input.size(), '*') : input;
        surface.text({x, y}, inkcell::text::truncate("> " + shown + "█", inner), theme::bright());
    }

    if (!state.error.empty())
        surface.text({x, frame.bottom() - 3}, inkcell::text::truncate("error: " + state.error, inner), theme::red());
    std::string hint = (card->type == "choice") ? "↑↓/j/k select · Enter choose · Esc cancel"
                       : (card->type == "multi_choice") ? "↑↓ select · Space toggle · Enter done · Esc cancel"
                       : (card->type == "confirm") ? "y/n answer · Esc cancel"
                       : "Enter submit · Esc cancel";
    surface.text({x, frame.bottom() - 2}, inkcell::text::truncate(hint, inner), theme::dim());
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
