#pragma once
// Chat footer — cabinet plate under the prompt.
// Craft bar: elevated surface, accent rail, sparse fields. Not a status dump.
//
// Live layout (2–3 rows):
//   ▌  ⠼  waiting on coder                 1m24s
//   ▌  default · x-ai/grok-4.6    ▀▀▀▀░░░░  ·  hist 6
// Session / Engine are the same plate language, thinner content.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/assets/glyphs.hpp"
#include "src/ui/components/chrome.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::chat {

inline const char* footerSpinner(uint64_t nowMs) {
    static const char* kFrames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    return kFrames[(nowMs / 90) % 10];
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

// Honest short verbs. No synonym carousel — that read as noise, not life.
inline std::string phaseVerb(const std::string& key, const std::string& detail) {
    std::string v;
    if (key == "think") v = "thinking";
    else if (key == "act") v = detail.empty() ? "working" : detail;
    else if (key == "wait") v = "waiting on model";
    else if (key == "delegate")
        v = detail.empty() ? "waiting on child" : (std::string("waiting on ") + detail);
    else if (key == "reply") v = "replying";
    else if (key == "ask") v = "waiting for you";
    else if (key == "ready") v = "ready";
    else if (key == "cancel") v = "stopping";
    else if (key == "fail") v = "failed";
    else v = key.empty() ? "ready" : key;
    return v;
}

// Tetris-style unit bar (▀ cells), not a █░ wall of glyphs.
inline void drawUnitBar(inkcell::Surface& s, int x, int y, int width, float pct,
                        inkcell::Style on, inkcell::Style off) {
    width = std::max(4, std::min(20, width));
    pct = std::max(0.f, std::min(1.f, pct));
    int filled = static_cast<int>(std::round(pct * width));
    if (pct > 0.f && filled == 0) filled = 1;
    for (int i = 0; i < width; ++i)
        s.put({x + i, y}, "▀", i < filled ? on : off);
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
    if (id.empty()) return {};
    return id.size() > 8 ? id.substr(id.size() - 8) : id;
}

inline void joinChip(std::string& out, const std::string& chip) {
    if (chip.empty()) return;
    if (!out.empty()) out += " · ";
    out += chip;
}

// Legacy name kept for call sites that still pass a synonym key.
inline const char* phaseSynonym(const std::string& key, uint64_t /*nowMs*/) {
    if (key == "think") return "thinking";
    if (key == "act") return "working";
    if (key == "wait") return "waiting on model";
    if (key == "delegate") return "waiting on child";
    if (key == "reply") return "replying";
    if (key == "ask") return "waiting for you";
    if (key == "ready") return "ready";
    if (key == "cancel") return "stopping";
    if (key == "fail") return "failed";
    return key.c_str();
}

// Old block bar kept for any external caller.
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

// Idle: 2 rows (verb + who). Live with pressure: 3.
inline int footerBaseRows(const ChatFooterModel& f) {
    if (!f.running && !f.failed) return 2;
    return 3;
}

inline int footerHeightFor(const ChatFooterModel& f, int maxAvail) {
    if (maxAvail < 1) return 0;
    int want = footerBaseRows(f) + static_cast<int>(f.extraLines.size());
    if (want > 8) want = 8;
    if (want < 1) want = 1;
    return std::min(want, maxAvail);
}

inline constexpr int kChatFooterHMin = 1;
inline constexpr int kChatFooterHTypical = 2;

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
    const bool live = f.running && !f.failed;
    auto bg = focus ? theme::footer_bg_focus() : theme::footer_bg();

    // Plate
    components::elevatedFill(surface, box, focus || live);

    // Top hairline — separates prompt from plate (cabinet edge).
    {
        auto rule = theme::footer_dim();
        rule.bg = bg.bg;
        rule.dim = true;
        components::hairline(surface, box.x + 1, box.y, std::max(0, box.w - 1), rule);
    }

    // Accent rail — full height. Live breathes slightly (earned motion only).
    auto accent = f.failed ? theme::footer_warn()
                  : live   ? theme::footer_accent_live()
                  : focus  ? theme::footer_accent_focus()
                           : footerDimAccent(bg);
    accent.bg = bg.bg;
    if (live) {
        // Soft breath on the rail only (~2.2s). Not a second spinner.
        const double phase = (f.nowMs % 2200) / 2200.0 * 6.283185307179586;
        const double breath = 0.55 + 0.45 * std::sin(phase);
        accent.dim = breath < 0.7;
        accent.bold = true;
    }
    components::accentBar(surface, box.x, box.y, box.h, accent);

    auto bright = theme::footer_bright();
    bright.bg = bg.bg;
    auto dim = theme::footer_dim();
    dim.bg = bg.bg;
    auto text = theme::footer_text();
    text.bg = bg.bg;
    auto liveSt = theme::footer_live();
    liveSt.bg = bg.bg;
    auto violet = inkcell::Style::normal()
                      .with_fg(theme::color(inkcell::Color::rgb(180, 155, 203),
                                            inkcell::Color::rgb(219, 130, 255)))
                      .with_bg(bg.bg);
    violet.bold = true;

    const int x0 = box.x + 2;
    const int innerW = std::max(1, box.w - 3);
    const int rightEdge = box.right() - 1;

    auto putLeft = [&](int row, const std::string& s, inkcell::Style st, int maxW = 0) {
        if (row < 0 || row >= box.h || s.empty()) return;
        int w = maxW > 0 ? maxW : innerW;
        surface.text({x0, box.y + row}, inkcell::text::truncate(s, w), st);
    };
    auto putRight = [&](int row, const std::string& s, inkcell::Style st) {
        if (row < 0 || row >= box.h || s.empty()) return;
        int ww = inkcell::text::display_width(s);
        int x = std::max(x0, rightEdge - ww);
        surface.text({x, box.y + row}, inkcell::text::truncate(s, innerW), st);
    };

    // Pane dots — quiet, not "live ^F" chrome noise.
    auto paneDots = [&](int row) {
        if (row < 0 || row >= box.h) return;
        std::string dots;
        for (int i = 0; i < 3; ++i)
            dots += (static_cast<int>(f.pane) == i) ? "●" : "·";
        putRight(row, dots, dim);
    };

    if (f.pane == ChatFooterPane::Live) {
        // ── Row 0: glyph + verb … clock ─────────────────────────────
        {
            std::string verb = phaseVerb(f.phaseKey, f.phaseDetail);
            std::string glyph = live ? std::string(footerSpinner(f.nowMs))
                               : f.failed ? "✗"
                               : focus    ? "›"
                                          : "○";
            std::string left = glyph + "  " + verb;
            auto st = f.failed                         ? theme::footer_warn()
                      : f.phaseKey == "delegate"       ? violet
                      : live                           ? liveSt
                      : focus                          ? bright
                                                       : text;
            st.bg = bg.bg;

            std::string right;
            if (live || f.turnElapsedMs > 0) right = footerFmtElapsed(f.turnElapsedMs);
            if (live && f.pendingOps > 0) {
                if (!right.empty()) right += "  ·  ";
                right += std::to_string(f.pendingOps);
                right += " open";
            }
            int rightW = right.empty() ? 0 : inkcell::text::display_width(right) + 2;
            putLeft(0, left, st, std::max(8, innerW - rightW - 4));
            if (!right.empty()) putRight(0, right, dim);
        }

        // ── Row 1: who  ·  pressure bar when it matters ─────────────
        if (box.h >= 2) {
            std::string who;
            if (!f.agentName.empty()) who = f.agentName;
            if (!f.provider.empty() || !f.model.empty()) {
                std::string pm = (f.provider.empty() ? "?" : f.provider) + "/" +
                                 (f.model.empty() ? "?" : f.model);
                // Prefer short model tail when wide identity fights the bar.
                if (pm.size() > 28 && !f.model.empty()) pm = f.model;
                joinChip(who, pm);
            }
            if (who.empty()) who = f.themeName.empty() ? "cortex" : f.themeName;

            int ceil = std::max(1, f.ctxCompactAt > 0 ? f.ctxCompactAt : f.ctxMaxTokens);
            int used = std::max(0, f.ctxUsedTokens);
            float pct = static_cast<float>(used) / static_cast<float>(ceil);
            if (pct > 1.f) pct = 1.f;
            const bool showBar = live || pct >= 0.35f || f.compactedRecently;
            const int barW = 10;
            int reserve = showBar ? (barW + 8) : 6;
            putLeft(1, who, dim, std::max(10, innerW - reserve));

            if (showBar) {
                auto on = pct >= 0.85f ? theme::footer_warn()
                          : pct >= 0.55f ? theme::amber()
                                         : theme::footer_accent_focus();
                on.bg = bg.bg;
                auto off = dim;
                off.fg = theme::color(inkcell::Color::rgb(48, 48, 56),
                                      inkcell::Color::rgb(28, 36, 52));
                int bx = rightEdge - barW - 1;
                if (bx > x0 + 12)
                    drawUnitBar(surface, bx, box.y + 1, barW, pct, on, off);
            } else {
                paneDots(1);
            }
        }

        // ── Row 2 (live only): quiet hist · session ──────────────────
        if (box.h >= 3) {
            std::string left;
            if (f.historyMax > 0) {
                left = "hist ";
                left += std::to_string(std::max(0, f.historyUsed));
                left += "/";
                left += std::to_string(f.historyMax);
            }
            if (f.compactEnabled && f.compactedRecently) joinChip(left, "compacted");
            putLeft(2, left, dim);
            std::string right = suffix8(f.sessionId);
            if (!right.empty()) putRight(2, right, dim);
            else paneDots(2);
        }
        return;
    }

    if (f.pane == ChatFooterPane::Session) {
        putLeft(0, f.sessionId.empty() ? "no session" : f.sessionId, bright);
        paneDots(0);
        if (box.h >= 2) {
            std::string p = f.path.empty() ? "root" : f.path;
            putLeft(1, p, text);
        }
        if (box.h >= 3) {
            std::string m = "turns " + std::to_string(f.turnCount);
            if (f.actionCount > 0) joinChip(m, "act " + std::to_string(f.actionCount));
            if (f.resultCount > 0) joinChip(m, "res " + std::to_string(f.resultCount));
            if (f.tokenBytes > 0) joinChip(m, footerFmtBytes(f.tokenBytes));
            putLeft(2, m, dim);
        }
        return;
    }

    // Engine
    {
        std::string eng = (f.provider.empty() ? "?" : f.provider) + "/" +
                          (f.model.empty() ? "?" : f.model);
        putLeft(0, eng, bright);
        paneDots(0);
        if (box.h >= 2) {
            putLeft(1, f.bodyFmt.empty() ? "fmt default" : f.bodyFmt, text);
        }
        if (box.h >= 3) {
            std::string c = f.compactEnabled ? "compact on" : "compact off";
            joinChip(c, "win " + fmtTok(f.ctxMaxTokens));
            if (f.iterMax > 0) {
                joinChip(c, std::to_string(f.iterCurrent) + "/" + std::to_string(f.iterMax));
            }
            putLeft(2, c, dim);
        }
    }

    for (int i = 0; i < box.h && i < static_cast<int>(f.extraLines.size()); ++i)
        putLeft(i, f.extraLines[static_cast<size_t>(i)], dim);
}

}  // namespace cortex::mk3::ui::chat
