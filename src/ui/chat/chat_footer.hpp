#pragma once
// Chat footer — live instrument plate under the prompt.
//
// Always 3 rows (cabinet, not a status crumb):
//   ▌ LIVE  ⠼ waiting on model · list #x          1m24s · 2 open
//   ▌ ✓ tree #t1  21ms · 1.7KB   OR   2 open: tree, coder
//   ▌ act 4 · res 3 · 12KB · i 2/48 · ctx ███░░ 62% · grok-4.6   ···
//
// Pane cycle (^F): Live | Session | Engine — same plate language.

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
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
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
        return detail.empty() ? "waiting on child" : ("child · " + detail);
    if (key == "reply") return "replying";
    if (key == "ask") return "your move";
    if (key == "ready") return "ready";
    if (key == "cancel") return "stopping";
    if (key == "fail") return "failed";
    return key.empty() ? "ready" : key;
}

// Half-block meter — filled cells use phase hue, empty use deep plate.
inline void drawUnitBar(inkcell::Surface& s, int x, int y, int width, float pct,
                        inkcell::Style on, inkcell::Style off) {
    width = std::max(4, std::min(16, width));
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

inline const char* phaseSynonym(const std::string& key, uint64_t) {
    return phaseVerb(key, {}).c_str();  // detail-less
}

inline std::string pressureBar(float pct, int width) {
    width = std::max(4, std::min(24, width));
    pct = std::max(0.f, std::min(1.f, pct));
    int filled = static_cast<int>(std::round(pct * width));
    if (pct > 0.f && filled == 0) filled = 1;
    std::string s;
    for (int i = 0; i < width; ++i) s += (i < filled) ? "█" : "░";
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
    std::string focusLine;       // current tool/agent focus
    std::string lastResultLine;  // last completed result one-liner
    std::string openLine;        // open queue summary
    std::string statusHint;      // last STATUS/LIMIT/FALLBACK tail
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
    std::vector<std::string> extraLines;
};

// Always 3 rows — plate needs room for phase + truth + meters.
inline int footerBaseRows(const ChatFooterModel&) { return 3; }

inline int footerHeightFor(const ChatFooterModel& f, int maxAvail) {
    if (maxAvail < 1) return 0;
    int want = footerBaseRows(f) + static_cast<int>(f.extraLines.size());
    if (want > 6) want = 6;
    if (want < 1) want = 1;
    return std::min(want, maxAvail);
}

inline constexpr int kChatFooterHMin = 2;
inline constexpr int kChatFooterHTypical = 3;

inline void drawChatFooter(inkcell::Surface& surface, inkcell::Rect box,
                           const ChatFooterModel& f) {
    if (box.h < 1 || box.w < 12) return;

    const bool focus = f.inputFocused && !f.running;
    const bool live = f.running && !f.failed;
    auto bg = focus ? theme::footer_bg_focus() : theme::footer_bg();
    // Live plate slightly lifted
    if (live) {
        bg = inkcell::Style::normal().with_bg(theme::color(
            inkcell::Color::rgb(24, 26, 32), inkcell::Color::rgb(12, 18, 30)));
    }

    components::fillRect(surface, box, bg);

    // Top edge — brighter hairline (cabinet shelf under prompt)
    {
        auto edge = inkcell::Style::normal().with_bg(bg.bg);
        edge.fg = theme::color(inkcell::Color::rgb(55, 60, 72),
                               inkcell::Color::rgb(40, 70, 100));
        components::hairline(surface, box.x, box.y, box.w, edge);
    }
    // Second micro-edge for depth
    if (box.h >= 2) {
        auto edge2 = inkcell::Style::normal().with_bg(bg.bg);
        edge2.fg = theme::color(inkcell::Color::rgb(32, 34, 42),
                                inkcell::Color::rgb(18, 28, 42));
        edge2.dim = true;
        // only first cell row already filled; skip second hairline to save rows
        (void)edge2;
    }

    // ── Phase rail (full height) ─────────────────────────────────────
    auto mk = [&](int r, int g, int b, int nr, int ng, int nb) {
        auto s = inkcell::Style::normal().with_bg(bg.bg);
        s.fg = theme::color(inkcell::Color::rgb(r, g, b), inkcell::Color::rgb(nr, ng, nb));
        s.bold = true;
        return s;
    };
    inkcell::Style accent = mk(70, 70, 78, 40, 52, 72);
    if (f.failed || f.phaseKey == "fail")
        accent = mk(230, 90, 100, 255, 95, 115);
    else if (f.phaseKey == "delegate")
        accent = mk(185, 120, 230, 220, 130, 255);
    else if (f.phaseKey == "think")
        accent = mk(140, 145, 210, 160, 165, 255);
    else if (f.phaseKey == "act")
        accent = mk(220, 160, 70, 255, 185, 70);
    else if (f.phaseKey == "wait")
        accent = mk(90, 185, 210, 80, 230, 255);
    else if (f.phaseKey == "reply")
        accent = mk(110, 200, 150, 100, 240, 165);
    else if (f.phaseKey == "ask")
        accent = mk(210, 130, 200, 245, 140, 235);
    else if (f.phaseKey == "cancel")
        accent = mk(210, 150, 80, 245, 175, 80);
    else if (live)
        accent = mk(120, 195, 145, 100, 235, 160);
    else if (focus)
        accent = mk(120, 175, 190, 90, 220, 255);

    if (live) {
        const double ph = (f.nowMs % 1800) / 1800.0 * 6.283185307179586;
        accent.dim = (0.5 + 0.5 * std::sin(ph)) < 0.4;
    }
    components::accentBar(surface, box.x, box.y, box.h, accent);

    auto bright = theme::footer_bright().with_bg(bg.bg);
    auto dim = theme::footer_dim().with_bg(bg.bg);
    dim.dim = false;  // readable secondary
    dim.fg = theme::color(inkcell::Color::rgb(130, 132, 145),
                          inkcell::Color::rgb(120, 135, 160));
    auto text = theme::footer_text().with_bg(bg.bg);
    auto cyan = mk(100, 175, 195, 90, 220, 255);
    cyan.bold = false;
    auto amber = theme::amber().with_bg(bg.bg);
    auto green = theme::green().with_bg(bg.bg);
    auto warn = theme::footer_warn().with_bg(bg.bg);
    auto violet = mk(185, 150, 220, 220, 150, 255);

    const int x0 = box.x + 2;
    const int rightEdge = box.right() - 1;
    const int innerW = std::max(1, box.w - 3);

    auto putRight = [&](int row, const std::string& s, inkcell::Style st) {
        if (row < 0 || row >= box.h || s.empty()) return;
        int ww = inkcell::text::display_width(s);
        int x = std::max(x0, rightEdge - ww);
        surface.text({x, box.y + row}, inkcell::text::truncate(s, innerW), st);
    };

    // ── Pane tab strip (left of row 0 content) ───────────────────────
    auto paintPaneTabs = [&](int row, int& cursorX) {
        static const char* names[] = {"LIVE", "SESS", "ENG"};
        for (int i = 0; i < 3; ++i) {
            bool on = static_cast<int>(f.pane) == i;
            auto st = on ? accent : dim;
            st.bg = bg.bg;
            st.bold = on;
            std::string lab = on ? (std::string("[") + names[i] + "]") : names[i];
            surface.text({cursorX, box.y + row}, lab, st);
            cursorX += inkcell::text::display_width(lab) + 1;
        }
        surface.text({cursorX, box.y + row}, " ", dim);
        cursorX += 1;
    };

    // ═════════════════════════════════════════════════════════════════
    if (f.pane == ChatFooterPane::Live) {
        // ROW 0 — tabs · spinner · verb · focus ………… clock · open
        {
            int x = x0;
            paintPaneTabs(0, x);
            const int contentLeft = x;

            std::string glyph = live ? std::string(footerSpinner(f.nowMs))
                               : f.failed ? "✗"
                               : focus    ? "›"
                                          : "○";
            auto gst = f.failed ? warn
                       : f.phaseKey == "delegate" ? violet
                       : f.phaseKey == "act"      ? amber
                       : f.phaseKey == "think"    ? violet
                       : f.phaseKey == "wait"     ? cyan
                       : f.phaseKey == "reply"    ? green
                       : live                     ? accent
                       : focus                    ? bright
                                                  : text;
            gst.bg = bg.bg;
            gst.bold = true;

            std::string left = glyph + "  " + phaseVerb(f.phaseKey, f.phaseDetail);
            if (!f.focusLine.empty() && f.phaseKey != "ready" &&
                f.focusLine != f.phaseDetail) {
                left += "  ·  ";
                left += f.focusLine;
            }

            std::string right;
            if (live || f.turnElapsedMs > 0)
                right = footerFmtElapsed(f.turnElapsedMs);
            if (live && f.pendingOps > 0) {
                if (!right.empty()) right += "  ·  ";
                right += std::to_string(f.pendingOps) + " open";
            }
            if (f.childPending > 0) {
                if (!right.empty()) right += "  ·  ";
                right += std::to_string(f.childPending) + " child";
            }
            if (f.queuedSteer > 0) {
                if (!right.empty()) right += "  ·  ";
                right += "steer";
            }

            int rightW = right.empty() ? 0 : inkcell::text::display_width(right) + 2;
            int maxL = std::max(8, rightEdge - contentLeft - rightW - 1);
            surface.text({contentLeft, box.y},
                         inkcell::text::truncate(left, maxL), gst);
            if (!right.empty()) {
                auto rst = live ? amber : dim;
                rst.bg = bg.bg;
                rst.bold = live;
                putRight(0, right, rst);
            }
        }

        // ROW 1 — truth line (open queue / last result / status / idle who)
        if (box.h >= 2) {
            std::string mid;
            auto mst = text;
            if (live && !f.openLine.empty()) {
                mid = f.openLine;
                mst = amber;
            } else if (!f.statusHint.empty() &&
                       (live || f.statusHint.find('[') != std::string::npos)) {
                mid = f.statusHint;
                mst = (f.statusHint.find("FAIL") != std::string::npos ||
                       f.statusHint.find("ERROR") != std::string::npos ||
                       f.statusHint.find("403") != std::string::npos)
                          ? warn
                      : f.statusHint.find("FALLBACK") != std::string::npos ? amber
                      : f.statusHint.find("TIMEOUT") != std::string::npos  ? warn
                                                                           : cyan;
            } else if (!f.lastResultLine.empty()) {
                mid = f.lastResultLine;
                mst = (mid.find("✗") != std::string::npos) ? warn : green;
            } else if (live && f.phaseKey == "wait") {
                mid = "awaiting provider tokens…";
                mst = cyan;
            } else if (live && f.phaseKey == "think") {
                mid = "streaming thought";
                mst = violet;
            } else if (live && f.phaseKey == "delegate") {
                mid = f.phaseDetail.empty() ? "child agent running…"
                                            : ("child " + f.phaseDetail + " running…");
                mst = violet;
            } else if (live && f.phaseKey == "act") {
                mid = f.phaseDetail.empty() ? "tool running…" : f.phaseDetail;
                mst = amber;
            } else {
                // idle identity — still useful
                mid = f.agentName.empty() ? "cortex" : f.agentName;
                if (!f.provider.empty() || !f.model.empty()) {
                    mid += "  ·  ";
                    mid += f.provider.empty() ? "?" : f.provider;
                    mid += "/";
                    mid += f.model.empty() ? "?" : f.model;
                }
                if (f.turnCount > 0) {
                    mid += "  ·  ";
                    mid += std::to_string(f.turnCount);
                    mid += f.turnCount == 1 ? " turn" : " turns";
                }
                mst = dim;
            }
            mst.bg = bg.bg;

            int ceil = std::max(1, f.ctxCompactAt > 0 ? f.ctxCompactAt : f.ctxMaxTokens);
            float pct = static_cast<float>(std::max(0, f.ctxUsedTokens)) /
                        static_cast<float>(ceil);
            if (pct > 1.f) pct = 1.f;
            const int barW = 10;
            const bool showBar = true;  // always — context is load-bearing
            int reserve = showBar ? barW + 6 : 2;
            surface.text({x0, box.y + 1},
                         inkcell::text::truncate(mid, std::max(8, innerW - reserve)), mst);
            if (showBar) {
                auto on = pct >= 0.85f ? warn : pct >= 0.55f ? amber : cyan;
                on.bg = bg.bg;
                auto off = dim;
                off.fg = theme::color(inkcell::Color::rgb(38, 40, 50),
                                      inkcell::Color::rgb(22, 30, 46));
                int bx = rightEdge - barW - 1;
                // pct label left of bar
                std::string pl = std::to_string(static_cast<int>(pct * 100)) + "%";
                int plw = inkcell::text::display_width(pl);
                surface.text({std::max(x0, bx - plw - 1), box.y + 1}, pl, dim);
                drawUnitBar(surface, bx, box.y + 1, barW, pct, on, off);
            }
        }

        // ROW 2 — meters (always multi-tone chips)
        if (box.h >= 3) {
            int x = x0;
            auto chip = [&](const std::string& s, inkcell::Style st) {
                if (s.empty() || x >= rightEdge - 4) return;
                if (x > x0) {
                    surface.text({x, box.y + 2}, " · ", dim);
                    x += 3;
                }
                st.bg = bg.bg;
                std::string t = inkcell::text::truncate(s, rightEdge - x - 2);
                surface.text({x, box.y + 2}, t, st);
                x += inkcell::text::display_width(t);
            };
            auto num = text;
            num.bold = true;

            chip("act " + std::to_string(std::max(0, f.actionCount)), amber);
            chip("res " + std::to_string(std::max(0, f.resultCount)), green);
            if (f.tokenBytes > 0) chip(footerFmtBytes(f.tokenBytes), cyan);
            if (f.iterMax > 0)
                chip("i " + std::to_string(f.iterCurrent) + "/" +
                         std::to_string(f.iterMax),
                     num);
            if (f.historyMax > 0)
                chip("h " + std::to_string(f.historyUsed) + "/" +
                         std::to_string(f.historyMax),
                     dim);
            if (f.compactedRecently) chip("compacted", amber);
            if (!f.model.empty())
                chip(f.model, cyan);
            else if (!f.provider.empty())
                chip(f.provider, dim);
            if (!f.manifestStem.empty()) chip(f.manifestStem, dim);

            std::string sid = suffix8(f.sessionId);
            if (!sid.empty()) putRight(2, sid, dim);
        }
        return;
    }

    // ── Session pane ─────────────────────────────────────────────────
    if (f.pane == ChatFooterPane::Session) {
        int x = x0;
        paintPaneTabs(0, x);
        surface.text({x, box.y},
                     inkcell::text::truncate(
                         f.sessionId.empty() ? "no session" : f.sessionId,
                         rightEdge - x - 1),
                     bright);
        if (box.h >= 2) {
            std::string p = f.path.empty() ? "(cwd)" : f.path;
            surface.text({x0, box.y + 1}, inkcell::text::truncate(p, innerW), text);
        }
        if (box.h >= 3) {
            std::string m = "turns " + std::to_string(f.turnCount);
            joinChip(m, "act " + std::to_string(f.actionCount));
            joinChip(m, "res " + std::to_string(f.resultCount));
            if (f.tokenBytes > 0) joinChip(m, footerFmtBytes(f.tokenBytes));
            surface.text({x0, box.y + 2}, inkcell::text::truncate(m, innerW), dim);
        }
        return;
    }

    // ── Engine pane ──────────────────────────────────────────────────
    {
        int x = x0;
        paintPaneTabs(0, x);
        std::string eng = (f.provider.empty() ? "?" : f.provider) + "/" +
                          (f.model.empty() ? "?" : f.model);
        surface.text({x, box.y}, inkcell::text::truncate(eng, rightEdge - x - 1), bright);
        if (box.h >= 2) {
            surface.text({x0, box.y + 1},
                         inkcell::text::truncate(
                             f.bodyFmt.empty() ? "fmt default" : f.bodyFmt, innerW),
                         text);
        }
        if (box.h >= 3) {
            std::string c = f.compactEnabled ? "compact on" : "compact off";
            joinChip(c, "win " + fmtTok(f.ctxMaxTokens));
            if (f.iterMax > 0)
                joinChip(c, std::to_string(f.iterCurrent) + "/" +
                                std::to_string(f.iterMax));
            joinChip(c, f.themeName.empty() ? "theme" : f.themeName);
            surface.text({x0, box.y + 2}, inkcell::text::truncate(c, innerW), dim);
        }
    }
}

}  // namespace cortex::mk3::ui::chat
