#pragma once
// Chat footer — 3 rows idle, 4th only while a turn is live.
// Type hierarchy: name bright, meta dim, one ctx meter. No spinner, no rainbow.

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
        return std::to_string(t / 10) + "." + std::to_string(t % 10) + "kB";
    }
    int t = (bytes * 10) / (1024 * 1024);
    return std::to_string(t / 10) + "." + std::to_string(t % 10) + "MB";
}

inline std::string footerFmtElapsed(int64_t ms) {
    if (ms < 0) ms = 0;
    if (ms < 10000) return std::to_string(static_cast<int>(ms)) + "ms";
    if (ms < 60000) {
        int t = static_cast<int>((ms + 50) / 100);
        return std::to_string(t / 10) + "." + std::to_string(t % 10) + "s";
    }
    int secs = static_cast<int>(ms / 1000);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", secs / 60, secs % 60);
    return buf;
}

inline std::string fmtTok(int n) {
    if (n < 0) n = 0;
    if (n < 1000) return std::to_string(n);
    int t = (n * 10) / 1000;
    if (n < 10000) return std::to_string(t / 10) + "." + std::to_string(t % 10) + "k";
    return std::to_string(n / 1000) + "k";
}

inline std::string suffix8(const std::string& id) {
    if (id.empty()) return {};
    return id.size() > 8 ? id.substr(id.size() - 8) : id;
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
    if (key == "wait") return "waiting";
    if (key == "delegate") return detail.empty() ? "child" : detail;
    if (key == "reply") return "replying";
    if (key == "ask") return "your move";
    if (key == "ready") return "ready";
    if (key == "cancel") return "stopping";
    if (key == "fail") return "failed";
    return key.empty() ? "ready" : key;
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
    int bodyMode = 0;  // 0 stream · 1 compact
    std::vector<std::string> extraLines;
};

inline int footerBaseRows(const ChatFooterModel&) { return 3; }

inline int footerHeightFor(const ChatFooterModel&, int maxAvail) {
    if (maxAvail < 1) return 0;
    return std::min(3, maxAvail);
}

inline constexpr int kChatFooterHMin = 3;
inline constexpr int kChatFooterHTypical = 3;

inline void drawCtxMeter(inkcell::Surface& surface, int x, int y, int barW, float pct,
                         inkcell::Style on, inkcell::Style off) {
    barW = std::max(6, std::min(16, barW));
    pct = std::max(0.f, std::min(1.f, pct));
    int filled = static_cast<int>(pct * static_cast<float>(barW) + 1e-6f);
    if (pct > 0.02f && filled == 0) filled = 1;
    for (int i = 0; i < barW; ++i)
        surface.put({x + i, y}, i < filled ? "█" : "░", i < filled ? on : off);
}

