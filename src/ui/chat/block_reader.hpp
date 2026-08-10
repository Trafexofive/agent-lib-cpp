#pragma once
// Full-block reader overlay — Enter on a timeline text block opens this.
// Lightweight markdown-aware paint + vim-ish cursor / visual selection.
//   j/k ↑↓     move cursor (scroll follows)
//   v          visual (char-wise within lines, multi-line as line-span)
//   V          visual-line
//   y          yank selection (or current line / whole body)
//   /          search forward (type query, Enter)
//   n / N      next / prev match
//   Esc        exit visual/search, or close reader
//   Backspace  close reader

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::chat {

enum class ReaderVisual : uint8_t { None = 0, Char = 1, Line = 2 };

struct BlockReaderState {
    bool open = false;
    std::string title;
    std::string body;
    int scroll = 0;
    int cursor = 0;       // line index into lines[]
    int cursorCol = 0;    // display column within line (char visual)
    bool markdown = false;
    ReaderVisual visual = ReaderVisual::None;
    int selAnchor = 0;    // line
    int selAnchorCol = 0;
    // Search (/ n N)
    bool searchMode = false;
    std::string searchQuery;
    std::string lastSearch;
    int searchHit = -1;
    // Cached wrap for current width.
    int wrapW = -1;
    std::vector<std::string> lines;
    std::vector<uint8_t> lineKind;  // 0 normal, 1 header, 2 code, 3 bullet, 4 dim
};

inline void readerEnsureCursorVisible(BlockReaderState& r, int viewH);  // fwd

inline bool readerLineHas(const std::string& line, const std::string& q) {
    if (q.empty()) return false;
    // case-insensitive substring
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    return lower(line).find(lower(q)) != std::string::npos;
}

// dir +1 forward, -1 backward. Returns true if landed on a hit.
inline bool readerSearchStep(BlockReaderState& r, int dir, int viewH) {
    if (r.lines.empty()) return false;
    const std::string& q = r.lastSearch.empty() ? r.searchQuery : r.lastSearch;
    if (q.empty()) return false;
    const int n = static_cast<int>(r.lines.size());
    int start = r.cursor;
    for (int step = 1; step <= n; ++step) {
        int i = (start + dir * step) % n;
        if (i < 0) i += n;
        if (readerLineHas(r.lines[static_cast<size_t>(i)], q)) {
            r.cursor = i;
            r.cursorCol = 0;
            r.searchHit = i;
            readerEnsureCursorVisible(r, viewH);
            return true;
        }
    }
    return false;
}

inline bool looksLikeMarkdown(const std::string& body) {
    if (body.size() < 8) return false;
    int hits = 0;
    if (body.find("\n#") != std::string::npos || body.rfind("# ", 0) == 0) ++hits;
    if (body.find("```") != std::string::npos) ++hits;
    if (body.find("**") != std::string::npos) ++hits;
    if (body.find("\n- ") != std::string::npos || body.find("\n* ") != std::string::npos) ++hits;
    if (body.find("`") != std::string::npos) ++hits;
    if (body.find("\n> ") != std::string::npos) ++hits;
    return hits >= 2;
}

inline void openBlockReader(BlockReaderState& r, std::string title, std::string body) {
    r = BlockReaderState{};
    r.open = true;
    r.title = std::move(title);
    r.body = std::move(body);
    r.markdown = looksLikeMarkdown(r.body);
}

inline void closeBlockReader(BlockReaderState& r) { r = BlockReaderState{}; }

inline void rebuildBlockReaderLines(BlockReaderState& r, int width) {
    width = std::max(20, width);
    if (r.wrapW == width && !r.lines.empty()) return;
    r.wrapW = width;
    r.lines.clear();
    r.lineKind.clear();

    bool inFence = false;
    size_t start = 0;
    const std::string& body = r.body;
    auto pushWrapped = [&](const std::string& raw, uint8_t kind) {
        if (raw.empty()) {
            r.lines.push_back("");
            r.lineKind.push_back(kind);
            return;
        }
        std::string cur;
        int used = 0;
        for (size_t i = 0; i < raw.size();) {
            size_t len = 1;
            unsigned char c = static_cast<unsigned char>(raw[i]);
            if ((c & 0x80) == 0) len = 1;
            else if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            if (i + len > raw.size()) len = 1;
            std::string g = raw.substr(i, len);
            int gw = inkcell::text::display_width(g);
            if (gw <= 0) gw = (g == " " ? 1 : 0);
            if (gw == 0) {
                // zero-width: keep on current line, don't advance columns
                cur += g;
                i += len;
                continue;
            }
            if (used + gw > width && !cur.empty()) {
                r.lines.push_back(cur);
                r.lineKind.push_back(kind);
                cur.clear();
                used = 0;
            }
            cur += g;
            used += gw;
            i += len;
        }
        if (!cur.empty() || raw.empty()) {
            r.lines.push_back(cur);
            r.lineKind.push_back(kind);
        }
    };

    while (start <= body.size()) {
        size_t end = body.find('\n', start);
        if (end == std::string::npos) end = body.size();
        std::string line = body.substr(start, end - start);
        if (!r.markdown) {
            pushWrapped(line, 0);
        } else if (line.rfind("```", 0) == 0) {
            inFence = !inFence;
            pushWrapped(line, 2);
        } else if (inFence) {
            pushWrapped(line, 2);
        } else if (line.rfind("### ", 0) == 0 || line.rfind("## ", 0) == 0 ||
                   line.rfind("# ", 0) == 0) {
            pushWrapped(line, 1);
        } else if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0 ||
                   (line.size() > 2 && std::isdigit(static_cast<unsigned char>(line[0])) &&
                    line.find(". ") != std::string::npos && line.find(". ") < 4)) {
            pushWrapped(line, 3);
        } else if (line.rfind("> ", 0) == 0) {
            pushWrapped(line, 4);
        } else {
            pushWrapped(line, 0);
        }
        if (end == body.size()) break;
        start = end + 1;
    }
    if (r.lines.empty()) {
        r.lines.push_back("(empty)");
        r.lineKind.push_back(4);
    }
    // Clamp cursor after rebuild.
    r.cursor = std::max(0, std::min(r.cursor, static_cast<int>(r.lines.size()) - 1));
}

