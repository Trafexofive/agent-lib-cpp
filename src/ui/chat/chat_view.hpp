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
#include "src/ui/chat/ask_dialog_model.hpp"
#include "src/ui/chat/chat_blocks.hpp"
#include "src/ui/chat/transcript_cache.hpp"
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
    std::string hint;
    std::string agentName;  // real agent display name for the assistant label (replaces CORTEX)
    std::string scopeName;  // drilled-in subagent name (empty at root) for header/status scope indicator
    // Transient readline-style completion menu (NOT transcript history).
    // Drawn between the body separator and the status line; cleared when the
    // operator types, submits, or leaves the stem.
    std::vector<std::string> completionMenu;
    int completionSelected = -1;  // index highlighted while cycling; -1 = none
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

    // Left accent tick (reads as elevation / focus without a full box).
    auto accent = m.running   ? theme::footer_accent_live()
                  : m.failed  ? theme::footer_warn()
                  : focusBar  ? theme::footer_accent_focus()
                              : theme::footer_accent_idle();
    surface.text({row.x, row.y}, "▌", accent);

    auto stateStyle = m.failed ? theme::footer_warn()
                      : m.running ? theme::footer_live()
                                  : theme::footer_dim();
    std::string glyph = m.running ? liveSpinner(m.nowMs)
                        : m.failed ? "✗"
                                   : "○";
    std::string state = m.status.empty() || m.status == "idle" ? "ready" : m.status;
    // Drop noisy "cancelling (...)" tails in the tight left cluster.
    if (state.rfind("cancelling", 0) == 0) state = "cancelling";

    // Build one dense left cluster so narrow terminals still show live signal.
    // Prefer: spinner · state · elapsed · scope · pend/act/res/bytes
    std::string left = glyph + " " + state;
    int64_t elapsed = m.running ? m.turnElapsedMs : m.lastTurnElapsedMs;
    if (m.running || elapsed > 0)
        left += " " + fmtCompactElapsed(elapsed);
    if (!m.scopeName.empty())
        left += " ◀" + m.scopeName;
    if (m.pendingOps > 0) left += " pend" + std::to_string(m.pendingOps);
    if (m.actionCount > 0) left += " act" + std::to_string(m.actionCount);
    if (m.resultCount > 0) left += " res" + std::to_string(m.resultCount);
    if (m.tokenBytes > 0) left += " " + fmtCompactBytes(m.tokenBytes);

    // Right: mode pills + theme only. Keybind prose lives in help (?).
    std::string right = m.mode + " · " + theme::name();

    int x = row.x + 2;
    int rightW = inkcell::text::display_width(right);
    int avail = std::max(0, row.w - 2 - rightW - 1);
    // Prefer metrics over long status prose when squeezed.
    if (inkcell::text::display_width(left) > avail && m.running) {
        left = glyph;
        if (elapsed > 0) left += " " + fmtCompactElapsed(elapsed);
        if (m.pendingOps > 0) left += " pend" + std::to_string(m.pendingOps);
        if (m.actionCount > 0) left += " act" + std::to_string(m.actionCount);
        if (m.resultCount > 0) left += " res" + std::to_string(m.resultCount);
        if (m.tokenBytes > 0) left += " " + fmtCompactBytes(m.tokenBytes);
    }
    surface.text({x, row.y}, inkcell::text::truncate(left, avail), stateStyle);
    surface.text({std::max(row.x, row.right() - rightW), row.y},
                 inkcell::text::truncate(right, row.w), theme::footer_dim());
}

