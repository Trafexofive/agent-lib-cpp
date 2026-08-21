#pragma once
// Alternate chat body projections (Ctrl-O): Stream | Compact | Canvas.
// Compact + Canvas consume structured TimelineRow* when provided — never
// re-parse display strings (that path was slop).

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/chat/chat_blocks.hpp"
#include "src/ui/model/timeline_codec.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::chat {

enum class ChatBodyMode : uint8_t {
    Stream = 0,
    Compact = 1,
    Canvas = 2,
    Count = 3
};

inline const char* chatBodyModeName(ChatBodyMode m) {
    switch (m) {
        case ChatBodyMode::Stream: return "stream";
        case ChatBodyMode::Compact: return "compact";
        case ChatBodyMode::Canvas: return "canvas";
        default: return "stream";
    }
}

inline ChatBodyMode nextChatBodyMode(ChatBodyMode m, int dir = 1) {
    int n = static_cast<int>(ChatBodyMode::Count);
    int i = (static_cast<int>(m) + (dir >= 0 ? 1 : n - 1)) % n;
    return static_cast<ChatBodyMode>(i);
}

// ── helpers ───────────────────────────────────────────────────────────
inline ChatBlockKind kindFromTimeline(TimelineKind k, bool ok,
                                      const std::string& actionType,
                                      const std::string& actionName) {
    using TK = TimelineKind;
    switch (k) {
        case TK::User: return ChatBlockKind::User;
        case TK::Thought: return ChatBlockKind::Thought;
        case TK::Response:
        case TK::Final: return ChatBlockKind::Assistant;
        case TK::Error: return ChatBlockKind::Error;
        case TK::Status: return ChatBlockKind::Notice;
        case TK::Stream: return ChatBlockKind::Raw;
        case TK::Log: return ChatBlockKind::Notice;
        case TK::Result:
            return ok ? ChatBlockKind::ResultOk : ChatBlockKind::ResultError;
        case TK::Action: {
            if (actionType == "agent") return ChatBlockKind::Agent;
            // crude tool family
            if (actionName.find("write") != std::string::npos ||
                actionName == "fs_write" || actionName == "artifact")
                return ChatBlockKind::ToolWrite;
            if (actionName == "exec" || actionName == "sleep")
                return ChatBlockKind::ToolExec;
            if (actionName.find("ask") != std::string::npos)
                return ChatBlockKind::ToolAsk;
            if (actionName == "list" || actionName == "grep" ||
                actionName == "fs_read" || actionName == "tree" ||
                actionName == "web_fetch" || actionName == "json")
                return ChatBlockKind::ToolRead;
            return ChatBlockKind::ToolOther;
        }
    }
    return ChatBlockKind::None;
}

inline std::string oneLine(const std::string& s, size_t maxW) {
    std::string t;
    t.reserve(std::min(s.size(), maxW + 8));
    for (char c : s) {
        if (c == '\n' || c == '\r') {
            if (!t.empty() && t.back() != ' ') t.push_back(' ');
            continue;
        }
        t.push_back(c);
        if (t.size() >= maxW) break;
    }
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (s.size() > maxW || s.find('\n') != std::string::npos) {
        if (t.size() > maxW) t = t.substr(0, maxW);
        if (t.size() >= 2) t = t.substr(0, t.size() - 1) + "…";
    }
    return t;
}

inline const char* compactTag(TimelineKind k, bool ok, const std::string& at,
                              const std::string& an) {
    using TK = TimelineKind;
    switch (k) {
        case TK::User: return "YOU ";
        case TK::Thought: return "··· ";
        case TK::Response:
        case TK::Final: return "OUT ";
        case TK::Error: return "ERR ";
        case TK::Status: return "SYS ";
        case TK::Stream: return "RAW ";
        case TK::Log: return "LOG ";
        case TK::Result: return ok ? "OK  " : "BAD ";
        case TK::Action:
            if (at == "agent") return "SUB ";
            if (an == "exec" || an == "sleep") return "RUN ";
            if (an.find("write") != std::string::npos) return "WR  ";
            if (an.find("ask") != std::string::npos) return "ASK ";
            return "RD  ";
    }
    return "·   ";
}

