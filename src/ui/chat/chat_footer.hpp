#pragma once
// Cyclable footer under the prompt box.
// Height is DYNAMIC — base rows + optional extra lines; callers must ask
// footerHeightFor() every frame and reserve that many rows (can grow anytime).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::chat {

inline const char* footerSpinner(uint64_t nowMs) {
    static const char* kFrames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    return kFrames[(nowMs / 80) % 10];
}
inline std::string footerFmtBytes(int bytes) {
    if (bytes < 1024) return std::to_string(bytes) + "B";
    if (bytes < 1024 * 1024) {
        int t = (bytes * 10) / 1024;
        return std::to_string(t / 10) + "." + std::to_string(t % 10) + "KB";
    }
    int t = (bytes * 10) / (1024 * 1024);
    return std::to_string(t / 10) + "." + std::to_string(t % 10) + "MB";
}
inline std::string footerFmtElapsed(int64_t ms) {
    if (ms < 0) ms = 0;
    if (ms < 1000) return std::to_string(static_cast<int>(ms)) + "ms";
    if (ms < 60000) {
        int t = static_cast<int>(ms / 100);
        return std::to_string(t / 10) + "." + std::to_string(t % 10) + "s";
    }
    int secs = static_cast<int>(ms / 1000);
    int m = secs / 60;
    int s = secs % 60;
    return std::to_string(m) + "m" + (s < 10 ? "0" : "") + std::to_string(s) + "s";
}

enum class ChatFooterPane : uint8_t {
    Live = 0,
    Session = 1,
    Engine = 2,
    Count = 3
};

inline const char* footerPaneName(ChatFooterPane p) {
    switch (p) {
        case ChatFooterPane::Live: return "live";
        case ChatFooterPane::Session: return "session";
        case ChatFooterPane::Engine: return "engine";
        default: return "live";
    }
}

inline ChatFooterPane nextFooterPane(ChatFooterPane p, int dir = 1) {
    int n = static_cast<int>(ChatFooterPane::Count);
    int i = (static_cast<int>(p) + (dir >= 0 ? 1 : n - 1)) % n;
    return static_cast<ChatFooterPane>(i);
}

inline const char* phaseSynonym(const std::string& key, uint64_t nowMs) {
    auto pick = [&](std::initializer_list<const char*> opts) -> const char* {
        if (opts.size() == 0) return key.c_str();
        size_t idx = static_cast<size_t>((nowMs / 2200) % opts.size());
        return *(opts.begin() + static_cast<long>(idx));
    };
    if (key == "think")
        return pick({"pondering", "thinking", "reflecting", "weighing", "mulling"});
    if (key == "act")
        return pick({"acting", "working", "executing", "in motion"});
    if (key == "wait")
        return pick({"awaiting provider", "waiting on inference", "streaming in", "holding for model"});
    if (key == "delegate")
        return pick({"waiting on child", "delegated", "subagent running", "holding for specialist"});
    if (key == "reply")
        return pick({"composing reply", "drafting", "answering", "wrapping up"});
    if (key == "ask")
        return pick({"waiting for you", "needs input", "holding for answer"});
    if (key == "ready")
        return "ready";
    if (key == "cancel")
        return pick({"cancelling", "stopping", "winding down"});
    if (key == "fail")
        return pick({"failed", "errored", "aborted"});
    return key.c_str();
}

inline std::string pressureBar(float pct, int width) {
    width = std::max(4, std::min(24, width));
    pct = std::max(0.f, std::min(1.f, pct));
    int filled = static_cast<int>(std::round(pct * width));
    if (pct > 0.f && filled == 0) filled = 1;
    std::string s;
    s.reserve(static_cast<size_t>(width) * 3);
    for (int i = 0; i < width; ++i)
        s += (i < filled) ? "█" : "░";
    return s;
}

inline std::string fmtTok(int n) {
    if (n < 1000) return std::to_string(n);
    if (n < 10000) {
        int t = (n * 10) / 1000;
        return std::to_string(t / 10) + "." + std::to_string(t % 10) + "k";
    }
    return std::to_string(n / 1000) + "k";
}

inline std::string suffix8(const std::string& id) {
    if (id.empty()) return "—";
    return id.size() > 10 ? id.substr(id.size() - 10) : id;
}

inline void joinChip(std::string& out, const std::string& chip) {
    if (chip.empty()) return;
    if (!out.empty()) out += " · ";
    out += chip;
}

