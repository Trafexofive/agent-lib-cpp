#pragma once
// Chat footer — sober instrument. No spinner, no phase rainbow, no pulse.
//
// Idle 3 / live 4:
//   coder  x-ai/grok-4.5                         1:24  sess …08174
//   tool list #l1 · open 1                       stream
//   ctx 12.4k/128k  9%  ████░░░░  arm@47% idle
//   turn 3  iter 2/80  tools 1/2  hist 8/48      ^F pane
//
// Ctrl-F cycles live / session / engine data on the same plate.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/components/chrome.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::chat {

inline std::string footerFmtBytes(int bytes) {
    if (bytes < 0) bytes = 0;
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
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d", secs / 60, secs % 60);
    return buf;
}

inline std::string fmtTok(int n) {
    if (n < 0) n = 0;
    if (n < 1000) return std::to_string(n);
    if (n < 10000) {
        int t = (n * 10) / 1000;
        return std::to_string(t / 10) + "." + std::to_string(t % 10) + "k";
    }
    return std::to_string(n / 1000) + "k";
}

inline std::string suffix8(const std::string& id) {
    if (id.empty()) return {};
    return id.size() > 8 ? id.substr(id.size() - 8) : id;
}

inline void joinChip(std::string& out, const std::string& chip) {
    if (chip.empty()) return;
    if (!out.empty()) out += "  ";
    out += chip;
}

enum class ChatFooterPane : uint8_t { Live = 0, Session = 1, Engine = 2, Count = 3 };

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

inline std::string phaseVerb(const std::string& key, const std::string& detail) {
    if (key == "think") return "thinking";
    if (key == "act") return detail.empty() ? "tool" : detail;
    if (key == "wait") return "waiting on model";
    if (key == "delegate")
        return detail.empty() ? "waiting on child" : ("child " + detail);
    if (key == "reply") return "replying";
    if (key == "ask") return "your move";
    if (key == "ready") return "ready";
    if (key == "cancel") return "stopping";
    if (key == "fail") return "failed";
    return key.empty() ? "ready" : key;
}

inline void drawUnitBar(inkcell::Surface& s, int x, int y, int width, float pct,
                        inkcell::Style on, inkcell::Style off) {
    width = std::max(8, std::min(16, width));
    pct = std::max(0.f, std::min(1.f, pct));
    int filled = static_cast<int>(pct * width + 1e-6f);
    if (pct > 0.02f && filled == 0) filled = 1;
    if (pct <= 0.f) filled = 0;
    for (int i = 0; i < width; ++i)
        s.put({x + i, y}, i < filled ? "#" : "-", i < filled ? on : off);
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
    int iterMax = 80;
    int historyUsed = 0;
    int historyMax = 1700;
    std::string phaseKey = "ready";
    std::string phaseDetail;
    std::string focusLine;
    std::string lastResultLine;
    std::string openLine;
    std::string statusHint;
    int childPending = 0;
    std::string agentName;
    std::string provider;
    std::string model;
    std::string sessionId;
    std::string path;
    std::string manifestStem;
    std::string bodyFmt;
    std::string themeName;
    int turnCount = 0;
    int queuedSteer = 0;
    int bodyMode = 0;  // 0 stream 1 compact 2 canvas
    std::vector<std::string> extraLines;
};

inline int footerBaseRows(const ChatFooterModel& f) {
    if (f.running || f.failed) return 4;
    return 3;
}

inline int footerHeightFor(const ChatFooterModel& f, int maxAvail) {
    if (maxAvail < 1) return 0;
    int want = footerBaseRows(f);
    if (want < 3) want = 3;
    if (want > 4) want = 4;
    return std::min(want, maxAvail);
}

inline constexpr int kChatFooterHMin = 3;
inline constexpr int kChatFooterHTypical = 4;

