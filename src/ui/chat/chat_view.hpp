#pragma once
// Inkcell chat surface ported from the ReplSession composition contract:
// transcript viewport + truthful status line + prompt line. No per-row boxes,
// no scene-specific business logic, no dependency on src/tui.

#include <algorithm>
#include <cstdint>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "inkcell/widgets/status_bar.hpp"
#include "inkcell/command.hpp"
#include "src/ui/chat/ask_dialog_model.hpp"
#include "src/ui/chat/chat_blocks.hpp"
#include "src/ui/chat/notification.hpp"
#include "src/ui/chat/transcript_cache.hpp"
#include "src/ui/theme/cortex_theme.hpp"
#include "src/ui/model/inkcell_commands.hpp"

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
    int64_t turnElapsedMs = 0;   // live elapsed while running, else last turn
    int64_t lastTurnElapsedMs = 0;
    uint64_t nowMs = 0;          // animation clock (steady_clock ms)
    int scrollOffset = 0;
    bool followBottom = true;
    std::vector<std::string> transcript;  // standalone/test fallback
    const std::vector<std::string>* transcriptSource = nullptr;
    uint64_t transcriptVersion = 0;
    TranscriptWrapCache* transcriptCache = nullptr;
    std::string input;
    int inputCursor = 0;
    int inputScrollRow = 0;   // first visible logical/wrapped row in multi-line prompt
    int inputMaxRows = 8;     // hard cap for grow-with-content prompt box
    std::string hint;
    std::string agentName;  // real agent display name for the assistant label (replaces CORTEX)
    std::string scopeName;  // drilled-in subagent name (empty at root) for header/status scope indicator
    // Transient readline-style completion menu (NOT transcript history).
    // Drawn between the body separator and the status line; cleared when the
    // operator types, submits, or leaves the stem.
    std::vector<std::string> completionMenu;
    int completionSelected = -1;  // index highlighted while cycling; -1 = none
    // Optional transient feedback strip (above status). Not owned; may be null.
    const NotificationStack* notifications = nullptr;
};

inline std::string suffix(const std::string& id) {
    if (id.empty()) return "no-session";
    return id.size() > 8 ? id.substr(id.size() - 8) : id;
}