struct ChatFooterModel {
    ChatFooterPane pane = ChatFooterPane::Live;
    bool running = false;
    bool failed = false;
    bool inputFocused = false;
    uint64_t nowMs = 0;
    int64_t turnElapsedMs = 0;
    int actionCount = 0;
    int resultCount = 0;
    int pendingOps = 0;
    int tokenBytes = 0;
    int ctxUsedTokens = 0;
    int ctxMaxTokens = 128000;
    int ctxCompactAt = 60000;
    bool compactEnabled = false;
    bool compactedRecently = false;
    int iterCurrent = 0;
    int iterMax = 180;
    int historyUsed = 0;
    int historyMax = 1700;
    std::string phaseKey = "ready";
    std::string phaseDetail;
    std::string agentName;
    std::string provider;
    std::string model;
    std::string sessionId;
    std::string path;
    std::string manifestStem;
    std::string bodyFmt;
    std::string themeName;
    int turnCount = 0;
    std::vector<std::string> extraLines;
};

inline int footerBaseRows(const ChatFooterModel& /*f*/) {
    return 3;
}

inline int footerHeightFor(const ChatFooterModel& f, int maxAvail) {
    if (maxAvail < 1) return 0;
    int want = footerBaseRows(f) + static_cast<int>(f.extraLines.size());
    if (want > 12) want = 12;
    if (want < 1) want = 1;
    return std::min(want, maxAvail);
}

inline constexpr int kChatFooterHMin = 1;
inline constexpr int kChatFooterHTypical = 3;

inline inkcell::Style footerDimAccent(const inkcell::Style& bg) {
    auto s = theme::footer_accent_idle();
    s.bg = bg.bg;
    s.dim = true;
    return s;
}