inline void readerEnsureCursorVisible(BlockReaderState& r, int viewH) {
    viewH = std::max(1, viewH);
    if (r.cursor < r.scroll) r.scroll = r.cursor;
    if (r.cursor >= r.scroll + viewH) r.scroll = r.cursor - viewH + 1;
    int maxOff = std::max(0, static_cast<int>(r.lines.size()) - viewH);
    r.scroll = std::max(0, std::min(maxOff, r.scroll));
}

inline void moveReaderCursor(BlockReaderState& r, int dLine, int dCol, int viewH) {
    if (!r.open || r.lines.empty()) return;
    r.cursor = std::max(0, std::min(static_cast<int>(r.lines.size()) - 1, r.cursor + dLine));
    const std::string& line = r.lines[static_cast<size_t>(r.cursor)];
    int maxCol = std::max(0, inkcell::text::display_width(line));
    r.cursorCol = std::max(0, std::min(maxCol, r.cursorCol + dCol));
    // When moving lines, clamp col to new line length.
    if (dLine != 0) r.cursorCol = std::min(r.cursorCol, maxCol);
    readerEnsureCursorVisible(r, viewH);
}

// Byte offset of the glyph that occupies display column `col` on `line`.
// Returns line.size() if col is past the end.
inline size_t readerByteAtCol(const std::string& line, int col) {
    if (col <= 0) return 0;
    int used = 0;
    size_t i = 0;
    while (i < line.size()) {
        size_t len = 1;
        unsigned char c = static_cast<unsigned char>(line[i]);
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > line.size()) len = 1;
        int gw = inkcell::text::display_width(line.substr(i, len));
        if (gw <= 0) gw = (len == 1 && line[i] == ' ') ? 1 : 0;
        if (gw == 0) {
            i += len;
            continue;
        }
        if (used + gw > col) return i;
        used += gw;
        i += len;
        if (used >= col) return i;
    }
    return line.size();
}

inline int readerColAtByte(const std::string& line, size_t byte) {
    int used = 0;
    size_t i = 0;
    while (i < line.size() && i < byte) {
        size_t len = 1;
        unsigned char c = static_cast<unsigned char>(line[i]);
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > line.size()) len = 1;
        int gw = inkcell::text::display_width(line.substr(i, len));
        if (gw <= 0) gw = (len == 1 && line[i] == ' ') ? 1 : 0;
        used += std::max(0, gw);
        i += len;
    }
    return used;
}

inline bool readerIsWordChar(unsigned char c) {
    return std::isalnum(c) || c == '_' || c > 127;
}