inline inkcell::Style lineStyle(const std::string& line, bool selected,
                             const std::string& agentName = {}) {
    if (selected) return theme::selected_style();
    size_t first = line.find_first_not_of(' ');
    std::string content = first == std::string::npos ? std::string() : line.substr(first);
    if (content.rfind("YOU", 0) == 0 || content.rfind("PARENT", 0) == 0) return theme::green();
    // Assistant label is the real agent name (or CORTEX fallback) + meta.
    if (!agentName.empty() && content.rfind(agentName, 0) == 0) return theme::cyan();
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

inline std::string fmtCompactBytes(int bytes) {
    if (bytes < 1024) return std::to_string(bytes) + "B";
    if (bytes < 1024 * 1024) {
        int tenths = (bytes * 10) / 1024;
        return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "KB";
    }
    int tenths = (bytes * 10) / (1024 * 1024);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "MB";
}

inline std::string fmtCompactElapsed(int64_t ms) {
    if (ms < 0) ms = 0;
    if (ms < 1000) return std::to_string(static_cast<int>(ms)) + "ms";
    if (ms < 60000) {
        int tenths = static_cast<int>(ms / 100);
        return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "s";
    }
    int secs = static_cast<int>(ms / 1000);
    int m = secs / 60;
    int s = secs % 60;
    return std::to_string(m) + "m" + (s < 10 ? "0" : "") + std::to_string(s) + "s";
}

// Braille spinner — only while running. Phase from nowMs (~12.5 fps feel).
inline const char* liveSpinner(uint64_t nowMs) {
    static const char* kFrames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    return kFrames[(nowMs / 80) % 10];
}

// Blinking block cursor when focused; solid when typing recently is overkill —
// simple 530ms duty cycle keeps it zen without fighting the input stream.
inline bool cursorVisible(uint64_t nowMs, bool focused) {
    if (!focused) return false;
    return ((nowMs / 530) % 2) == 0;
}

// Elevated footer: status metrics row + prompt row on a raised panel.
// No placeholder copy. Running state is the spinner + live chips, never the
// input text. Completion menus sit above this block (caller reserves space).
inline void drawStatusLine(inkcell::Surface& surface, inkcell::Rect row, const ChatSurfaceModel& m) {
    const bool focusBar = m.inputFocused && !m.running;
    auto bg = focusBar ? theme::footer_bg_focus() : theme::footer_bg();
    surface.fill(row, " ", bg);

    // Live alert (retry / protocol) rides the status line — no second row.
    const chat::Notification* alert =
        (m.notifications && !m.notifications->empty()) ? m.notifications->top() : nullptr;

    // Product accent tick; metrics via inkcell StatusBar segments (fill off).
    auto accent = alert && alert->severity == "error" ? theme::red().with_bg(bg.bg)
                  : alert && alert->severity == "warn"  ? theme::footer_warn()
                  : m.running   ? theme::footer_accent_live()
                  : m.failed  ? theme::footer_warn()
                  : focusBar  ? theme::footer_accent_focus()
                              : theme::footer_accent_idle();
    if (alert && (alert->severity == "error" || alert->severity == "warn")) accent.bold = true;
    surface.text({row.x, row.y}, "▌", accent);

    std::string glyph = m.running ? liveSpinner(m.nowMs)
                        : m.failed ? "✗"
                                   : "○";
    std::string state = m.status.empty() || m.status == "idle" ? "ready" : m.status;
    if (state.rfind("cancelling", 0) == 0) state = "cancelling";
    int64_t elapsed = m.running ? m.turnElapsedMs : m.lastTurnElapsedMs;

    inkcell::widgets::StatusBar bar;
    auto inkTheme = theme::activeInkcellTheme();
    bar.theme(inkTheme).separator(" ").fill_background(false);
    bar.left_seg(glyph + " " + state,
                 m.failed ? inkcell::Role::Error
                 : m.running ? inkcell::Role::Success
                             : inkcell::Role::TextMuted,
                 m.running || m.failed);
    if (m.running || elapsed > 0)
        bar.left_seg(fmtCompactElapsed(elapsed), inkcell::Role::Info);
    if (!m.scopeName.empty())
        bar.left_seg("◀" + m.scopeName, inkcell::Role::Accent);
    if (m.pendingOps > 0)
        bar.left_seg("pend" + std::to_string(m.pendingOps), inkcell::Role::Warning, true);
    if (m.actionCount > 0)
        bar.left_seg("act" + std::to_string(m.actionCount), inkcell::Role::TextMuted);
    if (m.resultCount > 0)
        bar.left_seg("res" + std::to_string(m.resultCount), inkcell::Role::TextMuted);
    if (m.tokenBytes > 0)
        bar.left_seg(fmtCompactBytes(m.tokenBytes), inkcell::Role::TextMuted);

    // Alert first on the right (highest priority transient), then mode/theme.
    if (alert) {
        std::string a;
        if (!alert->source.empty()) {
            a = alert->source;
            a += " · ";
        }
        a += alert->title;
        if (alert->maxAttempts > 0) {
            a += " ";
            a += std::to_string(std::max(1, alert->attempt));
            a += "/";
            a += std::to_string(alert->maxAttempts);
        }
        // Keep metrics readable — cap alert width roughly.
        if (a.size() > 42) a = a.substr(0, 39) + "…";
        inkcell::Role ar = inkcell::Role::Info;
        bool bold = false;
        if (alert->severity == "error") {
            ar = inkcell::Role::Error;
            bold = true;
        } else if (alert->severity == "warn") {
            ar = inkcell::Role::Warning;
            bold = true;
        }
        bar.right_seg(std::move(a), ar, bold);
    }
    bar.right_seg(m.mode, inkcell::Role::TextMuted);
    bar.right_seg(theme::name(), inkcell::Role::Ghost);

    inkcell::Rect barRow{row.x + 2, row.y, std::max(0, row.w - 2), row.h};
    bar.draw(surface, barRow);
}

// Soft-wrap helper for the multi-line prompt box. Logical \n lines first,
// then hard-wrap by display width so long lines don't clip into oblivion.
inline std::vector<std::string> promptDisplayLines(const std::string& input, int maxW) {
    if (maxW < 1) maxW = 1;
    std::vector<std::string> out;
    auto logical = inkcell::text::split_lines(input);
    if (logical.empty()) logical.push_back({});
    for (const auto& ln : logical) {
        if (ln.empty()) {
            out.push_back({});
            continue;
        }
        // Greedy codepoint wrap by display width (no word-break heroics).
        std::string cur;
        size_t i = 0;
        while (i < ln.size()) {
            size_t len = 1;
            while (i + len < ln.size() &&
                   (static_cast<unsigned char>(ln[i + len]) & 0xc0) == 0x80)
                ++len;
            std::string cp = ln.substr(i, len);
            if (!cur.empty() &&
                inkcell::text::display_width(cur) + inkcell::text::display_width(cp) > maxW) {
                out.push_back(cur);
                cur.clear();
            }
            cur += cp;
            i += len;
        }
        out.push_back(cur);
    }
    if (out.empty()) out.push_back({});
    return out;
}

// Map a byte cursor in `input` onto a display-row index under the same wrap
// rules as promptDisplayLines. Used for scroll + cursor paint.
inline int promptCursorDisplayRow(const std::string& input, int cursor, int maxW) {
    if (maxW < 1) maxW = 1;
    cursor = std::max(0, std::min(cursor, static_cast<int>(input.size())));
    // Prefix up to cursor, then count display rows of that prefix. If cursor
    // sits on a trailing newline, split_lines yields an extra empty line —
    // which is the row we want.
    std::string prefix = input.substr(0, static_cast<size_t>(cursor));
    auto rows = promptDisplayLines(prefix, maxW);
    int row = static_cast<int>(rows.size()) - 1;
    if (row < 0) row = 0;
    // If prefix ends with \n, last row is empty and correct. If not, still last.
    return row;
}

// Height of the grow-with-content prompt box (1..inputMaxRows).
inline int promptBoxHeight(const ChatSurfaceModel& m, int frameW) {
    const int maxW = std::max(1, frameW - 4);  // accent + gutter
    if (m.input.empty()) return 1;
    auto rows = promptDisplayLines(m.input, maxW);
    int n = static_cast<int>(rows.size());
    int cap = std::max(1, m.inputMaxRows);
    if (n < 1) n = 1;
    if (n > cap) n = cap;
    return n;
}

inline void drawPromptBox(inkcell::Surface& surface, inkcell::Rect box, const ChatSurfaceModel& m) {
    // Multi-line elevated prompt (pi-style). Grows with content up to box.h.
    // Single-row footer was the usability killer: newlines existed in state but
    // painted as one flattened horizontal scrap.
    if (box.w <= 0 || box.h <= 0) return;
    const bool focusBar = m.inputFocused;
    auto bg = focusBar ? theme::footer_bg_focus() : theme::footer_bg();
    surface.fill(box, " ", bg);

    auto accent = m.running  ? theme::footer_accent_live()
                  : focusBar ? theme::footer_accent_focus()
                             : theme::footer_accent_idle();
    auto textSt = focusBar ? theme::footer_bright() : theme::footer_dim();
    auto dim = theme::footer_dim();
    const int maxW = std::max(1, box.w - 4);

    // Left accent bar on every prompt row.
    for (int r = 0; r < box.h; ++r)
        surface.text({box.x, box.y + r}, "▌", accent);

    if (m.input.empty() && !m.running && focusBar) {
        surface.text({box.x + 2, box.y},
                     inkcell::text::truncate(
                         "› type to run  ·  ↵ send  ·  ⇧↵ newline  ·  / commands  ·  ? help",
                         maxW),
                     dim);
        return;
    }

    auto lines = promptDisplayLines(m.input, maxW);
    int scroll = std::max(0, m.inputScrollRow);
    if (scroll > static_cast<int>(lines.size()) - 1)
        scroll = std::max(0, static_cast<int>(lines.size()) - 1);

    // Cursor location in display rows + column within that row.
    const int cursor = std::max(0, std::min(m.inputCursor, static_cast<int>(m.input.size())));
    const int cursorRow = promptCursorDisplayRow(m.input, cursor, maxW);
    // Column: re-wrap the logical line that contains the cursor and measure.
    int cursorCol = 0;
    {
        // Find byte offset of start of the display-row's content by replaying wrap
        // on the full prefix — simpler: width of last display line of prefix.
        std::string prefix = m.input.substr(0, static_cast<size_t>(cursor));
        auto prefRows = promptDisplayLines(prefix, maxW);
        if (!prefRows.empty())
            cursorCol = inkcell::text::display_width(prefRows.back());
    }
    const bool showCursor = cursorVisible(m.nowMs, focusBar);

    for (int r = 0; r < box.h; ++r) {
        int li = scroll + r;
        std::string line = (li >= 0 && li < static_cast<int>(lines.size()))
                               ? lines[static_cast<size_t>(li)]
                               : std::string();
        // First visible row gets the › glyph; continuation rows indent to align.
        const char* g = (r == 0 && focusBar) ? "› " : "  ";
        std::string shown = std::string(g) + line;

        if (focusBar && showCursor && li == cursorRow) {
            // Paint block cursor by inserting █ at display column in the raw line,
            // then re-prefix glyph. Approximate: insert at byte pos matching col.
            std::string withCur = line;
            // Walk display width to byte offset.
            int col = 0;
            size_t bi = 0;
            while (bi < withCur.size() && col < cursorCol) {
                size_t len = 1;
                while (bi + len < withCur.size() &&
                       (static_cast<unsigned char>(withCur[bi + len]) & 0xc0) == 0x80)
                    ++len;
                col += inkcell::text::display_width(withCur.substr(bi, len));
                bi += len;
            }
            withCur.insert(bi, "█");
            shown = std::string(g) + withCur;
        } else if (focusBar && !showCursor && li == cursorRow &&
                   cursor >= static_cast<int>(m.input.size()) &&
                   r == box.h - 1) {
            shown.push_back(' ');  // layout stable at EOL blink-off
        }

        surface.text({box.x + 2, box.y + r},
                     inkcell::text::truncate(shown, maxW + 2),
                     textSt);
    }

    // Scroll affordance when content exceeds box height.
    if (static_cast<int>(lines.size()) > box.h) {
        auto mark = theme::footer_dim();
        if (scroll > 0)
            surface.text({box.x + box.w - 1, box.y}, "▴", mark);
        if (scroll + box.h < static_cast<int>(lines.size()))
            surface.text({box.x + box.w - 1, box.y + box.h - 1}, "▾", mark);
    }
}

// Back-compat name used by older call sites.
inline void drawPromptLine(inkcell::Surface& surface, inkcell::Rect row, const ChatSurfaceModel& m) {
    drawPromptBox(surface, row, m);
}

inline void drawHeader(inkcell::Surface& surface, inkcell::Rect frame, const ChatSurfaceModel& m) {
    std::string path = m.path.empty() ? "root" : m.path;
    // Render the title + path with a highlighted drilled-in segment so the
    // operator can see WHERE they are in the agent tree at a glance. The root
    // segment stays bright; the current scope (last path segment) is rendered
    // in the agent-amber color to match AGENT blocks. Intermediate segments
    // are dim to convey the breadcrumb hierarchy.
    std::string left = m.title + "  /  ";
    int x = frame.x;
    int rightReserve = std::max(0, frame.w - x);
    surface.text({x, frame.y}, inkcell::text::truncate(left, rightReserve), theme::bright());
    int used = inkcell::text::display_width(left);
    x += used; rightReserve = std::max(0, frame.w - x);
    if (rightReserve <= 0) return;
    // Split the path on " / " and render each segment with hierarchy styling.
    std::vector<std::string> segments;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(" / ", start);
        if (end == std::string::npos) { segments.push_back(path.substr(start)); break; }
        segments.push_back(path.substr(start, end - start));
        start = end + 3;
    }
    auto segStyle = [&](size_t i) -> inkcell::Style {
        if (i + 1 == segments.size() && !m.scopeName.empty()) return theme::amber(); // current scope
        if (i == 0) return theme::bright();                                            // root
        return theme::dim();                                                            // intermediate
    };
    for (size_t i = 0; i < segments.size() && rightReserve > 0; ++i) {
        if (i > 0) {
            std::string sep = " / ";
            surface.text({x, frame.y}, inkcell::text::truncate(sep, rightReserve), theme::dim());
            int w = inkcell::text::display_width(sep);
            x += w; rightReserve = std::max(0, frame.w - x);
            if (rightReserve <= 0) break;
        }
        surface.text({x, frame.y}, inkcell::text::truncate(segments[i], rightReserve), segStyle(i));
        int w = inkcell::text::display_width(segments[i]);
        x += w; rightReserve = std::max(0, frame.w - x);
    }
    // Backend + session suffix on the right edge, dim.
    std::string backend = (m.provider.empty() ? "provider?" : m.provider) + "/" +
                          (m.model.empty() ? "default" : m.model);
    std::string right = backend + "  ·  " + suffix(m.sessionId);
    int rightWidth = inkcell::text::display_width(right);
    surface.text({std::max(frame.x, frame.right() - rightWidth), frame.y},
                 inkcell::text::truncate(right, std::max(0, frame.w - rightWidth - 2)), theme::dim());
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

// Count display lines for one source row without allocating the strings.
// Must stay semantically aligned with wrapTranscriptRange for scroll math.
inline int countTranscriptLineSpan(const std::string& original, int width, bool& inCode,
                                   const std::string& agentName = {}) {
    width = std::max(1, width);
    if (original.empty()) return 1;
    size_t indentSize = 0;
    while (indentSize < original.size() && original[indentSize] == ' ' && indentSize < 6) ++indentSize;
    std::string content = original.substr(indentSize);
    int available = std::max(1, width - static_cast<int>(indentSize));
    const std::string selectionPrefix = "› ";
    std::string semanticProbe = content.rfind(selectionPrefix, 0) == 0
                                    ? content.substr(selectionPrefix.size())
                                    : content;
    bool semanticHeader = semanticProbe.rfind("YOU", 0) == 0 ||
                          (!agentName.empty() && semanticProbe.rfind(agentName, 0) == 0) ||
                          semanticProbe.rfind("CORTEX", 0) == 0 ||
                          semanticProbe.rfind("AGENT", 0) == 0 || semanticProbe.rfind("TOOL", 0) == 0 ||
                          semanticProbe.rfind("FEED", 0) == 0 || semanticProbe.rfind("RELIC", 0) == 0 ||
                          semanticProbe.rfind("WORKFLOW", 0) == 0 || semanticProbe.rfind("ACTION", 0) == 0 ||
                          semanticProbe.rfind("✓ RESULT", 0) == 0 || semanticProbe.rfind("✗ RESULT", 0) == 0 ||
                          semanticProbe.rfind("THOUGHT", 0) == 0 || semanticProbe.rfind("RAW", 0) == 0 ||
                          semanticProbe.rfind("✗ ERROR", 0) == 0;
    if (semanticHeader) {
        return std::max(1, static_cast<int>(hardWrapUtf8(content, available).size()));
    }
    if (content.rfind("```", 0) == 0) {
        inCode = !inCode;
        return 1;
    }
    if (inCode) {
        return std::max(1, static_cast<int>(hardWrapUtf8(content, std::max(1, available - 2)).size()));
    }
    auto wrapped = wrapWordsLossless(content, available);
    return std::max(1, static_cast<int>(wrapped.size()));
}

inline void countTranscriptRangeSpans(const std::vector<std::string>& source, size_t begin, size_t end,
                                      int width, bool inCodeInit, std::vector<int>& spans,
                                      std::vector<bool>& inCodeAfter,
                                      const std::string& agentName = {}) {
    width = std::max(1, width);
    bool inCode = inCodeInit;
    for (size_t idx = begin; idx < end; ++idx) {
        int n = countTranscriptLineSpan(source[idx], width, inCode, agentName);
        spans.push_back(n);
        inCodeAfter.push_back(inCode);
    }
}

inline void wrapTranscriptRange(const std::vector<std::string>& source, size_t begin, size_t end,
                                int width, bool inCodeInit,
                                std::vector<std::string>& out,
                                std::vector<int>& spans,
                                std::vector<bool>& inCodeAfter,
                                const std::string& agentName = {}) {
    width = std::max(1, width);
    bool inCode = inCodeInit;
    for (size_t idx = begin; idx < end; ++idx) {
        const auto& original = source[idx];
        size_t before = out.size();
        if (original.empty()) {
            out.push_back("");
            spans.push_back(static_cast<int>(out.size() - before));
            inCodeAfter.push_back(inCode);
            continue;
        }
        size_t indentSize = 0;
        while (indentSize < original.size() && original[indentSize] == ' ' && indentSize < 6) ++indentSize;
        std::string indent(indentSize, ' ');
        std::string content = original.substr(indentSize);
        int available = std::max(1, width - static_cast<int>(indentSize));
        const std::string selectionPrefix = "› ";
        std::string semanticProbe = content.rfind(selectionPrefix, 0) == 0
                                        ? content.substr(selectionPrefix.size())
                                        : content;
        bool semanticHeader = semanticProbe.rfind("YOU", 0) == 0 ||
                              (!agentName.empty() && semanticProbe.rfind(agentName, 0) == 0) ||
                              semanticProbe.rfind("CORTEX", 0) == 0 ||
                              semanticProbe.rfind("AGENT", 0) == 0 || semanticProbe.rfind("TOOL", 0) == 0 ||
                              semanticProbe.rfind("FEED", 0) == 0 || semanticProbe.rfind("RELIC", 0) == 0 ||
                              semanticProbe.rfind("WORKFLOW", 0) == 0 || semanticProbe.rfind("ACTION", 0) == 0 ||
                              semanticProbe.rfind("✓ RESULT", 0) == 0 || semanticProbe.rfind("✗ RESULT", 0) == 0 ||
                              semanticProbe.rfind("THOUGHT", 0) == 0 || semanticProbe.rfind("RAW", 0) == 0 ||
                              semanticProbe.rfind("✗ ERROR", 0) == 0;
        if (semanticHeader) {
            for (const auto& line : hardWrapUtf8(content, available)) out.push_back(indent + line);
        } else if (content.rfind("```", 0) == 0) {
            if (!inCode) {
                std::string language = content.substr(3);
                size_t first = language.find_first_not_of(" \t");
                language = first == std::string::npos ? std::string() : language.substr(first);
                out.push_back(indent + "┌─" + (language.empty() ? std::string() : " " + language));
            } else {
                out.push_back(indent + "└─");
            }
            inCode = !inCode;
        } else if (inCode) {
            for (const auto& line : hardWrapUtf8(content, std::max(1, available - 2)))
                out.push_back(indent + "│ " + line);
        } else {
            auto wrapped = wrapWordsLossless(content, available);
            if (wrapped.empty()) out.push_back(indent);
            else for (const auto& line : wrapped) out.push_back(indent + line);
        }
        spans.push_back(static_cast<int>(out.size() - before));
        inCodeAfter.push_back(inCode);
    }
}

inline std::vector<std::string> wrapTranscript(const std::vector<std::string>& source, int width,
                                              const std::string& agentName = {}) {
    std::vector<std::string> out;
    std::vector<int> spans;
    std::vector<bool> inCodeAfter;
    wrapTranscriptRange(source, 0, source.size(), width, false, out, spans, inCodeAfter, agentName);
    return out;
}

inline void buildBlockMetadata(const std::vector<std::string>& lines,
                               std::vector<uint8_t>& kinds,
                               std::vector<bool>& headers,
                               std::vector<bool>& selected,
                               const std::string& agentName = {}) {
    kinds.assign(lines.size(), static_cast<uint8_t>(ChatBlockKind::None));
    headers.assign(lines.size(), false);
    selected.assign(lines.size(), false);
    ChatBlockKind currentKind = ChatBlockKind::None;
    bool currentSelected = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];
        // Empty separator lines inherit the current block's kind so the block
        // background flows through them — contiguous rendering with no base-bg
        // gutters between colored blocks. Separators stay marker-less (header
        // is false) so the ━▎ only marks real block headers.
        if (line.empty()) {
            kinds[i] = static_cast<uint8_t>(currentKind);
            headers[i] = false;
            selected[i] = currentSelected;
            continue;
        }
        bool header = line.rfind("    ", 0) != 0;
        if (header) {
            currentKind = classifyChatBlock(line, agentName);
            currentSelected = line.rfind("› ", 0) == 0;
        }
        kinds[i] = static_cast<uint8_t>(currentKind);
        headers[i] = header;
        selected[i] = currentSelected;
    }
}

