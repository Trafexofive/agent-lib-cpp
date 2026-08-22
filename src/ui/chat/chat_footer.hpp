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
    int tokenBytes = 0;        // live stream this turn, else last gen
    int lastStreamBytes = 0;
    int ctxUsedTokens = 0;
    int ctxMaxTokens = 128000;
    int ctxCompactAt = 60000;
    bool compactEnabled = false;   // compaction: enabled
    bool trimEnabled = false;      // trim: block (tail always if cap>0)
    bool trimFilter = false;       // trim policy/trigger armed
    bool compactedRecently = false;
    int trimArmTokens = 0;
    int compactArmTokens = 0;
    int tailCap = 0;
    int tailEvery = 0;
    std::string lastEconomyCode;  // TRIM | COMPACT | TAIL
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
    std::string compactProfile;  // light | balanced | aggressive | trim | …
    std::vector<std::string> extraLines;
};

inline int footerBaseRows(const ChatFooterModel&) { return 5; }

inline int footerHeightFor(const ChatFooterModel&, int maxAvail) {
    if (maxAvail < 1) return 0;
    return std::min(5, maxAvail);
}

inline constexpr int kChatFooterHMin = 5;
inline constexpr int kChatFooterHTypical = 5;

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
    auto accent = theme::footer_accent_idle().with_bg(bg.bg);
    if (live) accent = theme::footer_accent_live().with_bg(bg.bg);
    else if (f.failed) accent = theme::footer_warn().with_bg(bg.bg);
    for (int r = 0; r < box.h; ++r)
        surface.put({box.x, box.y + r}, "▌", accent);

    auto dim = theme::footer_dim().with_bg(bg.bg);
    dim.dim = false;
    auto text = theme::footer_text().with_bg(bg.bg);
    auto bright = theme::footer_bright().with_bg(bg.bg);
    auto liveSt = theme::footer_live().with_bg(bg.bg);
    auto warn = theme::footer_warn().with_bg(bg.bg);
    auto cyan = theme::cyan().with_bg(bg.bg);

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
    const char* paneHint = "^F cycle";

    if (f.pane == ChatFooterPane::Session) {
        putL(0, x0, f.sessionId.empty() ? "no session" : f.sessionId, bright, inner);
        if (box.h >= 2) putL(1, x0, f.path.empty() ? "." : f.path, dim, inner);
        if (box.h >= 3) {
            std::string m = std::to_string(f.turnCount) + " turns";
            if (!f.manifestStem.empty()) m += "   " + f.manifestStem;
            if (!f.agentName.empty()) m += "   " + f.agentName;
            putL(2, x0, m, text, inner);
        }
        if (box.h >= 4) putL(3, x0, f.bodyFmt.empty() ? view : f.bodyFmt, dim, inner);
        if (box.h >= 5) putL(4, x0, paneHint, dim, inner);
        return;
    }
    if (f.pane == ChatFooterPane::Engine) {
        std::string eng = (f.provider.empty() ? "?" : f.provider) + " / " +
                          (f.model.empty() ? "?" : f.model);
        putL(0, x0, eng, bright, inner);
        if (box.h >= 2) {
            std::string t = "trim ";
            t += f.trimFilter ? "filter" : (f.trimEnabled ? "tail" : "off");
            if (f.trimArmTokens > 0) t += "  arm " + fmtTok(f.trimArmTokens);
            putL(1, x0, t, f.trimFilter ? text : dim, inner);
        }
        if (box.h >= 3) {
            std::string c = "compact ";
            c += f.compactEnabled ? "on" : "off";
            if (f.compactEnabled && !f.compactProfile.empty()) c += "  " + f.compactProfile;
            if (f.compactArmTokens > 0) c += "  arm " + fmtTok(f.compactArmTokens);
            putL(2, x0, c, f.compactEnabled ? text : dim, inner);
        }
        if (box.h >= 4) {
            std::string h = "tail " + std::to_string(std::max(0, f.tailCap));
            if (f.tailEvery >= 0) h += " / " + std::to_string(f.tailEvery) + "t";
            h += "   hist " + std::to_string(f.historyUsed) + "/" +
                 std::to_string(f.historyMax);
            if (!f.lastEconomyCode.empty()) {
                h += "   last ";
                h += f.lastEconomyCode;
            }
            putL(3, x0, h, text, inner);
        }
        if (box.h >= 5) putL(4, x0, paneHint, dim, inner);
        return;
    }

    // 0 — identity · engine · clock (hairline already on this row's top)
    {
        std::string name = f.agentName.empty() ? "-" : f.agentName;
        auto nst = f.failed ? warn : (live ? liveSt : bright);
        nst.bold = true;
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
            eng += "/";
            eng += f.model.empty() ? "?" : f.model;
        }
        if (!eng.empty()) {
            int nx = x0 + inkcell::text::display_width(name) + 2;
            int ew = right - rw - nx;
            if (ew >= 8) putL(0, nx, eng, dim, ew);
        }
    }

    if (box.h < 2) return;

    // 1 — NOW (phase + open tools + steer). This was the dead row.
    {
        std::string now;
        auto st = text;
        if (f.failed) {
            now = f.statusHint.empty() ? "failed" : f.statusHint;
            st = warn;
        } else if (live) {
            now = phaseVerb(f.phaseKey, f.phaseDetail);
            if (!f.openLine.empty()) {
                now += "  ·  ";
                now += f.openLine;
            } else if (!f.focusLine.empty() && f.phaseKey != "wait") {
                now += "  ·  ";
                now += f.focusLine;
            }
            if (f.childPending > 0) now += "  ·  child×" + std::to_string(f.childPending);
            if (f.queuedSteer > 0) now += "  ·  steer";
            st = liveSt;
        } else if (!f.statusHint.empty() &&
                   (f.statusHint.find("FALLBACK") != std::string::npos ||
                    f.statusHint.find("TIMEOUT") != std::string::npos ||
                    f.statusHint.find("CANCEL") != std::string::npos)) {
            now = f.statusHint;
            st = warn;
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

    // 2 — context meter (real tokens, compact/trim arm)
    {
        int x = x0;
        std::string nums = fmtTok(used) + "/" + fmtTok(win);
        surface.text({x, y0 + 2}, nums, text);
        x += inkcell::text::display_width(nums) + 1;
        const int barW = std::min(18, std::max(8, inner / 5));
        auto on = pct >= 0.85f ? warn : (live ? liveSt : text);
        on.bold = true;
        drawCtxMeter(surface, x, y0 + 2, barW, pct, on, dim);
        x += barW + 2;
        std::string pl = std::to_string(static_cast<int>(pct * 100.f + 0.5f)) + "%";
        surface.text({x, y0 + 2}, pl, pct >= 0.85f ? warn : dim);
        x += inkcell::text::display_width(pl) + 2;
        std::string tr;
        if (f.compactedRecently && !f.lastEconomyCode.empty()) {
            if (f.lastEconomyCode == "TRIM") tr = "trimmed";
            else if (f.lastEconomyCode == "TAIL") tr = "tailed";
            else tr = "compacted";
        } else {
            std::string bits;
            if (f.trimFilter) bits = "trim";
            else if (f.trimEnabled && f.tailCap > 0) bits = "tail";
            if (f.compactEnabled) {
                if (!bits.empty()) bits += "+";
                bits += f.compactProfile.empty() ? "compact" : f.compactProfile;
            }
            if (bits.empty()) bits = "ctx-clean off";
            tr = bits;
            if (armPct > 0) {
                tr += " ";
                tr += std::to_string(armPct);
                tr += "%";
                if (pct * 100.f + 1e-3f >= static_cast<float>(armPct)) tr += " armed";
            }
        }
        if (!tr.empty() && x < right - 2)
            surface.text({x, y0 + 2}, inkcell::text::truncate(tr, right - x), dim);
    }

    if (box.h < 4) return;

    // 3 — occupancy (every kv is live)
    {
        auto kv = [&](int& x, const char* k, const std::string& v, inkcell::Style vs) {
            if (x >= right - 6) return;
            surface.text({x, y0 + 3}, k, dim);
            x += inkcell::text::display_width(k) + 1;
            surface.text({x, y0 + 3}, v, vs);
            x += inkcell::text::display_width(v) + 3;
        };
        int x = x0;
        kv(x, "turn", std::to_string(std::max(0, f.turnCount)), text);
        kv(x, "iter",
           std::to_string(std::max(0, f.iterCurrent)) + "/" +
               std::to_string(std::max(0, f.iterMax)),
           (live && f.iterCurrent > 0) ? liveSt : text);
        kv(x, "open", std::to_string(std::max(0, f.pendingOps)),
           f.pendingOps > 0 ? liveSt : dim);
        if (f.childPending > 0)
            kv(x, "child", std::to_string(f.childPending), cyan);
        kv(x, "hist",
           std::to_string(std::max(0, f.historyUsed)) + "/" +
               std::to_string(std::max(0, f.historyMax)),
           dim);
        if (f.tailCap > 0 && f.tailCap != f.historyMax)
            kv(x, "tail", std::to_string(f.tailCap), dim);
        else if (f.tailCap > 0 && f.historyMax == f.tailCap) {
            /* hist already shows the tail cap */
        }
        int streamB = f.running ? f.tokenBytes : (f.lastStreamBytes > 0 ? f.lastStreamBytes : f.tokenBytes);
        if (streamB > 0)
            kv(x, "stream", footerFmtBytes(streamB), f.running ? liveSt : dim);
        if (f.queuedSteer > 0) kv(x, "steer", std::to_string(f.queuedSteer), warn);
    }

    if (box.h < 5) return;

    // 4 — place (cwd · manifest · session)
    {
        std::string left = f.path.empty() ? "." : f.path;
        if (!f.manifestStem.empty()) {
            left += "  ·  ";
            left += f.manifestStem;
        }
        std::string sid = suffix8(f.sessionId);
        int rw = putR(4, sid.empty() ? paneHint : sid, dim);
        putL(4, x0, left, dim, std::max(8, inner - rw));
    }
}


}  // namespace cortex::mk3::ui::chat
