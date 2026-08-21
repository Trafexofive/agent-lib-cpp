#pragma once
// Chat body projections (Ctrl-O): Stream | Compact.
// Canvas (chat) removed — workflow canvas is a separate page.

#include <algorithm>
#include <deque>
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
    Count = 2
};

inline const char* chatBodyModeName(ChatBodyMode m) {
    switch (m) {
        case ChatBodyMode::Stream: return "stream";
        case ChatBodyMode::Compact: return "compact";
        default: return "stream";
    }
}

inline ChatBodyMode nextChatBodyMode(ChatBodyMode m, int dir = 1) {
    int n = static_cast<int>(ChatBodyMode::Count);
    int i = (static_cast<int>(m) + (dir >= 0 ? 1 : n - 1)) % n;
    return static_cast<ChatBodyMode>(i);
}

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
        case TK::Action:
            if (actionType == "agent") return ChatBlockKind::Agent;
            if (actionName == "exec" || actionName == "sleep")
                return ChatBlockKind::ToolExec;
            if (actionName.find("write") != std::string::npos ||
                actionName == "fs_write" || actionName == "artifact")
                return ChatBlockKind::ToolWrite;
            if (actionName.find("ask") != std::string::npos)
                return ChatBlockKind::ToolAsk;
            return ChatBlockKind::ToolRead;
    }
    return ChatBlockKind::None;
}