inline int sumSpans(const std::vector<int>& spans) {
    int t = 0;
    for (int s : spans) t += s;
    return t;
}

// Map a display-line offset → first source index whose span range covers it.
// displayStartOut = display line index where that source line begins.
inline size_t sourceIndexAtDisplayOffset(const std::vector<int>& spans, int displayOffset,
                                         int& displayStartOut) {
    int acc = 0;
    for (size_t i = 0; i < spans.size(); ++i) {
        int next = acc + spans[i];
        if (displayOffset < next) {
            displayStartOut = acc;
            return i;
        }
        acc = next;
    }
    displayStartOut = acc;
    return spans.size();
}

// Ensure span map (+ optional full materialization) matches source/version/width.
// virtualize=true → only update spans/snapshot; leave cache.lines empty/unused.
inline void syncTranscriptWrapCache(TranscriptWrapCache& cache,
                                    const std::vector<std::string>& source, int wrapWidth,
                                    uint64_t version, const std::string& agentName,
                                    bool virtualize) {
    if (cache.sourceVersion == version && cache.width == wrapWidth &&
        cache.sourceSnapshot.size() == source.size() &&
        cache.sourceLineSpans.size() == source.size()) {
        // Still valid. For non-virtualized path, lines must exist.
        if (!virtualize && cache.lines.empty() && !source.empty()) {
            // Fall through to rebuild materialization.
        } else {
            return;
        }
    }

    const bool sameWidth = cache.width == wrapWidth;
    if (!sameWidth || cache.sourceSnapshot.empty() || cache.sourceLineSpans.empty()) {
        cache.sourceLineSpans.clear();
        cache.inCodeAfter.clear();
        cache.lines.clear();
        if (virtualize) {
            countTranscriptRangeSpans(source, 0, source.size(), wrapWidth, false,
                                      cache.sourceLineSpans, cache.inCodeAfter, agentName);
        } else {
            wrapTranscriptRange(source, 0, source.size(), wrapWidth, false, cache.lines,
                                cache.sourceLineSpans, cache.inCodeAfter, agentName);
        }
        cache.sourceSnapshot = source;
    } else {
        // Incremental dirty-tail update of spans (and lines if not virtualizing).
        size_t d = 0;
        const auto& snap = cache.sourceSnapshot;
        while (d < source.size() && d < snap.size() && source[d] == snap[d]) ++d;
        int stableEnd = 0;
        for (size_t i = 0; i < d && i < cache.sourceLineSpans.size(); ++i)
            stableEnd += cache.sourceLineSpans[i];
        cache.sourceLineSpans.resize(d);
        cache.inCodeAfter.resize(d);
        bool inCode = d > 0 ? cache.inCodeAfter[d - 1] : false;
        if (virtualize) {
            countTranscriptRangeSpans(source, d, source.size(), wrapWidth, inCode,
                                      cache.sourceLineSpans, cache.inCodeAfter, agentName);
            cache.lines.clear();  // invalidate full materialization
        } else {
            cache.lines.resize(static_cast<size_t>(stableEnd));
            wrapTranscriptRange(source, d, source.size(), wrapWidth, inCode, cache.lines,
                                cache.sourceLineSpans, cache.inCodeAfter, agentName);
        }
        cache.sourceSnapshot = source;
    }
    cache.blockKinds.clear();
    cache.blockHeaders.clear();
    cache.blockSelected.clear();
    cache.viewportOffset = -1;
    cache.viewportH = -1;
    cache.viewportLines.clear();
    cache.totalDisplayLines = sumSpans(cache.sourceLineSpans);
    cache.sourceVersion = version;
    cache.width = wrapWidth;
}