inline void drawPromptLine(inkcell::Surface& surface, inkcell::Rect row, const ChatSurfaceModel& m) {
    // Elevated prompt surface. Input always wins over any status copy.
    const bool focusBar = m.inputFocused;
    auto bg = focusBar ? theme::footer_bg_focus() : theme::footer_bg();
    surface.fill(row, " ", bg);

    auto accent = m.running  ? theme::footer_accent_live()
                  : focusBar ? theme::footer_accent_focus()
                             : theme::footer_accent_idle();
    surface.text({row.x, row.y}, "▌", accent);

    auto textSt = focusBar ? theme::footer_bright() : theme::footer_dim();
    const std::string glyph = focusBar ? "› " : "  ";

    std::string body = m.input;
    const bool showCursor = cursorVisible(m.nowMs, focusBar);
    if (focusBar) {
        int cursor = std::max(0, std::min(m.inputCursor, static_cast<int>(body.size())));
        if (showCursor)
            body.insert(static_cast<size_t>(cursor), "█");
        else if (cursor >= static_cast<int>(body.size()))
            body.push_back(' ');  // keep layout stable on blink-off at EOL
        else
            body.insert(static_cast<size_t>(cursor), " ");
    }

    // Keep the cursor end in view for long input (simple tail window).
    int maxW = std::max(1, row.w - 4);
    std::string shown = glyph + body;
    if (inkcell::text::display_width(shown) > maxW) {
        // Drop from the left of body until it fits, then mark with ellipsis.
        std::string tail = body;
        while (!tail.empty() && inkcell::text::display_width(glyph + "…" + tail) > maxW) {
            size_t len = 1;
            while (len < tail.size() &&
                   (static_cast<unsigned char>(tail[len]) & 0xc0) == 0x80)
                ++len;
            tail.erase(0, len);
        }
        shown = glyph + "…" + tail;
    }

    surface.text({row.x + 2, row.y}, inkcell::text::truncate(shown, maxW), textSt);
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

inline void drawTranscript(inkcell::Surface& surface, inkcell::Rect body, const ChatSurfaceModel& m) {
    if (body.w <= 0 || body.h <= 0) return;
    const auto& source = m.transcriptSource ? *m.transcriptSource : m.transcript;
    int wrapWidth = std::max(1, body.w - 1);
    std::vector<std::string> uncachedLines;
    const std::vector<std::string>* displayLinesPtr = nullptr;
    if (m.transcriptCache) {
        auto& cache = *m.transcriptCache;
        if (cache.sourceVersion != m.transcriptVersion || cache.width != wrapWidth) {
            const bool sameWidth = cache.width == wrapWidth;
            if (!sameWidth || cache.sourceSnapshot.empty()) {
                // Full rewrap: new width or first run.
                cache.lines.clear();
                cache.sourceLineSpans.clear();
                cache.inCodeAfter.clear();
                wrapTranscriptRange(source, 0, source.size(), wrapWidth, false,
                                    cache.lines, cache.sourceLineSpans, cache.inCodeAfter,
                                    m.agentName);
                cache.sourceSnapshot = source;
            } else {
                // Incremental: keep the stable display prefix, re-wrap only the dirty tail.
                // Find the first source line that differs from the snapshot.
                size_t d = 0;
                const auto& snap = cache.sourceSnapshot;
                while (d < source.size() && d < snap.size() && source[d] == snap[d]) ++d;
                int stableEnd = 0;
                for (size_t i = 0; i < d; ++i) stableEnd += cache.sourceLineSpans[i];
                cache.lines.resize(static_cast<size_t>(stableEnd));
                cache.sourceLineSpans.resize(d);
                cache.inCodeAfter.resize(d);
                bool inCode = d > 0 ? cache.inCodeAfter[d - 1] : false;
                wrapTranscriptRange(source, d, source.size(), wrapWidth, inCode,
                                    cache.lines, cache.sourceLineSpans, cache.inCodeAfter,
                                    m.agentName);
                cache.sourceSnapshot = source;
            }
            // Block metadata rebuilds on size mismatch (checked below). Clearing here
            // forces a fresh pass — cheap (no allocs) and avoids stale tail metadata
            // when the dirty tail produces the same display-line count.
            cache.blockKinds.clear();
            cache.blockHeaders.clear();
            cache.blockSelected.clear();
            cache.sourceVersion = m.transcriptVersion;
            cache.width = wrapWidth;
        }
        displayLinesPtr = &cache.lines;
    } else {
        uncachedLines = wrapTranscript(source, wrapWidth, m.agentName);
        displayLinesPtr = &uncachedLines;
    }
    const auto& displayLines = *displayLinesPtr;
    int total = static_cast<int>(displayLines.size());
    if (total <= 0) {
        // First-run empty state: the body would otherwise be a void between
        // the header and the status line. Show a centered dim headline + a
        // tip so a fresh operator knows what this surface is and how to
        // start. Hidden once any content exists.
        const std::string headline = "No conversation yet";
        const std::string tip = "Type a prompt below and press Enter \xe2\x80\x94 or press ? for help";
        int yHeadline = body.y + std::max(0, body.h / 2 - 1);
        int yTip = yHeadline + 1;
        if (yTip < body.y + body.h) {
            int xHeadline = body.x + std::max(0, (body.w - inkcell::text::display_width(headline)) / 2);
            int xTip = body.x + std::max(0, (body.w - inkcell::text::display_width(tip)) / 2);
            if (yHeadline >= body.y)
                surface.text({xHeadline, yHeadline},
                             inkcell::text::truncate(headline, std::max(0, body.w - (xHeadline - body.x))),
                             theme::dim());
            surface.text({xTip, yTip},
                         inkcell::text::truncate(tip, std::max(0, body.w - (xTip - body.x))),
                         theme::dim());
        }
        return;
    }
    std::vector<uint8_t> localKinds;
    std::vector<bool> localHeaders;
    std::vector<bool> localSelected;
    std::vector<uint8_t>* blockKinds = &localKinds;
    std::vector<bool>* blockHeaders = &localHeaders;
    std::vector<bool>* blockSelected = &localSelected;
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
    int offset = m.followBottom ? maxOffset : std::max(0, std::min(m.scrollOffset, maxOffset));
    if (m.historyFocused) {
        for (int i = 0; i < total; ++i) {
            if (displayLines[static_cast<size_t>(i)].rfind("› ", 0) == 0) {
                offset = std::max(0, std::min(maxOffset, i - body.h / 3));
                break;
            }
        }
    }
    int visible = std::min(body.h, total - offset);
    // Top-anchor the visible window within the body: short transcripts start
    // right below the header and grow downward (contiguous with the header, no
    // floating void); once the transcript overflows, offset = maxOffset keeps the
    // newest lines at the bottom of the body (stick-to-bottom). Standard chat
    // pattern (Slack/Discord): messages start at the top, stick when full.
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
            surface.put({body.right() - 1, body.y + y}, (y >= thumbY && y < thumbY + thumb) ? "│" : "┆", theme::dim());
        }
    }
}