// Vim-ish w: forward to start of next word (crosses lines).
inline void readerWordForward(BlockReaderState& r, int viewH) {
    if (!r.open || r.lines.empty()) return;
    const std::string& line = r.lines[static_cast<size_t>(r.cursor)];
    size_t i = readerByteAtCol(line, r.cursorCol);
    auto advanceFrom = [&](int lineIdx, size_t bi) -> bool {
        const std::string& ln = r.lines[static_cast<size_t>(lineIdx)];
        // Skip current word chars.
        while (bi < ln.size() && readerIsWordChar(static_cast<unsigned char>(ln[bi]))) {
            size_t len = 1;
            unsigned char c = static_cast<unsigned char>(ln[bi]);
            if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            bi += len;
        }
        // Skip non-word (spaces/punct).
        while (bi < ln.size() && !readerIsWordChar(static_cast<unsigned char>(ln[bi]))) {
            size_t len = 1;
            unsigned char c = static_cast<unsigned char>(ln[bi]);
            if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            bi += len;
        }
        if (bi < ln.size()) {
            r.cursor = lineIdx;
            r.cursorCol = readerColAtByte(ln, bi);
            return true;
        }
        return false;
    };
    if (!advanceFrom(r.cursor, i)) {
        // Next lines: first word start.
        for (int li = r.cursor + 1; li < static_cast<int>(r.lines.size()); ++li) {
            const std::string& ln = r.lines[static_cast<size_t>(li)];
            size_t bi = 0;
            while (bi < ln.size() && !readerIsWordChar(static_cast<unsigned char>(ln[bi]))) {
                size_t len = 1;
                unsigned char c = static_cast<unsigned char>(ln[bi]);
                if ((c & 0xE0) == 0xC0) len = 2;
                else if ((c & 0xF0) == 0xE0) len = 3;
                else if ((c & 0xF8) == 0xF0) len = 4;
                bi += len;
            }
            if (bi < ln.size() || ln.empty()) {
                r.cursor = li;
                r.cursorCol = readerColAtByte(ln, bi);
                break;
            }
        }
    }
    readerEnsureCursorVisible(r, viewH);
}

// Vim-ish b: back to start of word.
inline void readerWordBack(BlockReaderState& r, int viewH) {
    if (!r.open || r.lines.empty()) return;
    auto goLineEnd = [&](int li) {
        const std::string& ln = r.lines[static_cast<size_t>(li)];
        r.cursor = li;
        r.cursorCol = std::max(0, inkcell::text::display_width(ln));
    };
    const std::string& line = r.lines[static_cast<size_t>(r.cursor)];
    size_t i = readerByteAtCol(line, r.cursorCol);
    if (i == 0) {
        if (r.cursor > 0) {
            goLineEnd(r.cursor - 1);
            // fall through to find word start on previous line from end
        } else {
            r.cursorCol = 0;
            readerEnsureCursorVisible(r, viewH);
            return;
        }
    }
    {
        const std::string& ln = r.lines[static_cast<size_t>(r.cursor)];
        size_t bi = readerByteAtCol(ln, r.cursorCol);
        if (bi > 0) --bi;
        // Skip non-word backward.
        while (bi > 0 && !readerIsWordChar(static_cast<unsigned char>(ln[bi]))) --bi;
        // Skip word chars backward to start.
        while (bi > 0 && readerIsWordChar(static_cast<unsigned char>(ln[bi - 1]))) --bi;
        r.cursorCol = readerColAtByte(ln, bi);
    }
    readerEnsureCursorVisible(r, viewH);
}