// Materialize display lines covering [displayOffset, displayOffset+height).
inline void materializeViewport(TranscriptWrapCache& cache, const std::vector<std::string>& source,
                                int wrapWidth, int displayOffset, int height,
                                const std::string& agentName) {
    if (height <= 0) {
        cache.viewportLines.clear();
        cache.viewportOffset = displayOffset;
        cache.viewportH = 0;
        return;
    }
    // Reuse if same window already painted.
    if (cache.viewportOffset == displayOffset && cache.viewportH == height &&
        !cache.viewportLines.empty()) {
        return;
    }
    int overscan = std::max(4, height / 2);
    int winStart = std::max(0, displayOffset - overscan);
    int winEnd = displayOffset + height + overscan;

    int srcDisplayStart = 0;
    size_t srcBegin =
        sourceIndexAtDisplayOffset(cache.sourceLineSpans, winStart, srcDisplayStart);
    // Expand srcEnd until we cover winEnd display lines.
    size_t srcEnd = srcBegin;
    int covered = srcDisplayStart;
    while (srcEnd < cache.sourceLineSpans.size() && covered < winEnd) {
        covered += cache.sourceLineSpans[srcEnd];
        ++srcEnd;
    }
    if (srcBegin > source.size()) srcBegin = source.size();
    if (srcEnd > source.size()) srcEnd = source.size();

    bool inCode = srcBegin > 0 && srcBegin - 1 < cache.inCodeAfter.size()
                      ? cache.inCodeAfter[srcBegin - 1]
                      : false;
    cache.viewportLines.clear();
    std::vector<int> localSpans;
    std::vector<bool> localInCode;
    wrapTranscriptRange(source, srcBegin, srcEnd, wrapWidth, inCode, cache.viewportLines,
                        localSpans, localInCode, agentName);
    // viewportLines[0] corresponds to display line srcDisplayStart.
    // Trim leading lines if winStart > srcDisplayStart.
    int lead = winStart - srcDisplayStart;
    if (lead > 0 && lead < static_cast<int>(cache.viewportLines.size())) {
        cache.viewportLines.erase(cache.viewportLines.begin(),
                                  cache.viewportLines.begin() + lead);
        // winStart is now the base of viewportLines[0]
    } else if (lead >= static_cast<int>(cache.viewportLines.size())) {
        cache.viewportLines.clear();
    }
    // Now viewportLines[0] is display line winStart (or empty).
    // We need lines starting at displayOffset.
    int skip = displayOffset - winStart;
    if (skip > 0 && skip < static_cast<int>(cache.viewportLines.size())) {
        cache.viewportLines.erase(cache.viewportLines.begin(),
                                  cache.viewportLines.begin() + skip);
    } else if (skip >= static_cast<int>(cache.viewportLines.size())) {
        cache.viewportLines.clear();
    }
    if (static_cast<int>(cache.viewportLines.size()) > height)
        cache.viewportLines.resize(static_cast<size_t>(height));

    buildBlockMetadata(cache.viewportLines, cache.viewportKinds, cache.viewportHeaders,
                       cache.viewportSelected, agentName);
    cache.viewportOffset = displayOffset;
    cache.viewportH = height;
}