inline void drawChatFooter(inkcell::Surface& surface, inkcell::Rect box,
                           const ChatFooterModel& f) {
    if (box.h < 1 || box.w < 12) return;

    const bool live = f.running && !f.failed;
    auto bg = theme::footer_bg();
    components::fillRect(surface, box, bg);

    auto dim = theme::footer_dim().with_bg(bg.bg);
    dim.dim = false;
    auto text = theme::footer_text().with_bg(bg.bg);
    auto bright = theme::footer_bright().with_bg(bg.bg);
    auto liveSt = theme::footer_live().with_bg(bg.bg);
    auto warn = theme::footer_warn().with_bg(bg.bg);

    const int y0 = box.y;
    const int x0 = box.x + 1;
    const int inner = std::max(1, box.w - 2);
    const int right = box.right() - 1;

    auto putL = [&](int row, int x, const std::string& s, inkcell::Style st, int maxW) {
        if (row < 0 || row >= box.h || s.empty() || maxW < 1) return;
        surface.text({x, y0 + row}, inkcell::text::truncate(s, maxW), st);
    };
    auto putR = [&](int row, const std::string& s, inkcell::Style st) -> int {
        if (s.empty() || row < 0 || row >= box.h) return 0;
        int ww = inkcell::text::display_width(s);
        int x = std::max(x0, right - ww);
        surface.text({x, y0 + row}, s, st);
        return ww + 2;
    };

    const int win = std::max(1, f.ctxMaxTokens > 0 ? f.ctxMaxTokens : 128000);
    const int used = std::max(0, f.ctxUsedTokens);
    float pct = std::min(1.f, static_cast<float>(used) / static_cast<float>(win));
    const int armPct =
        (f.ctxCompactAt > 0) ? std::min(100, (f.ctxCompactAt * 100) / win) : 0;
    const char* view = (f.bodyMode == 1) ? "compact" : "stream";

    if (f.pane == ChatFooterPane::Session) {
        putL(0, x0, f.sessionId.empty() ? "no session" : f.sessionId, bright, inner);
        if (box.h >= 2) putL(1, x0, f.path.empty() ? "." : f.path, dim, inner);
        if (box.h >= 3) {
            std::string m = std::to_string(f.turnCount) + " turns";
            if (!f.manifestStem.empty()) m += "   " + f.manifestStem;
            putL(2, x0, m, dim, inner);
        }
        return;
    }
    if (f.pane == ChatFooterPane::Engine) {
        std::string eng = (f.provider.empty() ? "?" : f.provider) + " / " +
                          (f.model.empty() ? "?" : f.model);
        putL(0, x0, eng, bright, inner);
        if (box.h >= 2) {
            std::string c = f.compactEnabled ? "compact on" : "compact off";
            if (f.compactEnabled && armPct > 0) c += "  " + std::to_string(armPct) + "%";
            putL(1, x0, c, dim, inner);
        }
        if (box.h >= 3) {
            std::string h = "hist " + std::to_string(f.historyUsed) + "/" +
                            std::to_string(f.historyMax);
            h += "   ";
            h += view;
            putL(2, x0, h, dim, inner);
        }
        return;
    }

    // 0 — name                         provider / model                    clock
    {
        std::string name = f.agentName.empty() ? "-" : f.agentName;
        auto nst = f.failed ? warn : (live ? liveSt : bright);
        std::string clock;
        if (live || f.turnElapsedMs > 0)
            clock = footerFmtElapsed(f.turnElapsedMs);
        else {
            std::string sid = suffix8(f.sessionId);
            if (!sid.empty()) clock = sid;
        }
        int rw = putR(0, clock, dim);
        putL(0, x0, name, nst, std::max(4, inner - rw));
        std::string eng;
        if (!f.provider.empty() || !f.model.empty()) {
            eng += f.provider.empty() ? "?" : f.provider;
            eng += " / ";
            eng += f.model.empty() ? "?" : f.model;
        }
        if (!eng.empty()) {
            int nx = x0 + inkcell::text::display_width(name) + 3;
            int ew = right - rw - nx;
            if (ew >= 10) putL(0, nx, eng, dim, ew);
        }
    }

    if (box.h < 2) return;

    // 1 — now                                                              view
    {
        std::string now;
        auto st = text;
        if (f.failed) {
            now = f.statusHint.empty() ? "failed" : f.statusHint;
            st = warn;
        } else if (live) {
            now = phaseVerb(f.phaseKey, f.phaseDetail);
            if (!f.openLine.empty() && f.phaseKey != "act") {
                now += " · ";
                now += f.openLine;
            }
            if (f.pendingOps > 1)
                now += " · " + std::to_string(f.pendingOps) + " open";
            if (f.childPending > 0) now += " · child";
            if (f.queuedSteer > 0) now += " · steer";
        } else if (!f.lastResultLine.empty()) {
            now = f.lastResultLine;
        } else if (f.turnCount > 0) {
            now = std::to_string(f.turnCount) + (f.turnCount == 1 ? " turn" : " turns");
            st = dim;
        } else {
            now = "idle";
            st = dim;
        }
        int rw = putR(1, view, dim);
        putL(1, x0, now, st, std::max(8, inner - rw));
    }

    if (box.h < 3) return;

    // 2 — ctx meter
    {
        int x = x0;
        std::string nums = fmtTok(used) + " / " + fmtTok(win);
        surface.text({x, y0 + 2}, nums, text);
        x += inkcell::text::display_width(nums) + 2;
        const int barW = std::min(16, std::max(8, inner / 5));
        auto on = pct >= 0.85f ? warn : text;
        on.bold = true;
        drawCtxMeter(surface, x, y0 + 2, barW, pct, on, dim);
        x += barW + 2;
        std::string pl = std::to_string(static_cast<int>(pct * 100.f + 0.5f)) + "%";
        surface.text({x, y0 + 2}, pl, pct >= 0.85f ? warn : dim);
        x += inkcell::text::display_width(pl) + 3;
        std::string tr;
        if (!f.compactEnabled)
            tr = "compact off";
        else if (f.compactedRecently)
            tr = "compacted";
        else if (armPct > 0) {
            tr = "compact ";
            tr += std::to_string(armPct);
            tr += "%";
            if (pct * 100.f + 1e-3f >= static_cast<float>(armPct)) tr += " armed";
        }
        if (!tr.empty() && x < right - 2)
            surface.text({x, y0 + 2}, inkcell::text::truncate(tr, right - x), dim);
    }
}

}  // namespace cortex::mk3::ui::chat