// ── Compact: one operator line per TimelineRow ────────────────────────
inline void drawTranscriptCompact(inkcell::Surface& surface, inkcell::Rect body,
                                  const ChatSurfaceModel& m) {
    if (body.empty()) return;
    surface.fill(body, " ", theme::base_bg());

    const auto* rows = m.timelineRows;
    if (!rows || rows->empty()) {
        surface.text({body.x + 1, body.y}, "compact · no timeline rows", theme::amber());
        surface.text({body.x + 1, body.y + 1},
                     "stream view still has content · ctrl-o back", theme::dim());
        if (m.contentHWriteback) *m.contentHWriteback = 2;
        return;
    }

    // Filter like stream (thoughts/raw). idx[i] = root TimelineRow index.
    std::vector<int> idx;
    idx.reserve(rows->size());
    for (int i = 0; i < static_cast<int>(rows->size()); ++i) {
        const auto& r = (*rows)[static_cast<size_t>(i)];
        if (r.kind == TimelineKind::Thought && !m.showThoughts) continue;
        if (r.kind == TimelineKind::Stream && !m.showRaw) continue;
        idx.push_back(i);
    }

    // Optional ACT→OK pair collapse (adjacent same actionId).
    struct Line {
        int root = -1;
        int rootB = -1;  // paired result root, else -1
        bool paired = false;
    };
    std::vector<Line> lines;
    lines.reserve(idx.size());
    for (size_t i = 0; i < idx.size(); ++i) {
        const auto& r = (*rows)[static_cast<size_t>(idx[i])];
        if (r.kind == TimelineKind::Action && !r.actionId.empty() && i + 1 < idx.size()) {
            const auto& n = (*rows)[static_cast<size_t>(idx[i + 1])];
            if (n.kind == TimelineKind::Result && n.actionId == r.actionId) {
                lines.push_back({idx[static_cast<size_t>(i)], idx[i + 1], true});
                ++i;
                continue;
            }
        }
        lines.push_back({idx[static_cast<size_t>(i)], -1, false});
    }

    const int headerH = body.h >= 1 ? 1 : 0;
    const int vis = std::max(0, body.h - headerH);
    const int total = static_cast<int>(lines.size());
    // contentH includes header so transcript scroll math matches paint.
    const int contentH = headerH + std::max(1, total);
    const int maxOff = std::max(0, contentH - body.h);
    int off = m.followBottom ? maxOff : std::max(0, std::min(m.scrollOffset, maxOff));

    // Snap scroll so selected root row stays visible.
    if (m.historyFocused && m.selectedRow >= 0 && vis > 0) {
        int selLine = -1;
        for (int i = 0; i < total; ++i) {
            if (lines[static_cast<size_t>(i)].root == m.selectedRow ||
                lines[static_cast<size_t>(i)].rootB == m.selectedRow) {
                selLine = i;
                break;
            }
        }
        if (selLine >= 0) {
            // off is content scroll; first data row at content y = headerH.
            int dataOff = std::max(0, off - headerH);
            if (selLine < dataOff) off = selLine + headerH;
            else if (selLine >= dataOff + vis) off = selLine - vis + 1 + headerH;
            off = std::max(0, std::min(off, maxOff));
        }
    }
    if (m.contentHWriteback) *m.contentHWriteback = contentH;

    int nAct = 0, nOk = 0, nOpen = 0;
    for (const auto& L : lines) {
        const auto& r = (*rows)[static_cast<size_t>(L.root)];
        if (r.kind == TimelineKind::Action) ++nAct;
        if (L.paired || r.kind == TimelineKind::Result) ++nOk;
    }
    if (m.running && m.pendingOps > 0) nOpen = m.pendingOps;

    if (headerH) {
        auto st = theme::cyan();
        st.bold = true;
        surface.text({body.x + 1, body.y}, "compact", st);
        std::string meta = std::to_string(total) + " rows";
        if (nAct) meta += " · " + std::to_string(nAct) + "act";
        if (nOk) meta += " · " + std::to_string(nOk) + "ok";
        if (nOpen) meta += " · " + std::to_string(nOpen) + " open";
        meta += " · j/k · ^O";
        surface.text({body.x + 9, body.y},
                     inkcell::text::truncate(meta, body.w - 10), theme::dim());
    }

    int y0 = body.y + headerH;
    // data scroll: content rows above y0 scrolled away
    int dataOff = std::max(0, off - headerH);
    for (int y = 0; y < vis && dataOff + y < total; ++y) {
        const auto& L = lines[static_cast<size_t>(dataOff + y)];
        const auto& r = (*rows)[static_cast<size_t>(L.root)];
        bool sel = m.historyFocused && m.selectedRow >= 0 &&
                   (L.root == m.selectedRow || L.rootB == m.selectedRow);

        auto bk = kindFromTimeline(r.kind, r.ok, r.actionType, r.actionName);
        if (L.paired && L.rootB >= 0) {
            const auto& res = (*rows)[static_cast<size_t>(L.rootB)];
            bk = kindFromTimeline(res.kind, res.ok, res.actionType, res.actionName);
        }
        auto fill = blockStyle(bk, true, sel, m.nowMs);
        auto st = blockLineStyle(bk, true, r.title, sel, m.nowMs);
        const bool bar = total > vis;
        surface.fill({body.x, y0 + y, body.w - (bar ? 1 : 0), 1}, " ", fill);
        auto rail = blockRailStyle(bk, true, sel, m.nowMs);
        surface.text({body.x, y0 + y}, sel ? "▌" : "▎", rail);

        std::string line;
        if (L.paired && L.rootB >= 0) {
            const auto& res = (*rows)[static_cast<size_t>(L.rootB)];
            line = compactTag(r.kind, r.ok, r.actionType, r.actionName);
            line += r.actionName.empty() ? (r.actionType.empty() ? "act" : r.actionType)
                                         : r.actionName;
            if (!r.actionId.empty()) {
                line += " #";
                line += r.actionId;
            }
            line += res.ok ? "  → OK" : "  → FAIL";
            std::string teaser = oneLine(res.body, 36);
            if (!teaser.empty()) {
                line += "  ·  ";
                line += teaser;
            }
        } else {
            line = compactTag(r.kind, r.ok, r.actionType, r.actionName);
            std::string title;
            if (r.kind == TimelineKind::Action) {
                title = r.actionName.empty() ? (r.actionType.empty() ? "action" : r.actionType)
                                             : r.actionName;
                if (!r.actionType.empty() && !r.actionName.empty() &&
                    r.actionType != "tool") {
                    title = r.actionType + " " + r.actionName;
                }
                if (!r.actionId.empty()) title += " #" + r.actionId;
            } else if (r.kind == TimelineKind::Result) {
                title = (r.ok ? "ok " : "fail ");
                title += r.actionName.empty() ? "result" : r.actionName;
                if (!r.actionId.empty()) title += " #" + r.actionId;
            } else if (r.kind == TimelineKind::User) {
                title = oneLine(r.body.empty() ? r.title : r.body, 64);
            } else if (r.kind == TimelineKind::Status || r.kind == TimelineKind::Error) {
                title = oneLine(r.body.empty() ? r.title : r.body, 72);
            } else if (r.kind == TimelineKind::Response || r.kind == TimelineKind::Final) {
                title = oneLine(r.body.empty() ? r.title : r.body, 64);
            } else {
                title = oneLine(r.title.empty() ? r.body : r.title, 48);
            }
            line += title;
            // Teaser only when it adds info (skip if title already ate body).
            if (r.kind != TimelineKind::User && r.kind != TimelineKind::Status &&
                !r.body.empty() && r.kind != TimelineKind::Response &&
                r.kind != TimelineKind::Final) {
                std::string teaser = oneLine(r.body, 40);
                if (!teaser.empty() && title.find(teaser.substr(0, std::min<size_t>(12, teaser.size()))) ==
                                          std::string::npos) {
                    line += "  ·  ";
                    line += teaser;
                }
            }
            if (r.drillable) line += "  ↳";
        }

        surface.text({body.x + 1, y0 + y},
                     inkcell::text::fit_left(line, std::max(1, body.w - 3 - (bar ? 1 : 0))),
                     st);
    }

    if (total > vis && body.w > 2 && vis > 0) {
        int thumb = std::max(1, vis * vis / std::max(1, total));
        int thumbY =
            maxOff <= 0 ? 0 : (dataOff * std::max(1, vis - thumb)) / std::max(1, total - vis);
        for (int y = 0; y < vis; ++y)
            surface.put({body.right() - 1, y0 + y},
                        (y >= thumbY && y < thumbY + thumb) ? "│" : "┆", theme::dim());
    }
}