inline void drawTranscript(inkcell::Surface& surface, inkcell::Rect body, const ChatSurfaceModel& m) {
    if (body.w <= 0 || body.h <= 0) return;
    const auto& source = m.transcriptSource ? *m.transcriptSource : m.transcript;
    int wrapWidth = std::max(1, body.w - 1);
    const bool virtualize =
        m.transcriptCache && source.size() >= kViewportVirtualizeSourceThreshold;

    std::vector<std::string> uncachedLines;
    const std::vector<std::string>* displayLinesPtr = nullptr;
    int total = 0;
    int offset = 0;
    bool useViewportWindow = false;

    if (m.transcriptCache) {
        auto& cache = *m.transcriptCache;
        syncTranscriptWrapCache(cache, source, wrapWidth, m.transcriptVersion, m.agentName,
                                virtualize);
        total = cache.totalDisplayLines;
        if (total <= 0 && source.empty()) {
            // empty state below
        } else if (virtualize) {
            useViewportWindow = true;
            int maxOffset = std::max(0, total - body.h);
            offset = m.followBottom ? maxOffset : std::max(0, std::min(m.scrollOffset, maxOffset));
            // historyFocused: find › in source and estimate — scan source headers cheaply
            if (m.historyFocused) {
                int acc = 0;
                for (size_t i = 0; i < source.size() && i < cache.sourceLineSpans.size(); ++i) {
                    if (source[i].rfind("› ", 0) == 0) {
                        offset = std::max(0, std::min(maxOffset, acc - body.h / 3));
                        break;
                    }
                    acc += cache.sourceLineSpans[i];
                }
            }
            materializeViewport(cache, source, wrapWidth, offset, body.h, m.agentName);
            displayLinesPtr = &cache.viewportLines;
        } else {
            displayLinesPtr = &cache.lines;
            total = static_cast<int>(cache.lines.size());
            // Keep totalDisplayLines in sync for non-virtual path.
            cache.totalDisplayLines = total;
        }
    } else {
        uncachedLines = wrapTranscript(source, wrapWidth, m.agentName);
        displayLinesPtr = &uncachedLines;
        total = static_cast<int>(uncachedLines.size());
    }

    if (total <= 0) {
        const std::string headline = "No conversation yet";
        const std::string tip = "Type a prompt below and press Enter \xe2\x80\x94 or press ? for help";
        int yHeadline = body.y + std::max(0, body.h / 2 - 1);
        int yTip = yHeadline + 1;
        if (yTip < body.y + body.h) {
            int xHeadline =
                body.x + std::max(0, (body.w - inkcell::text::display_width(headline)) / 2);
            int xTip = body.x + std::max(0, (body.w - inkcell::text::display_width(tip)) / 2);
            if (yHeadline >= body.y)
                surface.text({xHeadline, yHeadline},
                             inkcell::text::truncate(headline,
                                                     std::max(0, body.w - (xHeadline - body.x))),
                             theme::dim());
            surface.text({xTip, yTip},
                         inkcell::text::truncate(tip, std::max(0, body.w - (xTip - body.x))),
                         theme::dim());
        }
        return;
    }

    const auto& displayLines = *displayLinesPtr;
    std::vector<uint8_t> localKinds;
    std::vector<bool> localHeaders;
    std::vector<bool> localSelected;
    std::vector<uint8_t>* blockKinds = &localKinds;
    std::vector<bool>* blockHeaders = &localHeaders;
    std::vector<bool>* blockSelected = &localSelected;

    if (useViewportWindow && m.transcriptCache) {
        blockKinds = &m.transcriptCache->viewportKinds;
        blockHeaders = &m.transcriptCache->viewportHeaders;
        blockSelected = &m.transcriptCache->viewportSelected;
        // displayLines already IS the viewport window starting at `offset`.
        int visible = std::min(body.h, static_cast<int>(displayLines.size()));
        int firstY = body.y;
        int blockWidth = std::max(1, body.w - (total > body.h ? 1 : 0));
        for (int y = 0; y < visible; ++y) {
            const auto& line = displayLines[static_cast<size_t>(y)];
            ChatBlockKind kind =
                y < static_cast<int>(blockKinds->size())
                    ? static_cast<ChatBlockKind>((*blockKinds)[static_cast<size_t>(y)])
                    : ChatBlockKind::None;
            bool header =
                y < static_cast<int>(blockHeaders->size()) ? (*blockHeaders)[static_cast<size_t>(y)]
                                                           : false;
            bool selected = m.historyFocused && y < static_cast<int>(blockSelected->size()) &&
                            (*blockSelected)[static_cast<size_t>(y)];
            if (kind != ChatBlockKind::None) {
                auto style = blockStyle(kind, header, selected);
                surface.fill({body.x, firstY + y, blockWidth, 1}, " ", style);
                surface.text({body.x, firstY + y}, header ? "▎" : " ", style);
                surface.text({body.x + 1, firstY + y},
                             inkcell::text::fit_left(line, std::max(1, blockWidth - 1)), style);
            } else {
                surface.text({body.x, firstY + y}, inkcell::text::fit_left(line, body.w),
                             theme::text());
            }
        }
        int maxOffset = std::max(0, total - body.h);
        if (total > body.h && body.w > 4) {
            int thumb = std::max(1, body.h * body.h / std::max(1, total));
            int thumbY = (offset * std::max(1, body.h - thumb)) / std::max(1, maxOffset);
            for (int y = 0; y < body.h; ++y) {
                surface.put({body.right() - 1, body.y + y},
                            (y >= thumbY && y < thumbY + thumb) ? "│" : "┆", theme::dim());
            }
        }
        return;
    }

    // Non-virtualized: full displayLines vector (small transcripts / tests).
    if (m.transcriptCache) {
        if (m.transcriptCache->blockKinds.size() != displayLines.size())
            buildBlockMetadata(displayLines, m.transcriptCache->blockKinds,
                               m.transcriptCache->blockHeaders, m.transcriptCache->blockSelected,
                               m.agentName);
        blockKinds = &m.transcriptCache->blockKinds;
        blockHeaders = &m.transcriptCache->blockHeaders;
        blockSelected = &m.transcriptCache->blockSelected;
    } else {
        buildBlockMetadata(displayLines, localKinds, localHeaders, localSelected, m.agentName);
    }

    int maxOffset = std::max(0, total - body.h);
    offset = m.followBottom ? maxOffset : std::max(0, std::min(m.scrollOffset, maxOffset));
    if (m.historyFocused) {
        for (int i = 0; i < total; ++i) {
            if (displayLines[static_cast<size_t>(i)].rfind("› ", 0) == 0) {
                offset = std::max(0, std::min(maxOffset, i - body.h / 3));
                break;
            }
        }
    }
    int visible = std::min(body.h, total - offset);
    int firstY = body.y;
    for (int y = 0; y < visible; ++y) {
        int idx = offset + y;
        const auto& line = displayLines[static_cast<size_t>(idx)];
        ChatBlockKind kind = static_cast<ChatBlockKind>((*blockKinds)[static_cast<size_t>(idx)]);
        bool header = (*blockHeaders)[static_cast<size_t>(idx)];
        bool selected = m.historyFocused && (*blockSelected)[static_cast<size_t>(idx)];
        if (kind != ChatBlockKind::None) {
            auto style = blockStyle(kind, header, selected);
            int blockWidth = std::max(1, body.w - (total > body.h ? 1 : 0));
            surface.fill({body.x, firstY + y, blockWidth, 1}, " ", style);
            surface.text({body.x, firstY + y}, header ? "▎" : " ", style);
            surface.text({body.x + 1, firstY + y},
                         inkcell::text::fit_left(line, std::max(1, blockWidth - 1)), style);
        } else {
            surface.text({body.x, firstY + y}, inkcell::text::fit_left(line, body.w), theme::text());
        }
    }
    if (total > body.h && body.w > 4) {
        int thumb = std::max(1, body.h * body.h / total);
        int thumbY = (offset * std::max(1, body.h - thumb)) / std::max(1, maxOffset);
        for (int y = 0; y < body.h; ++y) {
            surface.put({body.right() - 1, body.y + y},
                        (y >= thumbY && y < thumbY + thumb) ? "│" : "┆", theme::dim());
        }
    }
}