inline void drawHelpOverlay(inkcell::Surface& surface, inkcell::Rect page) {
    // Sectioned, color-keyed help. Keys colored by domain; actions stay quiet.
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

    auto section = [&](const char* title, inkcell::Style st) {
        if (y >= frame.bottom() - 3) return;
        if (y > frame.y + 2) ++y;
        surface.text({x, y++}, inkcell::text::truncate(title, inner), st);
    };
    auto row = [&](const char* key, const char* desc, inkcell::Style keySt) {
        if (y >= frame.bottom() - 2) return;
        std::string k = key;
        while (inkcell::text::display_width(k) < keyCol) k.push_back(' ');
        surface.text({x, y}, inkcell::text::truncate(k, keyCol), keySt);
        surface.text({x + keyCol, y},
                     inkcell::text::truncate(desc, std::max(0, inner - keyCol)), theme::text());
        ++y;
    };

    surface.text({x, y++}, "HELP", theme::bright());
    surface.text({x, y++},
                 inkcell::text::truncate(std::string("theme ") + theme::name() + "  ·  ? or Esc closes",
                                         inner),
                 theme::dim());

    section("COMPOSE", theme::green());
    row("Enter", "send prompt", theme::green());
    row("↑ / ↓", "prompt history", theme::green());
    row("Tab", "slash complete (LCP → cycle)", theme::green());
    row("Shift-Tab", "cycle completions backward", theme::green());
    row("/…", "slash commands  ·  /help catalog", theme::green());

    section("NAVIGATE", theme::amber());
    row("Esc", "transcript ↔ composer  ·  back nested", theme::amber());
    row("PgUp/PgDn", "scroll transcript", theme::amber());
    row("Home/End", "jump top / bottom", theme::amber());
    row("j / k", "select blocks (history focus)", theme::amber());
    row("Enter", "open selected sub-agent", theme::amber());
    row("i", "focus composer", theme::amber());

    section("VIEW", theme::cyan());
    row("Ctrl-T", "toggle thoughts", theme::cyan());
    row("Ctrl-O", "toggle body truncation", theme::cyan());
    row("Ctrl-R", "toggle raw stream", theme::cyan());
    row("t / r", "thoughts / raw (history focus)", theme::cyan());
    row("T", "theme graphite ↔ neon", theme::cyan());

    section("CONTROL", theme::red());
    row("Ctrl-X", "stop agent loop", theme::red());
    row("Ctrl-C", "stop if running  ·  quit if idle", theme::red());
    row("q", "quit (outside composer)", theme::red());

    surface.text({x, frame.bottom() - 2}, "?  Esc  close", theme::dim());
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
    int promptY = frame.bottom() - 1;
    int statusY = frame.bottom() - 2;
    int menuH = completionMenuHeight(m, frame.w);
    int menuY = statusY - menuH;
    // No separator rule — elevated footer is the visual break.
    inkcell::Rect body{frame.x, frame.y + 2, frame.w, std::max(1, menuY - (frame.y + 2))};
    drawTranscript(surface, body, m);
    if (menuH > 0)
        drawCompletionMenu(surface, {frame.x, menuY, frame.w, menuH}, m);
    drawStatusLine(surface, {frame.x, statusY, frame.w, 1}, m);
    drawPromptLine(surface, {frame.x, promptY, frame.w, 1}, m);
}

}  // namespace cortex::mk3::ui::chat