inline void drawChatFooter(inkcell::Surface& surface, inkcell::Rect box,
                           const ChatFooterModel& f) {
    if (box.h < 1 || box.w < 16) return;

    const bool live = f.running && !f.failed;
    auto bg = theme::footer_bg();
    components::fillRect(surface, box, bg);

    auto dim = theme::footer_dim().with_bg(bg.bg);
    dim.dim = false;
    auto text = theme::footer_text().with_bg(bg.bg);
    auto bright = theme::footer_bright().with_bg(bg.bg);
    auto warn = theme::footer_warn().with_bg(bg.bg);

    const int x0 = box.x + 1;
    const int rightEdge = box.right() - 1;
    const int innerW = std::max(1, box.w - 2);
    const int y0 = box.y;

    auto putRight = [&](int row, const std::string& s, inkcell::Style st) {
        if (row < 0 || row >= box.h || s.empty()) return;
        int ww = inkcell::text::display_width(s);
        int x = std::max(x0, rightEdge - ww);
        surface.text({x, y0 + row}, inkcell::text::truncate(s, innerW), st);
    };

    const int win = std::max(1, f.ctxMaxTokens > 0 ? f.ctxMaxTokens : 128000);
    const int used = std::max(0, f.ctxUsedTokens);
    float pct = static_cast<float>(used) / static_cast<float>(win);
    if (pct > 1.f) pct = 1.f;
    float trigPct = 0.f;
    if (f.ctxCompactAt > 0 && win > 0)
        trigPct = std::min(1.f, static_cast<float>(f.ctxCompactAt) / static_cast<float>(win));

    static const char* kView[] = {"stream", "compact", "canvas"};
    int bm = f.bodyMode;
    if (bm < 0 || bm > 2) bm = 0;

    auto ident = [&]() {
        std::string s = f.agentName.empty() ? std::string("-") : f.agentName;
        if (!f.provider.empty() || !f.model.empty()) {
            s += "  ";
            s += f.provider.empty() ? "?" : f.provider;
            s += "/";
            s += f.model.empty() ? "?" : f.model;
        }
        return s;
    };

    auto nowLine = [&]() {
        if (f.failed) {
            std::string s = "failed";
            if (!f.statusHint.empty()) {
                s += "  ";
                s += f.statusHint;
            }
            return s;
        }
        if (live) {
            std::string s = phaseVerb(f.phaseKey, f.phaseDetail);
            if (!f.openLine.empty() && f.phaseKey != "act") {
                s += "  ";
                s += f.openLine;
            }
            if (f.pendingOps > 0) {
                s += "  open ";
                s += std::to_string(f.pendingOps);
            }
            if (f.childPending > 0) {
                s += "  child ";
                s += std::to_string(f.childPending);
            }
            if (f.queuedSteer > 0) s += "  steer";
            return s;
        }
        if (!f.lastResultLine.empty()) return f.lastResultLine;
        if (f.turnCount > 0)
            return std::to_string(f.turnCount) +
                   (f.turnCount == 1 ? " turn" : " turns");
        return std::string("idle");
    };

    // ROW 0 — identity ……………… clock / session
    {
        std::string left = ident();
        if (f.failed) left = std::string("FAILED  ") + left;
        else if (live) left = std::string("run  ") + left;
        auto lst = f.failed ? warn : live ? bright : text;
        std::string right;
        if (live || f.turnElapsedMs > 0) right = footerFmtElapsed(f.turnElapsedMs);
        std::string sid = suffix8(f.sessionId);
        if (!sid.empty()) {
            if (!right.empty()) right += "  ";
            right += sid;
        }
        int rw = right.empty() ? 0 : inkcell::text::display_width(right) + 2;
        surface.text({x0, y0},
                     inkcell::text::truncate(left, std::max(8, innerW - rw)), lst);
        if (!right.empty()) putRight(0, right, dim);
    }

    if (box.h < 2) return;

    if (f.pane == ChatFooterPane::Session) {
        surface.text({x0, y0 + 1},
                     inkcell::text::truncate(
                         f.sessionId.empty() ? "no session" : f.sessionId, innerW),
                     text);
        if (box.h >= 3)
            surface.text({x0, y0 + 2},
                         inkcell::text::truncate(
                             f.path.empty() ? "(cwd)" : f.path, innerW),
                         dim);
        if (box.h >= 4) {
            std::string m = "turns " + std::to_string(f.turnCount);
            joinChip(m, "act " + std::to_string(f.actionCount));
            joinChip(m, "res " + std::to_string(f.resultCount));
            joinChip(m, f.manifestStem);
            surface.text({x0, y0 + 3}, inkcell::text::truncate(m, innerW), dim);
        }
        return;
    }

    if (f.pane == ChatFooterPane::Engine) {
        std::string eng = (f.provider.empty() ? "?" : f.provider) + "/" +
                          (f.model.empty() ? "?" : f.model);
        surface.text({x0, y0 + 1}, inkcell::text::truncate(eng, innerW), text);
        if (box.h >= 3) {
            std::string c = f.compactEnabled ? "compact on" : "compact off";
            if (f.compactEnabled && trigPct > 0.f) {
                c += "  arm@";
                c += std::to_string(static_cast<int>(trigPct * 100.f + 0.5f));
                c += "%";
            }
            joinChip(c, "win " + fmtTok(win));
            joinChip(c, std::string("view ") + kView[bm]);
            surface.text({x0, y0 + 2}, inkcell::text::truncate(c, innerW), dim);
        }
        if (box.h >= 4) {
            std::string h = "hist " + std::to_string(f.historyUsed) + "/" +
                            std::to_string(f.historyMax);
            joinChip(h, footerFmtBytes(f.tokenBytes));
            surface.text({x0, y0 + 3}, inkcell::text::truncate(h, innerW), dim);
        }
        return;
    }

    // ROW 1 — what is happening + view
    {
        std::string left = nowLine();
        auto lst = f.failed ? warn : text;
        std::string right = kView[bm];
        int rw = inkcell::text::display_width(right) + 2;
        surface.text({x0, y0 + 1},
                     inkcell::text::truncate(left, std::max(8, innerW - rw)), lst);
        putRight(1, right, dim);
    }

    if (box.h < 3) return;

    // ROW 2 — context
    {
        int x = x0;
        surface.text({x, y0 + 2}, "ctx", dim);
        x += 4;
        std::string nums = fmtTok(used) + "/" + fmtTok(win);
        surface.text({x, y0 + 2}, nums, bright);
        x += inkcell::text::display_width(nums) + 1;
        const int barW = std::min(14, std::max(8, innerW / 5));
        auto on = pct >= 0.85f ? warn : text;
        on.bold = true;
        auto off = dim;
        drawUnitBar(surface, x, y0 + 2, barW, pct, on, off);
        x += barW + 2;
        std::string pl = std::to_string(static_cast<int>(pct * 100.f + 0.5f)) + "%";
        surface.text({x, y0 + 2}, pl, pct >= 0.85f ? warn : dim);
        x += inkcell::text::display_width(pl) + 2;
        std::string tr;
        if (!f.compactEnabled)
            tr = "compact off";
        else if (f.compactedRecently)
            tr = "just compacted";
        else if (trigPct > 0.f) {
            tr = "arm@";
            tr += std::to_string(static_cast<int>(trigPct * 100.f + 0.5f));
            tr += "%";
            tr += (pct + 1e-6f >= trigPct) ? " armed" : " idle";
        }
        if (!tr.empty())
            surface.text({x, y0 + 2},
                         inkcell::text::truncate(tr, rightEdge - x), dim);
    }

    if (box.h < 4) return;

    // ROW 3 — counters
    {
        std::string line;
        joinChip(line, "turn " + std::to_string(std::max(0, f.turnCount)));
        joinChip(line, "iter " + std::to_string(std::max(0, f.iterCurrent)) + "/" +
                           std::to_string(std::max(0, f.iterMax)));
        std::string tv = std::to_string(std::max(0, f.resultCount)) + "/" +
                         std::to_string(std::max(0, f.actionCount));
        if (f.pendingOps > 0) tv += " open=" + std::to_string(f.pendingOps);
        joinChip(line, "tools " + tv);
        joinChip(line, "hist " + std::to_string(std::max(0, f.historyUsed)) + "/" +
                           std::to_string(std::max(0, f.historyMax)));
        joinChip(line, footerFmtBytes(f.tokenBytes));
        surface.text({x0, y0 + 3}, inkcell::text::truncate(line, innerW), dim);
    }
}

}  // namespace cortex::mk3::ui::chat