inline void drawHelpOverlay(inkcell::Surface& surface, inkcell::Rect page,
                            const inkcell::CommandRegistry& reg) {
    // Data-driven help from inkcell CommandRegistry (dogfood F8).
    int width = std::max(48, std::min(page.w - 4, 78));
    int height = std::max(20, std::min(page.h - 2, 28));
    inkcell::Rect frame{page.x + (page.w - width) / 2, page.y + (page.h - height) / 2, width, height};
    surface.fill(frame, " ", theme::panel_2());
    surface.box(frame, inkcell::BorderStyle::Rounded, theme::cyan());
    surface.hline({frame.x + 1, frame.y + 1}, std::max(0, frame.w - 2), "─", theme::footer_accent_focus());

    int x = frame.x + 2;
    int y = frame.y + 2;
    int inner = frame.w - 4;
    const int keyCol = 14;

    auto catStyle = [&](const std::string& cat) -> inkcell::Style {
        if (cat == "CHAT" || cat == "PROMPT") return theme::green();
        if (cat == "NAV") return theme::amber();
        if (cat == "ACTION" || cat == "SLASH") return theme::cyan();
        if (cat == "SYSTEM") return theme::red();
        return theme::bright();
    };

    surface.text({x, y++}, "HELP", theme::bright());
    surface.text({x, y++},
                 inkcell::text::truncate(std::string("theme ") + theme::name() +
                                             "  ·  " + std::to_string(reg.size()) +
                                             " commands  ·  ? or Esc closes",
                                         inner),
                 theme::dim());

    for (const auto& cat : registryCategoryOrder(reg)) {
        if (y >= frame.bottom() - 3) break;
        if (y > frame.y + 3) ++y;
        surface.text({x, y++}, inkcell::text::truncate(cat, inner), catStyle(cat));
        for (const auto& cmd : reg.by_category(cat)) {
            if (y >= frame.bottom() - 2) break;
            if (!cmd.visible || !cmd.enabled) continue;
            std::string k = cmd.default_key.empty() ? "·" : cmd.default_key;
            while (inkcell::text::display_width(k) < keyCol) k.push_back(' ');
            surface.text({x, y}, inkcell::text::truncate(k, keyCol), catStyle(cat));
            std::string desc = cmd.title;
            if (!cmd.description.empty()) desc += "  ·  " + cmd.description;
            surface.text({x + keyCol, y},
                         inkcell::text::truncate(desc, std::max(0, inner - keyCol)), theme::text());
            ++y;
        }
    }

    surface.text({x, frame.bottom() - 2}, "?  Esc  close", theme::dim());
}

