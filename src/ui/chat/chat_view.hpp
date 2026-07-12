#pragma once
// Inkcell chat surface ported from the ReplSession composition contract:
// transcript viewport + truthful status line + prompt line. No per-row boxes,
// no scene-specific business logic, no dependency on src/tui.

#include <algorithm>
#include <set>
#include <sstream>
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
    size_t first = line.find_first_not_of(' ');
    std::string content = first == std::string::npos ? std::string() : line.substr(first);
    if (content.rfind("YOU", 0) == 0) return theme::green();
    if (content.rfind("CORTEX", 0) == 0) return theme::cyan();
    if (content.rfind("AGENT", 0) == 0 || content.rfind("TOOL", 0) == 0 ||
        content.rfind("FEED", 0) == 0 || content.rfind("RELIC", 0) == 0 ||
        content.rfind("WORKFLOW", 0) == 0 || content.rfind("ACTION", 0) == 0)
        return theme::amber();
    if (content.rfind("✓ RESULT", 0) == 0) return theme::green();
    if (content.rfind("✗", 0) == 0 || content.rfind("ERROR", 0) == 0) return theme::red();
    if (content.rfind("THOUGHT", 0) == 0 || content.rfind("RAW", 0) == 0) return theme::dim();
    if (content.rfind("┌─", 0) == 0 || content.rfind("└─", 0) == 0 ||
        content.rfind("│ ", 0) == 0) return theme::dim();
    if (content.rfind("# ", 0) == 0 || content.rfind("## ", 0) == 0 ||
        content.rfind("### ", 0) == 0) return theme::bright();
    if (line.rfind("    ", 0) == 0) return theme::text();
    return theme::dim();
}

inline void drawStatusLine(inkcell::Surface& surface, inkcell::Rect row, const ChatSurfaceModel& m) {
    auto st = m.failed ? theme::red() : m.running ? theme::green() : theme::dim();
    std::string state = m.status == "idle" ? "ready" : m.status;
    std::string left = std::string(m.running ? "●" : m.failed ? "✗" : "○") + " " + state;
    left += " · " + m.mode + " · " + theme::name();
    std::string right;
    if (m.running) {
        right = "pending " + std::to_string(m.pendingOps) + " · " +
                std::to_string(m.actionCount) + " actions · " +
                std::to_string(m.resultCount) + " results · " +
                std::to_string(m.tokenBytes) + "b";
    } else {
        right = m.hint;
    }
    int rightWidth = inkcell::text::display_width(right);
    surface.text({row.x, row.y}, inkcell::text::truncate(left, std::max(0, row.w - rightWidth - 2)), st);
    surface.text({std::max(row.x, row.right() - rightWidth), row.y}, inkcell::text::truncate(right, row.w), theme::dim());
}

inline void drawPromptLine(inkcell::Surface& surface, inkcell::Rect row, const ChatSurfaceModel& m) {
    std::string prompt = m.inputFocused ? "› " : "  ";
    std::string input;
    if (m.running) {
        input = "agent running…";
    } else if (m.input.empty()) {
        if (m.inputFocused) input = "█";
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
    std::string path = m.path.empty() ? "root" : m.path;
    std::string left = m.title + "  /  " + path;
    std::string backend = (m.provider.empty() ? "provider?" : m.provider) + "/" +
                          (m.model.empty() ? "default" : m.model);
    std::string right = backend + "  ·  " + suffix(m.sessionId);
    int rightWidth = inkcell::text::display_width(right);
    surface.text({frame.x, frame.y}, inkcell::text::truncate(left, std::max(0, frame.w - rightWidth - 2)), theme::bright());
    surface.text({std::max(frame.x, frame.right() - rightWidth), frame.y}, right, theme::dim());
}

inline std::vector<std::string> hardWrapUtf8(const std::string& value, int width) {
    std::vector<std::string> out;
    width = std::max(1, width);
    std::string line;
    int columns = 0;
    for (size_t i = 0; i < value.size();) {
        size_t len = inkcell::text::utf8_codepoint_len(static_cast<unsigned char>(value[i]));
        if (i + len > value.size()) len = 1;
        std::string glyph = value.substr(i, len);
        int glyphWidth = std::max(1, inkcell::text::display_width(glyph));
        if (!line.empty() && columns + glyphWidth > width) {
            out.push_back(line);
            line.clear();
            columns = 0;
        }
        line += glyph;
        columns += glyphWidth;
        i += len;
    }
    if (!line.empty() || out.empty()) out.push_back(line);
    return out;
}

inline std::vector<std::string> wrapWordsLossless(const std::string& value, int width) {
    std::vector<std::string> out;
    std::istringstream words(value);
    std::string word;
    std::string line;
    while (words >> word) {
        if (inkcell::text::display_width(word) > width) {
            if (!line.empty()) {
                out.push_back(line);
                line.clear();
            }
            auto chunks = hardWrapUtf8(word, width);
            out.insert(out.end(), chunks.begin(), chunks.end());
            continue;
        }
        int next = inkcell::text::display_width(line) + inkcell::text::display_width(word) +
                   (line.empty() ? 0 : 1);
        if (next > width) {
            out.push_back(line);
            line = word;
        } else {
            if (!line.empty()) line += ' ';
            line += word;
        }
    }
    if (!line.empty()) out.push_back(line);
    return out;
}

inline std::vector<std::string> wrapTranscript(const std::vector<std::string>& source, int width) {
    std::vector<std::string> out;
    width = std::max(1, width);
    bool inCode = false;
    for (const auto& original : source) {
        if (original.empty()) {
            out.push_back("");
            continue;
        }
        size_t indentSize = 0;
        while (indentSize < original.size() && original[indentSize] == ' ' && indentSize < 6) ++indentSize;
        std::string indent(indentSize, ' ');
        std::string content = original.substr(indentSize);
        int available = std::max(1, width - static_cast<int>(indentSize));
        if (content.rfind("```", 0) == 0) {
            if (!inCode) {
                std::string language = content.substr(3);
                size_t first = language.find_first_not_of(" \t");
                language = first == std::string::npos ? std::string() : language.substr(first);
                out.push_back(indent + "┌─" + (language.empty() ? std::string() : " " + language));
            } else {
                out.push_back(indent + "└─");
            }
            inCode = !inCode;
            continue;
        }
        if (inCode) {
            for (const auto& line : hardWrapUtf8(content, std::max(1, available - 2)))
                out.push_back(indent + "│ " + line);
            continue;
        }
        auto wrapped = wrapWordsLossless(content, available);
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
        bool selected = m.historyFocused && line.rfind("› ", 0) == 0;
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
    int statusY = frame.bottom() - 2;
    int promptY = frame.bottom() - 1;
    inkcell::Rect body{frame.x, frame.y + 2, frame.w, std::max(1, statusY - (frame.y + 2) - 1)};
    drawTranscript(surface, body, m);
    surface.hline({frame.x, statusY - 1}, frame.w, "─", theme::dim());
    drawStatusLine(surface, {frame.x, statusY, frame.w, 1}, m);
    drawPromptLine(surface, {frame.x, promptY, frame.w, 1}, m);
}

}  // namespace cortex::mk3::ui::chat