// Vim-ish e: end of word.
inline void readerWordEnd(BlockReaderState& r, int viewH) {
    if (!r.open || r.lines.empty()) return;
    const std::string& line = r.lines[static_cast<size_t>(r.cursor)];
    size_t i = readerByteAtCol(line, r.cursorCol);
    // If on a word char, move past it first when already mid-word at end?
    // Standard e: if not on word, skip non-word then go to end of next word.
    auto endOfWordFrom = [&](int lineIdx, size_t bi) -> bool {
        const std::string& ln = r.lines[static_cast<size_t>(lineIdx)];
        if (bi < ln.size() && readerIsWordChar(static_cast<unsigned char>(ln[bi]))) {
            // advance to last char of this word
            while (bi + 1 < ln.size() &&
                   readerIsWordChar(static_cast<unsigned char>(ln[bi + 1])))
                ++bi;
            r.cursor = lineIdx;
            r.cursorCol = readerColAtByte(ln, bi);
            return true;
        }
        // skip non-word
        while (bi < ln.size() && !readerIsWordChar(static_cast<unsigned char>(ln[bi]))) {
            size_t len = 1;
            unsigned char c = static_cast<unsigned char>(ln[bi]);
            if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            bi += len;
        }
        if (bi >= ln.size()) return false;
        while (bi + 1 < ln.size() && readerIsWordChar(static_cast<unsigned char>(ln[bi + 1])))
            ++bi;
        r.cursor = lineIdx;
        r.cursorCol = readerColAtByte(ln, bi);
        return true;
    };
    // If already at end of a word, step one forward first (vim e behavior).
    if (i < line.size() && readerIsWordChar(static_cast<unsigned char>(line[i]))) {
        bool atEnd = (i + 1 >= line.size()) ||
                     !readerIsWordChar(static_cast<unsigned char>(line[i + 1]));
        if (atEnd) {
            size_t len = 1;
            unsigned char c = static_cast<unsigned char>(line[i]);
            if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            i += len;
        }
    }
    if (!endOfWordFrom(r.cursor, i)) {
        for (int li = r.cursor + 1; li < static_cast<int>(r.lines.size()); ++li) {
            if (endOfWordFrom(li, 0)) break;
        }
    }
    readerEnsureCursorVisible(r, viewH);
}

inline void scrollBlockReader(BlockReaderState& r, int delta, int viewH) {
    // Prefer cursor motion (scroll follows). Keep for PgUp bulk jumps.
    moveReaderCursor(r, delta, 0, viewH);
}

inline bool readerLineSelected(const BlockReaderState& r, int lineIdx) {
    if (r.visual == ReaderVisual::None) return false;
    int a = std::min(r.selAnchor, r.cursor);
    int b = std::max(r.selAnchor, r.cursor);
    return lineIdx >= a && lineIdx <= b;
}

inline std::string readerYankText(const BlockReaderState& r) {
    if (r.lines.empty()) return r.body;
    if (r.visual == ReaderVisual::None) {
        // Current line.
        return r.lines[static_cast<size_t>(std::max(0, r.cursor))];
    }
    int a = std::min(r.selAnchor, r.cursor);
    int b = std::max(r.selAnchor, r.cursor);
    if (r.visual == ReaderVisual::Line) {
        std::string out;
        for (int i = a; i <= b && i < static_cast<int>(r.lines.size()); ++i) {
            if (!out.empty()) out += '\n';
            out += r.lines[static_cast<size_t>(i)];
        }
        return out;
    }
    // Char visual: multi-line = full lines in between; first/last are full lines
    // for simplicity (column-precise yank is rare in a reader; good enough).
    std::string out;
    for (int i = a; i <= b && i < static_cast<int>(r.lines.size()); ++i) {
        if (!out.empty()) out += '\n';
        out += r.lines[static_cast<size_t>(i)];
    }
    return out;
}