// Default: chat command registry (palette + slash inventory).
inline void drawHelpOverlay(inkcell::Surface& surface, inkcell::Rect page) {
    drawHelpOverlay(surface, page, chatCommandRegistry());
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
        if (y >= frame.bottom() - 6) break;
        surface.text({x, y++}, line, theme::text());
    }
    if (!card->help.empty() && y < frame.bottom() - 6)
        surface.text({x, y++}, inkcell::text::truncate(card->help, inner), theme::dim());

    if (card->type == "choice" || card->type == "multi_choice" || card->type == "ranker") {
        ++y;
        for (int i = 0; i < static_cast<int>(card->options.size()) && y < frame.bottom() - 4; ++i) {
            const auto& option = card->options[static_cast<size_t>(i)];
            bool selected = i == state.selectedOption;
            bool checked = multiSelected.count(i) > 0;
            std::string marker = selected ? "> " : "  ";
            if (card->type == "multi_choice") marker += checked ? "[x] " : "[ ] ";
            else if (card->type == "ranker") marker += std::to_string(i + 1) + ". ";
            std::string label = option.label;
            if (!option.description.empty()) label += "  — " + option.description;
            surface.text({x, y++}, inkcell::text::truncate(marker + label, inner),
                         option.disabled ? theme::dim() : selected ? theme::selected_style() : theme::text());
        }
        if (y < frame.bottom() - 2) {
            if (card->type == "multi_choice")
                surface.text({x, y++}, "Space toggle · Enter submit", theme::dim());
            else if (card->type == "choice")
                surface.text({x, y++}, "↑↓ / j k select · Enter confirm", theme::dim());
            else
                surface.text({x, y++}, "Enter accepts current order (or type 1,3,2…)", theme::dim());
        }
    } else if (card->type == "confirm") {
        ++y;
        surface.text({x, y++}, "[Y] yes    [N] no", theme::bright());
        surface.text({x, y++}, "single key — no Enter needed", theme::dim());
    } else if (card->type == "type_confirm") {
        ++y;
        surface.text({x, y++},
                     inkcell::text::truncate("type exactly: " + card->confirmWord, inner),
                     theme::amber());
        std::string shown = input;
        surface.text({x, y++}, inkcell::text::truncate("> " + shown + "█", inner), theme::bright());
    } else if (card->type == "note" || card->type == "info" || card->type == "section_header") {
        ++y;
        surface.text({x, y++}, "(auto) non-interactive card", theme::dim());
    } else {
        ++y;
        if (!card->defaultValue.empty() && input.empty() && y < frame.bottom() - 3)
            surface.text({x, y++},
                         inkcell::text::truncate("default: " + card->defaultValue + "  (Enter accepts)",
                                                 inner),
                         theme::dim());
        if (card->type == "number" && (card->hasNumberMin || card->hasNumberMax) &&
            y < frame.bottom() - 3) {
            std::string bounds = "range";
            if (card->hasNumberMin) bounds += " ≥" + std::to_string(card->numberMin);
            if (card->hasNumberMax) bounds += " ≤" + std::to_string(card->numberMax);
            surface.text({x, y++}, bounds, theme::dim());
        }
        std::string shown = card->type == "secret" ? std::string(input.size(), '*') : input;
        surface.text({x, y++}, inkcell::text::truncate("> " + shown + "█", inner), theme::bright());
    }

    if (!state.error.empty())
        surface.text({x, frame.bottom() - 3}, inkcell::text::truncate("error: " + state.error, inner), theme::red());
    std::string hint = (card->type == "choice") ? "↑↓/j/k select · Enter choose · Esc cancel"
                       : (card->type == "multi_choice") ? "↑↓ select · Space toggle · Enter done · Esc cancel"
                       : (card->type == "confirm") ? "y/n answer · Esc cancel"
                       : "Enter submit · Esc cancel";
    surface.text({x, frame.bottom() - 2}, inkcell::text::truncate(hint, inner), theme::dim());
}

