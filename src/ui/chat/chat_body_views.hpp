#pragma once
// Alternate chat body projections — Stream (default) stays in chat_view.
// Ctrl-O cycles: Stream → Compact → Graph → Stream.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/chat/chat_blocks.hpp"
#include "src/ui/theme/cortex_theme.hpp"

// Included at the bottom of chat_view.hpp after ChatSurfaceModel is complete.

namespace cortex::mk3::ui::chat {

enum class ChatBodyMode : uint8_t {
    Stream = 0,   // full timeline (default)
    Compact = 1,  // one line per block
    Graph = 2,    // node canvas
    Count = 3
};

inline const char* chatBodyModeName(ChatBodyMode m) {
    switch (m) {
        case ChatBodyMode::Stream: return "stream";
        case ChatBodyMode::Compact: return "compact";
        case ChatBodyMode::Graph: return "graph";
        default: return "stream";
    }
}

inline ChatBodyMode nextChatBodyMode(ChatBodyMode m, int dir = 1) {
    int n = static_cast<int>(ChatBodyMode::Count);
    int i = (static_cast<int>(m) + (dir >= 0 ? 1 : n - 1)) % n;
    return static_cast<ChatBodyMode>(i);
}

// ── Compact: single scannable line per semantic block ─────────────────
inline void drawTranscriptCompact(inkcell::Surface& surface, inkcell::Rect body,
                                  const ChatSurfaceModel& m) {
    if (body.empty()) return;
    surface.fill(body, " ", theme::base_bg());

    const auto& lines =
        m.transcriptSource ? *m.transcriptSource : m.transcript;
    if (lines.empty()) {
        surface.text({body.x + 2, body.y + body.h / 2}, "empty · compact",
                     theme::dim());
        return;
    }

    // Rebuild block runs from display lines (header starts a block).
    struct Row {
        ChatBlockKind kind = ChatBlockKind::None;
        std::string head;
        std::string teaser;
        bool selected = false;
    };
    std::vector<Row> rows;
    rows.reserve(64);
    ChatBlockKind cur = ChatBlockKind::None;
    std::string head;
    std::string teaser;
    bool sel = false;

    auto flush = [&]() {
        if (cur == ChatBlockKind::None && head.empty()) return;
        rows.push_back({cur, head, teaser, sel});
        head.clear();
        teaser.clear();
        sel = false;
        cur = ChatBlockKind::None;
    };

    for (const auto& line : lines) {
        std::string bare = stripSelectionMarker(line);
        bool isHeader = line.rfind("    ", 0) != 0 && !bare.empty() &&
                        bare.find("───") == std::string::npos;
        // separators
        if (bare.find("───") != std::string::npos || bare == "━" || bare.empty()) {
            continue;
        }
        bool lineSel = line.find("› ") != std::string::npos;
        if (isHeader) {
            flush();
            cur = classifyChatBlock(bare, m.agentName);
            head = bare;
            if (head.size() > 72) head = head.substr(0, 70) + "…";
            sel = lineSel;
        } else {
            if (teaser.empty()) {
                size_t i = bare.find_first_not_of(" \t│┃▎▌┊");
                teaser = i == std::string::npos ? bare : bare.substr(i);
                if (teaser.size() > 56) teaser = teaser.substr(0, 54) + "…";
            }
            sel = sel || lineSel;
        }
    }
    flush();

    int total = static_cast<int>(rows.size());
    int maxOff = std::max(0, total - body.h);
    int off = m.followBottom ? maxOff : std::max(0, std::min(m.scrollOffset, maxOff));
    if (m.contentHWriteback) *m.contentHWriteback = total;

    for (int y = 0; y < body.h && off + y < total; ++y) {
        const auto& r = rows[static_cast<size_t>(off + y)];
        auto st = blockLineStyle(r.kind, true, r.head, r.selected && m.historyFocused, m.nowMs);
        auto bg = blockStyle(r.kind, false, r.selected && m.historyFocused, m.nowMs);
        surface.fill({body.x, body.y + y, body.w, 1}, " ", bg);
        auto rail = blockRailStyle(r.kind, true, r.selected && m.historyFocused, m.nowMs);
        surface.text({body.x, body.y + y},
                     blockRailGlyph(r.kind, true, r.selected && m.historyFocused), rail);

        std::string glyph;
        switch (r.kind) {
            case ChatBlockKind::User: glyph = "you "; break;
            case ChatBlockKind::Assistant: glyph = "out "; break;
            case ChatBlockKind::Agent: glyph = "kid "; break;
            case ChatBlockKind::ToolRead: glyph = "rd  "; break;
            case ChatBlockKind::ToolExec: glyph = "run "; break;
            case ChatBlockKind::ToolWrite: glyph = "wr  "; break;
            case ChatBlockKind::ResultOk: glyph = "ok  "; break;
            case ChatBlockKind::ResultError:
            case ChatBlockKind::Error: glyph = "err "; break;
            case ChatBlockKind::Thought: glyph = "··· "; break;
            case ChatBlockKind::Notice: glyph = "sys "; break;
            default: glyph = "·   "; break;
        }
        std::string line = glyph + r.head;
        if (!r.teaser.empty() && r.kind != ChatBlockKind::User) {
            line += "  ";
            line += r.teaser;
        }
        surface.text({body.x + 1, body.y + y},
                     inkcell::text::fit_left(line, std::max(1, body.w - 2)), st);
    }
    // scrollbar
    if (total > body.h && body.w > 2) {
        int thumb = std::max(1, body.h * body.h / total);
        int thumbY = maxOff <= 0 ? 0 : (off * std::max(1, body.h - thumb)) / maxOff;
        for (int y = 0; y < body.h; ++y)
            surface.put({body.right() - 1, body.y + y},
                        (y >= thumbY && y < thumbY + thumb) ? "│" : "┆", theme::dim());
    }
}

// ── Graph: vertical node rail + edges (timeline as DAG strip) ─────────
inline void drawTranscriptGraph(inkcell::Surface& surface, inkcell::Rect body,
                                const ChatSurfaceModel& m) {
    if (body.empty()) return;
    surface.fill(body, " ", theme::base_bg());

    // Prefer structured rows if caller stuffed them via transcript lines —
    // graph reads the same display lines, keeps headers as nodes.
    const auto& lines =
        m.transcriptSource ? *m.transcriptSource : m.transcript;

    struct Node {
        ChatBlockKind kind = ChatBlockKind::None;
        std::string label;
        bool live = false;
        bool selected = false;
    };
    std::vector<Node> nodes;
    for (const auto& line : lines) {
        std::string bare = stripSelectionMarker(line);
        if (bare.empty() || bare.find("───") != std::string::npos) continue;
        bool isHeader = line.rfind("    ", 0) != 0;
        if (!isHeader) continue;
        Node n;
        n.kind = classifyChatBlock(bare, m.agentName);
        n.label = bare;
        if (n.label.size() > 40) n.label = n.label.substr(0, 38) + "…";
        n.selected = line.find("› ") != std::string::npos;
        n.live = (n.kind == ChatBlockKind::Thought || n.kind == ChatBlockKind::Assistant) &&
                 m.running && nodes.size() + 1 >= 1;
        nodes.push_back(std::move(n));
    }
    // Mark only last streaming-ish node live
    if (m.running && !nodes.empty()) {
        for (auto& n : nodes) n.live = false;
        nodes.back().live = true;
    }

    if (nodes.empty()) {
        surface.text({body.x + 2, body.y + 1}, "graph · no nodes yet", theme::dim());
        surface.text({body.x + 2, body.y + 2}, "ctrl-o cycles stream · compact · graph",
                     theme::italic_dim());
        return;
    }

    // Layout: spine at x=3, cards to the right. One node per 2 rows.
    const int rowH = 2;
    int totalH = static_cast<int>(nodes.size()) * rowH;
    int maxOff = std::max(0, totalH - body.h);
    int off = m.followBottom ? maxOff : std::max(0, std::min(m.scrollOffset, maxOff));
    if (m.contentHWriteback) *m.contentHWriteback = totalH;

    // Title strip
    {
        auto st = theme::cyan();
        st.bold = true;
        surface.text({body.x + 1, body.y}, "graph", st);
        surface.text({body.x + 7, body.y},
                     inkcell::text::truncate(
                         std::to_string(nodes.size()) + " nodes · ctrl-o", body.w - 8),
                     theme::dim());
    }

    int spineX = body.x + 2;
    int cardX = body.x + 5;
    int cardW = std::max(12, body.w - 7);

    for (size_t i = 0; i < nodes.size(); ++i) {
        int y = body.y + 1 + static_cast<int>(i) * rowH - off;
        if (y + 1 < body.y || y >= body.bottom()) continue;
        const auto& n = nodes[i];

        // spine + edge to next
        auto rail = blockRailStyle(n.kind, true, n.selected, m.nowMs);
        if (y >= body.y && y < body.bottom())
            surface.put({spineX, y}, n.live ? "◆" : "●", rail);
        if (i + 1 < nodes.size() && y + 1 >= body.y && y + 1 < body.bottom())
            surface.put({spineX, y + 1}, "│", theme::dim());

        // card
        if (y >= body.y && y < body.bottom()) {
            auto bg = blockStyle(n.kind, true, n.selected || n.live, m.nowMs);
            int w = std::min(cardW, body.right() - cardX);
            if (w > 0) {
                surface.fill({cardX, y, w, 1}, " ", bg);
                std::string lab = n.label;
                if (n.live) lab = "▸ " + lab;
                surface.text({cardX + 1, y},
                             inkcell::text::fit_left(lab, std::max(1, w - 2)), bg);
            }
        }
    }
}

}  // namespace cortex::mk3::ui::chat