// ── Canvas: protocol flow as a navigable node column ──────────────────
inline void drawTranscriptCanvas(inkcell::Surface& surface, inkcell::Rect body,
                                 const ChatSurfaceModel& m) {
    if (body.empty()) return;
    surface.fill(body, " ", theme::base_bg());

    const auto* rows = m.timelineRows;
    if (!rows || rows->empty()) {
        surface.text({body.x + 1, body.y}, "canvas · empty timeline", theme::amber());
        surface.text({body.x + 1, body.y + 1}, "ctrl-o · stream · compact · canvas",
                     theme::dim());
        if (m.contentHWriteback) *m.contentHWriteback = 2;
        return;
    }

    struct Node {
        int rowIndex = 0;
        ChatBlockKind kind = ChatBlockKind::None;
        std::string label;
        std::string sub;
        std::string actionId;  // for ACT→OK edges
        bool live = false;
        bool ok = true;
        bool selected = false;
        bool edgeDown = true;
        bool linksPrev = false;  // result pairs with previous action same id
    };
    std::vector<Node> nodes;
    nodes.reserve(rows->size());

    for (int i = 0; i < static_cast<int>(rows->size()); ++i) {
        const auto& r = (*rows)[static_cast<size_t>(i)];
        if (r.kind == TimelineKind::Thought && !m.showThoughts) continue;
        if (r.kind == TimelineKind::Stream && !m.showRaw) continue;
        // Skip empty log noise
        if (r.kind == TimelineKind::Log && r.body.empty() && r.title.empty()) continue;

        Node n;
        n.rowIndex = i;
        n.kind = kindFromTimeline(r.kind, r.ok, r.actionType, r.actionName);
        n.ok = r.ok;
        n.selected = m.historyFocused && m.selectedRow == i;
        n.actionId = r.actionId;

        if (r.kind == TimelineKind::User) {
            n.label = "YOU";
            n.sub = oneLine(r.body, 48);
        } else if (r.kind == TimelineKind::Action) {
            n.label = (r.actionType == "agent" ? "SUB  " : "ACT  ");
            n.label += r.actionName.empty() ? "?" : r.actionName;
            if (!r.actionId.empty()) {
                n.label += "  #";
                n.label += r.actionId;
            }
            n.sub = oneLine(r.body, 52);
        } else if (r.kind == TimelineKind::Result) {
            n.label = r.ok ? "OK   " : "FAIL ";
            n.label += r.actionName.empty() ? "result" : r.actionName;
            if (!r.actionId.empty()) {
                n.label += "  #";
                n.label += r.actionId;
            }
            n.sub = oneLine(r.body, 52);
            if (!nodes.empty() && !r.actionId.empty() &&
                nodes.back().actionId == r.actionId)
                n.linksPrev = true;
        } else if (r.kind == TimelineKind::Response || r.kind == TimelineKind::Final) {
            n.label = "OUT";
            n.sub = oneLine(r.body.empty() ? r.title : r.body, 52);
        } else if (r.kind == TimelineKind::Thought) {
            n.label = "THINK";
            n.sub = oneLine(r.body, 52);
        } else if (r.kind == TimelineKind::Status) {
            n.label = "SYS";
            n.sub = oneLine(r.body.empty() ? r.title : r.body, 52);
        } else if (r.kind == TimelineKind::Error) {
            n.label = "ERR";
            n.sub = oneLine(r.body.empty() ? r.title : r.body, 52);
        } else {
            n.label = r.title.empty() ? "·" : r.title;
            n.sub = oneLine(r.body, 48);
        }
        nodes.push_back(std::move(n));
    }

    // Only the last Action without a following Result is "live"
    if (m.running) {
        for (auto& n : nodes) n.live = false;
        for (int i = static_cast<int>(nodes.size()) - 1; i >= 0; --i) {
            const auto& r = (*rows)[static_cast<size_t>(nodes[static_cast<size_t>(i)].rowIndex)];
            if (r.kind == TimelineKind::Action) {
                nodes[static_cast<size_t>(i)].live = true;
                break;
            }
            if (r.kind == TimelineKind::Thought || r.kind == TimelineKind::Response) {
                nodes[static_cast<size_t>(i)].live = true;
                break;
            }
        }
    }

    // Layout: denser 2-row cards + spine; paired ACT→OK share a tight edge glyph
    const int nodeH = 2;
    int contentH = 2 + static_cast<int>(nodes.size()) * nodeH;
    int maxOff = std::max(0, contentH - body.h);
    int off = m.followBottom ? maxOff : std::max(0, std::min(m.scrollOffset, maxOff));
    if (m.contentHWriteback) *m.contentHWriteback = std::max(1, contentH);

    int nAct = 0, nOk = 0, nFail = 0, nSub = 0;
    for (const auto& n : nodes) {
        if (n.label.rfind("ACT", 0) == 0) ++nAct;
        if (n.label.rfind("SUB", 0) == 0) ++nSub;
        if (n.label.rfind("OK", 0) == 0) ++nOk;
        if (n.label.rfind("FAIL", 0) == 0) ++nFail;
    }

    // Title
    {
        auto st = theme::cyan();
        st.bold = true;
        surface.text({body.x + 1, body.y}, "canvas", st);
        std::string meta = std::to_string(nodes.size()) + "n";
        if (nAct + nSub) meta += " · " + std::to_string(nAct + nSub) + "act";
        if (nOk) meta += " · " + std::to_string(nOk) + "ok";
        if (nFail) meta += " · " + std::to_string(nFail) + "fail";
        meta += " · ctrl-o";
        surface.text({body.x + 8, body.y},
                     inkcell::text::truncate(meta, body.w - 9), theme::dim());
    }

    int spineX = body.x + 2;
    int cardX = body.x + 5;
    int cardW = std::max(16, body.w - 8);

    for (size_t i = 0; i < nodes.size(); ++i) {
        int baseY = body.y + 2 + static_cast<int>(i) * nodeH - off;
        const auto& n = nodes[i];

        // spine
        if (baseY >= body.y && baseY < body.bottom()) {
            auto rail = blockRailStyle(n.kind, true, n.selected || n.live, m.nowMs);
            const char* glyph = n.live ? "◆" : (n.ok ? "●" : "✖");
            if (n.linksPrev) glyph = n.ok ? "└" : "┴";
            surface.put({spineX, baseY}, glyph, rail);
        }
        if (i + 1 < nodes.size() && baseY + 1 >= body.y && baseY + 1 < body.bottom()) {
            // Paired result: short edge; else flow spine
            const char* conn = nodes[i + 1].linksPrev ? "│" : "│";
            surface.put({spineX, baseY + 1}, conn, theme::dim());
        }

        // card row 0 — label
        if (baseY >= body.y && baseY < body.bottom()) {
            auto bg = blockStyle(n.kind, true, n.selected || n.live, m.nowMs);
            int w = std::min(cardW, body.right() - cardX - 1);
            if (w > 2) {
                surface.fill({cardX, baseY, w, 1}, " ", bg);
                std::string lab = n.label;
                if (n.linksPrev) lab = std::string("↳ ") + lab;
                if (n.live) lab = std::string("▸ ") + lab;
                if (n.selected) lab = std::string("› ") + lab;
                surface.text({cardX + 1, baseY},
                             inkcell::text::fit_left(lab, w - 2), bg);
            }
        }
        // card row 1 — sub teaser
        if (baseY + 1 >= body.y && baseY + 1 < body.bottom() && !n.sub.empty()) {
            auto bg = blockStyle(n.kind, false, n.selected, m.nowMs);
            int w = std::min(cardW, body.right() - cardX - 1);
            if (w > 2) {
                surface.fill({cardX, baseY + 1, w, 1}, " ", bg);
                surface.text({cardX + 1, baseY + 1},
                             inkcell::text::fit_left(n.sub, w - 2), bg);
            }
        }
    }

    if (nodes.empty()) {
        surface.text({body.x + 1, body.y + 2}, "no visible nodes", theme::dim());
    }
}


}  // namespace cortex::mk3::ui::chat