// Paint a readline-style completion listing just above the status/prompt chrome.
// Multi-column when it fits; wraps to extra rows. Never touches the transcript.
inline int completionMenuHeight(const ChatSurfaceModel& m, int width) {
    if (m.completionMenu.empty() || width <= 0) return 0;
    // Cap at 4 rows so a huge catalog does not eat the whole screen.
    const int maxRows = 4;
    int colW = 1;
    for (const auto& s : m.completionMenu)
        colW = std::max(colW, inkcell::text::display_width(s) + 2);
    colW = std::min(colW, std::max(8, width));
    int cols = std::max(1, width / colW);
    int rows = static_cast<int>((m.completionMenu.size() + static_cast<size_t>(cols) - 1) /
                               static_cast<size_t>(cols));
    return std::min(maxRows, std::max(1, rows));
}

inline void drawCompletionMenu(inkcell::Surface& surface, inkcell::Rect area,
                               const ChatSurfaceModel& m) {
    if (m.completionMenu.empty() || area.h <= 0 || area.w <= 0) return;
    int colW = 1;
    for (const auto& s : m.completionMenu)
        colW = std::max(colW, inkcell::text::display_width(s) + 2);
    colW = std::min(colW, std::max(8, area.w));
    int cols = std::max(1, area.w / colW);
    int maxItems = cols * area.h;
    for (int i = 0; i < static_cast<int>(m.completionMenu.size()) && i < maxItems; ++i) {
        int row = i / cols;
        int col = i % cols;
        int x = area.x + col * colW;
        bool selected = (i == m.completionSelected);
        std::string cell = m.completionMenu[static_cast<size_t>(i)];
        if (selected) cell = cell;  // style carries selection
        surface.text({x, area.y + row},
                     inkcell::text::truncate(cell, colW - 1),
                     selected ? theme::selected_style() : theme::dim());
    }
    if (static_cast<int>(m.completionMenu.size()) > maxItems) {
        // Last cell slot: overflow marker
        int i = maxItems - 1;
        int row = i / cols;
        int col = i % cols;
        surface.text({area.x + col * colW, area.y + row}, "…",
                     theme::dim());
    }
}

inline void drawChatSurface(inkcell::Surface& surface, inkcell::Rect frame, const ChatSurfaceModel& m) {
    surface.clear(theme::base_bg());
    if (frame.w <= 0 || frame.h <= 0) return;
    // One flat page, no nested boxes. Leave the app-level page inset to caller.
    drawHeader(surface, frame, m);
    const int promptH = std::max(1, promptBoxHeight(m, frame.w));
    const int promptY = frame.bottom() - promptH;
    const int statusY = promptY - 1;
    int menuH = completionMenuHeight(m, frame.w);
    int menuY = statusY - menuH;
    // No separator rule — elevated footer is the visual break.
    // Alerts fold into the status line (right) — no reserved strip row.
    inkcell::Rect body{frame.x, frame.y + 2, frame.w, std::max(1, menuY - (frame.y + 2))};
    drawTranscript(surface, body, m);
    if (menuH > 0)
        drawCompletionMenu(surface, {frame.x, menuY, frame.w, menuH}, m);
    drawStatusLine(surface, {frame.x, statusY, frame.w, 1}, m);
    drawPromptBox(surface, {frame.x, promptY, frame.w, promptH}, m);
}

}  // namespace cortex::mk3::ui::chat