inline std::string oneLine(const std::string& s, size_t maxW) {
    std::string t;
    t.reserve(std::min(s.size(), maxW + 4));
    for (char c : s) {
        if (c == '\n' || c == '\r') {
            if (!t.empty() && t.back() != ' ') t.push_back(' ');
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t') continue;
        t.push_back(c);
        if (t.size() >= maxW) break;
    }
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
    if (s.size() > maxW && t.size() >= 2)
        t = t.substr(0, t.size() - 1) + "…";
    return t;
}

// Fixed-width kind tag (4 cells) — monochrome index, not a rainbow.
inline const char* tag4(TimelineKind k, bool ok, const std::string& at,
                        const std::string& an) {
    using TK = TimelineKind;
    switch (k) {
        case TK::User: return "you ";
        case TK::Thought: return "··· ";
        case TK::Response:
        case TK::Final: return "out ";
        case TK::Error: return "err ";
        case TK::Status: return "sys ";
        case TK::Stream: return "raw ";
        case TK::Log: return "log ";
        case TK::Result: return ok ? "ok  " : "bad ";
        case TK::Action:
            if (at == "agent") return "sub ";
            if (an == "exec" || an == "sleep") return "run ";
            if (an.find("write") != std::string::npos) return "wr  ";
            if (an.find("ask") != std::string::npos) return "ask ";
            return "rd  ";
    }
    return "·   ";
}

inline void filterTimelineIdx(const std::deque<TimelineRow>& rows, bool showThoughts,
                              bool showRaw, std::vector<int>& idx) {
    idx.clear();
    idx.reserve(rows.size());
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const auto& r = rows[static_cast<size_t>(i)];
        if (r.kind == TimelineKind::Thought && !showThoughts) continue;
        if (r.kind == TimelineKind::Stream && !showRaw) continue;
        if (r.kind == TimelineKind::Log && r.body.empty() && r.title.empty()) continue;
        idx.push_back(i);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// COMPACT — dense operator index (not a second stream)
// ═══════════════════════════════════════════════════════════════════════
inline void drawTranscriptCompact(inkcell::Surface& surface, inkcell::Rect body,
                                  const ChatSurfaceModel& m) {
    if (body.empty()) return;
    surface.fill(body, " ", theme::base_bg());

    if (!m.timelineRows || m.timelineRows->empty()) {
        surface.text({body.x + 1, body.y}, "compact · empty", theme::dim());
        if (m.contentHWriteback) *m.contentHWriteback = 1;
        return;
    }

    std::vector<int> idx;
    filterTimelineIdx(*m.timelineRows, m.showThoughts, m.showRaw, idx);

    // Collapse adjacent action+result same id into one line.
    struct Row {
        int a = -1;
        int b = -1;
    };
    std::vector<Row> rows;
    rows.reserve(idx.size());
    for (size_t i = 0; i < idx.size(); ++i) {
        const auto& r = (*m.timelineRows)[static_cast<size_t>(idx[i])];
        if (r.kind == TimelineKind::Action && !r.actionId.empty() && i + 1 < idx.size()) {
            const auto& n = (*m.timelineRows)[static_cast<size_t>(idx[i + 1])];
            if (n.kind == TimelineKind::Result && n.actionId == r.actionId) {
                rows.push_back({idx[i], idx[i + 1]});
                ++i;
                continue;
            }
        }
        rows.push_back({idx[i], -1});
    }

    const int hdr = 1;
    const int vis = std::max(0, body.h - hdr);
    const int n = static_cast<int>(rows.size());
    const int contentH = hdr + std::max(1, n);
    const int maxOff = std::max(0, contentH - body.h);
    int off = m.followBottom ? maxOff : std::max(0, std::min(m.scrollOffset, maxOff));

    if (m.historyFocused && m.selectedRow >= 0 && vis > 0) {
        int sel = -1;
        for (int i = 0; i < n; ++i) {
            if (rows[static_cast<size_t>(i)].a == m.selectedRow ||
                rows[static_cast<size_t>(i)].b == m.selectedRow) {
                sel = i;
                break;
            }
        }
        if (sel >= 0) {
            int dataOff = std::max(0, off - hdr);
            if (sel < dataOff) off = sel;
            else if (sel >= dataOff + vis) off = sel - vis + 1 + hdr;
            // When header always at top, data scroll is separate:
            if (sel < dataOff) off = hdr + sel;
            else if (sel >= dataOff + vis) off = hdr + sel - vis + 1;
            off = std::max(0, std::min(off, maxOff));
        }
    }
    if (m.contentHWriteback) *m.contentHWriteback = contentH;

    // Header — quiet
    {
        auto st = theme::dim();
        st.bold = true;
        surface.text({body.x, body.y},
                     inkcell::text::truncate(
                         "compact  " + std::to_string(n) +
                             (m.pendingOps > 0
                                  ? "  open " + std::to_string(m.pendingOps)
                                  : "") +
                             "  ^O",
                         body.w),
                     st);
    }

    int y0 = body.y + hdr;
    int dataOff = std::max(0, off - hdr);
    auto dim = theme::dim();
    auto text = theme::text();
    auto bright = theme::bright();
    auto okSt = theme::green();
    auto badSt = theme::red();
    auto cyan = theme::cyan();

    for (int y = 0; y < vis && dataOff + y < n; ++y) {
        const auto& L = rows[static_cast<size_t>(dataOff + y)];
        const auto& r = (*m.timelineRows)[static_cast<size_t>(L.a)];
        bool sel = m.historyFocused && m.selectedRow >= 0 &&
                   (L.a == m.selectedRow || L.b == m.selectedRow);

        // Flat row — no wash fill flash; selected = bright fg only.
        auto st = sel ? bright : text;
        if (r.kind == TimelineKind::Thought) st = dim;
        if (r.kind == TimelineKind::Status) st = dim;
        if (r.kind == TimelineKind::Error) st = badSt;
        if (L.b >= 0) {
            const auto& res = (*m.timelineRows)[static_cast<size_t>(L.b)];
            st = res.ok ? (sel ? bright : okSt) : badSt;
        } else if (r.kind == TimelineKind::Result) {
            st = r.ok ? (sel ? bright : okSt) : badSt;
        } else if (r.kind == TimelineKind::User) {
            st = sel ? bright : cyan;
        }

        std::string line;
        line += sel ? "› " : "  ";
        if (L.b >= 0) {
            const auto& res = (*m.timelineRows)[static_cast<size_t>(L.b)];
            line += tag4(r.kind, r.ok, r.actionType, r.actionName);
            line += r.actionName.empty() ? "?" : r.actionName;
            if (!r.actionId.empty()) {
                line += " #";
                line += r.actionId;
            }
            line += res.ok ? " →ok" : " →fail";
            auto teaser = oneLine(res.body, 48);
            if (!teaser.empty()) {
                line += "  ";
                line += teaser;
            }
        } else {
            line += tag4(r.kind, r.ok, r.actionType, r.actionName);
            if (r.kind == TimelineKind::Action) {
                line += r.actionName.empty() ? "?" : r.actionName;
                if (!r.actionId.empty()) line += " #" + r.actionId;
                auto t = oneLine(r.body, 40);
                if (!t.empty()) {
                    line += "  ";
                    line += t;
                }
            } else if (r.kind == TimelineKind::Result) {
                line += r.ok ? "ok" : "fail";
                if (!r.actionName.empty()) line += " " + r.actionName;
                if (!r.actionId.empty()) line += " #" + r.actionId;
                auto t = oneLine(r.body, 48);
                if (!t.empty()) {
                    line += "  ";
                    line += t;
                }
            } else if (r.kind == TimelineKind::User || r.kind == TimelineKind::Response ||
                       r.kind == TimelineKind::Final || r.kind == TimelineKind::Status ||
                       r.kind == TimelineKind::Error) {
                line += oneLine(r.body.empty() ? r.title : r.body, body.w - 8);
            } else {
                line += oneLine(r.title.empty() ? r.body : r.title, body.w - 8);
            }
            if (r.drillable) line += " ↳";
        }

        surface.text({body.x, y0 + y},
                     inkcell::text::fit_left(line, std::max(1, body.w - 1)), st);
    }
}

}  // namespace cortex::mk3::ui::chat