inline void drawBlockReader(inkcell::Surface& surface, inkcell::Rect page,
                            BlockReaderState& r) {
    if (!r.open || page.w < 10 || page.h < 6) return;

    surface.fill(page, " ", theme::panel_bg());

    inkcell::Rect frame{page.x + 1, page.y, std::max(8, page.w - 2), page.h};
    surface.fill(frame, " ", theme::panel_2());

    auto titleSt = theme::bright();
    titleSt.bold = true;
    titleSt.bg = theme::panel_3().bg;
    surface.fill({frame.x, frame.y, frame.w, 1}, " ", theme::panel_3());
    std::string modeTag;
    if (r.searchMode) modeTag = "  ·  /" + r.searchQuery + "█";
    else if (!r.lastSearch.empty() && r.searchHit >= 0) modeTag = "  ·  n:" + r.lastSearch;
    else if (r.visual == ReaderVisual::Char) modeTag = "  ·  VISUAL";
    else if (r.visual == ReaderVisual::Line) modeTag = "  ·  V-LINE";
    std::string head = "▌ " + (r.title.empty() ? std::string("block") : r.title);
    if (r.markdown) head += "  ·  md";
    head += modeTag;
    surface.text({frame.x, frame.y},
                 inkcell::text::truncate(head, frame.w - 1), titleSt);

    int footY = frame.bottom() - 1;
    surface.fill({frame.x, footY, frame.w, 1}, " ", theme::panel_3());
    surface.text(
        {frame.x + 1, footY},
        inkcell::text::truncate(
            "hjkl  w/b/e word  ·  v/V visual  ·  y yank  ·  Esc back  ·  BS close",
            frame.w - 2),
        theme::italic_dim());

    inkcell::Rect body{frame.x + 1, frame.y + 1, std::max(1, frame.w - 2),
                       std::max(1, frame.h - 2)};
    rebuildBlockReaderLines(r, body.w);
    readerEnsureCursorVisible(r, body.h);

    int visible = std::min(body.h, static_cast<int>(r.lines.size()) - r.scroll);
    for (int y = 0; y < visible; ++y) {
        int lineIdx = r.scroll + y;
        size_t idx = static_cast<size_t>(lineIdx);
        uint8_t kind = idx < r.lineKind.size() ? r.lineKind[idx] : 0;
        const bool onCursor = (lineIdx == r.cursor);
        const bool inSel = readerLineSelected(r, lineIdx);

        inkcell::Style st = theme::text();
        st.bg = theme::panel_2().bg;
        if (kind == 1) {
            st = theme::bright();
            st.bg = theme::panel_2().bg;
            st.bold = true;
        } else if (kind == 2) {
            st = theme::dim();
            st.bg = theme::panel_bg().bg;
        } else if (kind == 3) {
            st = theme::cyan();
            st.bg = theme::panel_2().bg;
        } else if (kind == 4) {
            st = theme::italic_dim();
            st.bg = theme::panel_2().bg;
        }

        if (inSel) {
            st.bg = theme::color(inkcell::Color::rgb(50, 58, 72), inkcell::Color::rgb(20, 55, 48));
            st.bold = onCursor || kind == 1;
        } else if (onCursor) {
            st.bg = theme::color(inkcell::Color::rgb(40, 46, 56), inkcell::Color::rgb(16, 40, 36));
            st.bold = true;
        }

        surface.fill({body.x, body.y + y, body.w, 1}, " ", st);
        // Cursor gutter
        if (onCursor) {
            auto rail = theme::selected_style();
            rail.bg = st.bg;
            surface.text({body.x, body.y + y}, "▌", rail);
        } else if (inSel) {
            surface.text({body.x, body.y + y}, "│", theme::dim().with_bg(st.bg));
        } else {
            surface.text({body.x, body.y + y}, " ", st);
        }
        surface.text({body.x + 1, body.y + y},
                     inkcell::text::fit_left(r.lines[idx], std::max(1, body.w - 1)), st);

        // Char-visual: underline-ish caret mark via inverse block at cursor col
        if (onCursor && r.visual != ReaderVisual::Line) {
            int col = std::min(r.cursorCol, std::max(0, body.w - 2));
            auto caret = theme::bright();
            caret.bg = theme::color(inkcell::Color::rgb(90, 100, 120), inkcell::Color::rgb(40, 120, 90));
            caret.bold = true;
            // Paint a one-cell caret under the cursor column on this line.
            const std::string& ln = r.lines[idx];
            // Find byte offset at display col (approx: walk glyphs).
            int used = 0;
            size_t bi = 0;
            std::string g = " ";
            while (bi < ln.size() && used < col) {
                size_t len = 1;
                unsigned char c = static_cast<unsigned char>(ln[bi]);
                if ((c & 0xE0) == 0xC0) len = 2;
                else if ((c & 0xF0) == 0xE0) len = 3;
                else if ((c & 0xF8) == 0xF0) len = 4;
                if (bi + len > ln.size()) len = 1;
                g = ln.substr(bi, len);
                int gw = std::max(1, inkcell::text::display_width(g));
                used += gw;
                bi += len;
            }
            if (bi >= ln.size()) g = " ";
            surface.text({body.x + 1 + col, body.y + y}, g, caret);
        }
    }

    if (static_cast<int>(r.lines.size()) > body.h && body.w > 2) {
        int total = static_cast<int>(r.lines.size());
        int thumb = std::max(1, body.h * body.h / total);
        int maxOff = std::max(1, total - body.h);
        int thumbY = (r.scroll * std::max(1, body.h - thumb)) / maxOff;
        for (int y = 0; y < body.h; ++y) {
            surface.put({frame.right() - 1, body.y + y},
                        (y >= thumbY && y < thumbY + thumb) ? "│" : "┆", theme::dim());
        }
    }
}

}  // namespace cortex::mk3::ui::chat