inline void drawChatFooter(inkcell::Surface& surface, inkcell::Rect box,
                           const ChatFooterModel& f) {
    if (box.h < 1 || box.w < 8) return;
    const bool focus = f.inputFocused && !f.running;
    auto bg = focus ? theme::footer_bg_focus() : theme::footer_bg();
    surface.fill(box, " ", bg);

    auto accent = f.failed ? theme::red().with_bg(bg.bg)
                  : f.running ? theme::footer_accent_live()
                  : focus     ? theme::footer_accent_focus()
                              : footerDimAccent(bg);
    accent.dim = false;
    accent.bold = true;

    auto bright = theme::footer_bright(); bright.bg = bg.bg;
    auto dim = theme::footer_dim(); dim.bg = bg.bg;
    auto text = theme::footer_text(); text.bg = bg.bg;

    for (int r = 0; r < box.h; ++r)
        surface.put({box.x, box.y + r}, "▌", accent);

    const int x0 = box.x + 2;
    const int rightEdge = box.right() - 2;  // keep pane chip off the rail

    auto putLeft = [&](int row, std::string s, inkcell::Style st) {
        if (row < 0 || row >= box.h || s.empty()) return;
        surface.text({x0, box.y + row},
                     inkcell::text::truncate(s, std::max(1, box.w - 4)), st);
    };
    auto putRight = [&](int row, std::string s, inkcell::Style st) {
        if (row < 0 || row >= box.h || s.empty()) return;
        int w = inkcell::text::display_width(s);
        surface.text({rightEdge - w, box.y + row},
                     inkcell::text::truncate(s, std::max(1, box.w - 8)), st);
    };

    // Pane chip is drawn per-pane (Live row 0 right; Session/Engine rows use
    // the left column, chip on row 0 right). No global chip — avoids overlap
    // with right-aligned iter/identity.

    if (f.pane == ChatFooterPane::Live) {
        // ── Row 0: TRUTH LINE — one verb, one object, one clock ────────
        // Not a telemetry essay. act/res/KB/iter live on Session pane or
        // a dim right chip only when they change the story (pending kids).
        {
            std::string phase = phaseSynonym(f.phaseKey, f.nowMs);
            if (!f.phaseDetail.empty()) {
                phase += "  ";
                phase += f.phaseDetail;
            }
            std::string g = f.running ? std::string(footerSpinner(f.nowMs))
                            : f.failed ? "✗" : "○";
            std::string left = g + "  " + phase;
            if (f.running || f.turnElapsedMs > 0)
                left += "  ·  " + footerFmtElapsed(f.turnElapsedMs);
            if (f.running && f.pendingOps > 0) {
                left += "  ·  ";
                left += std::to_string(f.pendingOps);
                left += f.pendingOps == 1 ? " open" : " open";
            }
            auto st = f.running ? bright : f.failed ? theme::red().with_bg(bg.bg) : text;
            if (f.phaseKey == "delegate") {
                st = inkcell::Style::normal()
                         .with_fg(theme::color(inkcell::Color::rgb(180, 155, 203),
                                               inkcell::Color::rgb(219, 130, 255)))
                         .with_bg(bg.bg);
                st.bold = true;
            }
            putLeft(0, left, st);
            putRight(0, std::string(footerPaneName(f.pane)) + " ^F", dim);
        }
        // ── Row 1: ctx pressure only (compact is the story, not act23) ─
        if (box.h >= 2) {
            int ceil = std::max(1, f.ctxCompactAt > 0 ? f.ctxCompactAt : f.ctxMaxTokens);
            int used = std::max(0, f.ctxUsedTokens);
            float pct = static_cast<float>(used) / static_cast<float>(ceil);
            if (pct > 1.f) pct = 1.f;
            const int barW = 16;
            std::string bar = pressureBar(pct, barW);
            char pbuf[80];
            std::snprintf(pbuf, sizeof(pbuf), "ctx %s %d%%  %s/%s",
                          bar.c_str(), static_cast<int>(pct * 100.f + 0.5f),
                          fmtTok(used).c_str(), fmtTok(ceil).c_str());
            auto st = pct >= 0.85f ? theme::red().with_bg(bg.bg)
                      : pct >= 0.50f ? theme::amber().with_bg(bg.bg) : text;
            putLeft(1, pbuf, st);
            if (f.ctxMaxTokens > 0 && f.ctxCompactAt > 0 &&
                f.ctxCompactAt < f.ctxMaxTokens && box.w > 30) {
                int markCol =
                    x0 + 4 + static_cast<int>(static_cast<float>(f.ctxCompactAt) /
                                              static_cast<float>(f.ctxMaxTokens) *
                                                  barW);
                if (markCol < box.right() - 12) {
                    auto markSt = theme::footer_bright();
                    markSt.bg = bg.bg;
                    surface.put({markCol, box.y + 1}, "▏", markSt);
                }
            }
            // Dim telemetry parked on the right of ctx — not the verb.
            std::string tel;
            if (f.running && f.actionCount > 0)
                tel = "act" + std::to_string(f.actionCount);
            if (f.running && f.resultCount > 0)
                joinChip(tel, "res" + std::to_string(f.resultCount));
            if (f.tokenBytes > 0)
                joinChip(tel, footerFmtBytes(f.tokenBytes));
            if (tel.empty()) {
                if (f.compactEnabled)
                    putRight(1, f.compactedRecently ? "compacted" : "compact on", dim);
                else
                    putRight(1, "compact off", dim);
            } else {
                putRight(1, tel, dim);
            }
        }
        // ── Row 2: identity (who) — hist is secondary ─────────────────
        if (box.h >= 3) {
            std::string id;
            if (!f.agentName.empty()) {
                id = f.agentName;
                if (!f.manifestStem.empty() && f.manifestStem != f.agentName)
                    id += "/" + f.manifestStem;
            }
            if (!f.provider.empty() || !f.model.empty()) {
                joinChip(id, (f.provider.empty() ? "?" : f.provider) + "/" +
                            (f.model.empty() ? "?" : f.model));
            }
            if (id.empty()) id = f.themeName;
            auto idSt = bright;
            idSt.bold = false;
            putLeft(2, id, idSt);

            int maxH = std::max(1, f.historyMax);
            float hpct = static_cast<float>(std::max(0, f.historyUsed)) / static_cast<float>(maxH);
            if (hpct > 1.f) hpct = 1.f;
            char hbuf[40];
            std::snprintf(hbuf, sizeof(hbuf), "hist %d/%d",
                          std::max(0, f.historyUsed), maxH);
            if (!f.sessionId.empty()) {
                std::string right = hbuf;
                joinChip(right, suffix8(f.sessionId));
                putRight(2, right, dim);
            } else {
                putRight(2, hbuf, dim);
            }
        }
        return;
    }

    if (f.pane == ChatFooterPane::Session) {
        putLeft(0, "session " + (f.sessionId.empty() ? std::string("(none)") : f.sessionId), bright);
        putLeft(1, "path " + (f.path.empty() ? std::string("root") : f.path), text);
        putRight(0, "session ^F", dim);
        if (box.h >= 3) {
            std::string meta = "turns " + std::to_string(f.turnCount) +
                               " · act" + std::to_string(f.actionCount) +
                               " · res" + std::to_string(f.resultCount);
            putLeft(2, meta, dim);
            if (f.tokenBytes > 0) putRight(2, footerFmtBytes(f.tokenBytes), dim);
        }
        return;
    }

    // Engine pane
    {
        putLeft(0, "engine " + (f.provider.empty() ? "?" : f.provider) + "/" +
                    (f.model.empty() ? "?" : f.model), bright);
        putLeft(1, f.bodyFmt.empty() ? std::string("fmt default") : f.bodyFmt, text);
        putRight(0, "engine ^F", dim);
        if (box.h >= 3) {
            std::string c = f.compactEnabled ? "compaction on" : "compaction off";
            if (f.compactedRecently) c += " · last fire";
            c += " · window " + fmtTok(f.ctxMaxTokens);
            putLeft(2, c, dim);
        }
    }

    for (int i = 0; i < box.h && i < static_cast<int>(f.extraLines.size()); ++i)
        putLeft(i, f.extraLines[static_cast<size_t>(i)], dim);
}

}  // namespace cortex::mk3::ui::chat
