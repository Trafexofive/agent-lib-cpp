#pragma once
// Chat footer — daily-driver instrument under the prompt.
//
// Live (6 rows when height allows):
//   ▌ RUNNING · child coder #d1                         1:24  open 1
//   ▌ parent blocked until join · waiting on child
//   ▌ ctx  12.4k/128k  ████████░░░░░░░░  9%   trigger 60%
//   ▌ turn 3   iter 2/80   tools 1/2 open=1   hist 8/48   8.5KB
//   ▌ last  ✓ list #l1 · tree · squeezer                  stream ^O
//   ▌ live · sess · eng    ~/repos/…                      ab12cd34
//
// Idle keeps 4 rows (identity + last + ctx + meters). Ctrl-F panes.

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
    if (!out.empty()) out += " · ";
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
        return detail.empty() ? "waiting on child" : ("child · " + detail);
    if (key == "reply") return "replying";
    if (key == "ask") return "your move";
    if (key == "ready") return "ready";
    if (key == "cancel") return "stopping";
    if (key == "fail") return "failed";
    return key.empty() ? "ready" : key;
}

inline const char* phaseSynonym(const std::string& key, uint64_t) {
    return phaseVerb(key, {}).c_str();
}

// Context pressure — █ filled / ░ empty, honest fill math.
inline void drawUnitBar(inkcell::Surface& s, int x, int y, int width, float pct,
                        inkcell::Style on, inkcell::Style off) {
    width = std::max(10, std::min(20, width));
    pct = std::max(0.f, std::min(1.f, pct));
    int filled = static_cast<int>(std::floor(static_cast<double>(pct) * width + 1e-9));
    if (pct > 0.02f && filled == 0) filled = 1;
    if (pct <= 0.f) filled = 0;
    for (int i = 0; i < width; ++i)
        s.put({x + i, y}, i < filled ? "█" : "░", i < filled ? on : off);
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

// Daily-driver height: live uses the glass; idle still informative.
inline int footerBaseRows(const ChatFooterModel& f) {
    if (f.running || f.failed) return 6;
    return 4;
}

inline int footerHeightFor(const ChatFooterModel& f, int maxAvail) {
    if (maxAvail < 1) return 0;
    int want = footerBaseRows(f) + static_cast<int>(f.extraLines.size());
    if (want > 8) want = 8;
    if (want < 3) want = 3;
    return std::min(want, maxAvail);
}

inline constexpr int kChatFooterHMin = 3;
inline constexpr int kChatFooterHTypical = 5;

inline void drawChatFooter(inkcell::Surface& surface, inkcell::Rect box,
                           const ChatFooterModel& f) {
    if (box.h < 1 || box.w < 16) return;

    const bool focus = f.inputFocused && !f.running;
    const bool live = f.running && !f.failed;

    auto bg = theme::footer_bg();
    if (focus)
        bg = theme::footer_bg_focus();
    else if (live)
        bg = inkcell::Style::normal().with_bg(theme::color(
            inkcell::Color::rgb(22, 24, 30), inkcell::Color::rgb(10, 16, 28)));
    else if (f.failed)
        bg = inkcell::Style::normal().with_bg(theme::color(
            inkcell::Color::rgb(32, 20, 22), inkcell::Color::rgb(28, 8, 14)));

    components::fillRect(surface, box, bg);

    // Phase-colored top rule (full width) + left rail
    auto mk = [&](int r, int g, int b, int nr, int ng, int nb) {
        auto s = inkcell::Style::normal().with_bg(bg.bg);
        s.fg = theme::color(inkcell::Color::rgb(r, g, b), inkcell::Color::rgb(nr, ng, nb));
        s.bold = true;
        return s;
    };

    inkcell::Style accent = mk(70, 72, 82, 45, 55, 75);
    if (f.failed || f.phaseKey == "fail")
        accent = mk(235, 95, 105, 255, 100, 120);
    else if (f.phaseKey == "delegate")
        accent = mk(190, 125, 235, 225, 140, 255);
    else if (f.phaseKey == "think")
        accent = mk(145, 150, 215, 165, 170, 255);
    else if (f.phaseKey == "act")
        accent = mk(230, 165, 75, 255, 190, 80);
    else if (f.phaseKey == "wait")
        accent = mk(95, 190, 215, 90, 230, 255);
    else if (f.phaseKey == "reply")
        accent = mk(115, 205, 155, 105, 245, 170);
    else if (f.phaseKey == "ask")
        accent = mk(215, 135, 205, 250, 150, 240);
    else if (f.phaseKey == "cancel")
        accent = mk(220, 155, 85, 250, 180, 90);
    else if (live)
        accent = mk(125, 200, 150, 110, 240, 165);
    else if (focus)
        accent = mk(125, 180, 195, 95, 225, 255);

    // No sin-pulse on accent — phase color is steady; spinner glyph carries motion.
    (void)0;

    // Top shelf rule in phase hue
    {
        auto rule = accent;
        rule.bold = false;
        components::hairline(surface, box.x + 1, box.y, std::max(0, box.w - 1), rule);
    }
    components::accentBar(surface, box.x, box.y, box.h, accent);

    auto bright = theme::footer_bright().with_bg(bg.bg);
    auto dim = theme::footer_dim().with_bg(bg.bg);
    dim.dim = false;
    dim.fg = theme::color(inkcell::Color::rgb(128, 130, 142),
                          inkcell::Color::rgb(118, 132, 158));
    auto text = theme::footer_text().with_bg(bg.bg);
    auto cyan = mk(105, 180, 200, 95, 225, 255);
    cyan.bold = false;
    auto amber = theme::amber().with_bg(bg.bg);
    auto green = theme::green().with_bg(bg.bg);
    auto warn = theme::footer_warn().with_bg(bg.bg);
    auto violet = mk(190, 155, 225, 225, 155, 255);

    const int x0 = box.x + 2;
    const int rightEdge = box.right() - 1;
    const int innerW = std::max(1, box.w - 3);
    const int y0 = box.y;  // row 0 shares top rule; content starts same cell

    auto putRight = [&](int row, const std::string& s, inkcell::Style st) {
        if (row < 0 || row >= box.h || s.empty()) return;
        int ww = inkcell::text::display_width(s);
        int x = std::max(x0, rightEdge - ww);
        surface.text({x, y0 + row}, inkcell::text::truncate(s, innerW), st);
    };
    auto putLeft = [&](int row, const std::string& s, inkcell::Style st, int maxW = 0) {
        if (row < 0 || row >= box.h || s.empty()) return;
        int w = maxW > 0 ? maxW : innerW;
        surface.text({x0, y0 + row}, inkcell::text::truncate(s, w), st);
    };

    // Soft pane strip — no brackets
    auto paintPanes = [&](int row, int& x) {
        static const char* names[] = {"live", "sess", "eng"};
        for (int i = 0; i < 3; ++i) {
            if (i) {
                surface.text({x, y0 + row}, " · ", dim);
                x += 3;
            }
            bool on = static_cast<int>(f.pane) == i;
            auto st = on ? accent : dim;
            st.bg = bg.bg;
            st.bold = on;
            std::string lab = on ? (std::string("●") + names[i]) : names[i];
            surface.text({x, y0 + row}, lab, st);
            x += inkcell::text::display_width(lab);
        }
        surface.text({x, y0 + row}, "  ", dim);
        x += 2;
    };

    // ── Context numbers ──────────────────────────────────────────────
    const int win = std::max(1, f.ctxMaxTokens > 0 ? f.ctxMaxTokens : 128000);
    const int used = std::max(0, f.ctxUsedTokens);
    float pct = static_cast<float>(used) / static_cast<float>(win);
    if (pct > 1.f) pct = 1.f;
    float trigPct = 0.f;
    if (f.ctxCompactAt > 0 && win > 0)
        trigPct = std::min(1.f, static_cast<float>(f.ctxCompactAt) / static_cast<float>(win));

    // ═════════════════════════════════════════════════════════════════
    if (f.pane == ChatFooterPane::Live) {
        // ROW 0 — state badge · spinner · verb · focus ………… clock · open
        {
            std::string badge = f.failed ? "FAILED"
                               : live    ? "RUNNING"
                               : focus   ? "FOCUS"
                                         : "IDLE";
            auto bst = f.failed ? warn : live ? accent : focus ? cyan : dim;
            bst.bg = bg.bg;
            bst.bold = true;

            std::string glyph = live ? std::string(footerSpinner(f.nowMs))
                               : f.failed ? "✗"
                               : focus    ? "›"
                                          : "○";

            auto vst = f.failed                   ? warn
                       : f.phaseKey == "delegate" ? violet
                       : f.phaseKey == "act"      ? amber
                       : f.phaseKey == "think"    ? violet
                       : f.phaseKey == "wait"     ? cyan
                       : f.phaseKey == "reply"    ? green
                       : live                     ? accent
                       : focus                    ? bright
                                                  : text;
            vst.bg = bg.bg;
            vst.bold = true;

            std::string mid = glyph + "  " + phaseVerb(f.phaseKey, f.phaseDetail);
            if (!f.focusLine.empty() && f.phaseKey != "ready" &&
                f.focusLine.find(f.phaseDetail) == std::string::npos) {
                mid += "  ·  ";
                mid += f.focusLine;
            }

            std::string right;
            if (live || f.turnElapsedMs > 0) right = footerFmtElapsed(f.turnElapsedMs);
            if (live && f.pendingOps > 0) {
                if (!right.empty()) right += "   ";
                right += "open ";
                right += std::to_string(f.pendingOps);
            }
            if (f.childPending > 0) {
                if (!right.empty()) right += "   ";
                right += "child ";
                right += std::to_string(f.childPending);
            }
            if (f.queuedSteer > 0) {
                if (!right.empty()) right += "   ";
                right += "steer";
            }

            int x = x0;
            surface.text({x, y0}, badge, bst);
            x += inkcell::text::display_width(badge) + 2;
            int rightW = right.empty() ? 0 : inkcell::text::display_width(right) + 2;
            int maxMid = std::max(8, rightEdge - x - rightW);
            surface.text({x, y0}, inkcell::text::truncate(mid, maxMid), vst);
            if (!right.empty()) {
                auto rst = live ? amber : dim;
                rst.bg = bg.bg;
                rst.bold = live;
                putRight(0, right, rst);
            }
        }

        // ROW 1 — NOW sentence (what is actually happening)
        if (box.h >= 2) {
            std::string now;
            auto mst = text;
            if (f.failed) {
                now = "turn dead";
                if (!f.statusHint.empty()) {
                    now += " — ";
                    now += f.statusHint;
                }
                mst = warn;
            } else if (live && f.phaseKey == "delegate") {
                now = "parent blocked on child join";
                if (!f.phaseDetail.empty()) {
                    now += " · ";
                    now += f.phaseDetail;
                }
                if (!f.model.empty()) {
                    now += " · ";
                    now += f.model;
                }
                mst = violet;
            } else if (live && f.phaseKey == "act") {
                now = "tool in flight";
                if (!f.phaseDetail.empty()) {
                    now += " · ";
                    now += f.phaseDetail;
                }
                if (f.pendingOps > 1) {
                    now += " · +";
                    now += std::to_string(f.pendingOps - 1);
                    now += " queued";
                }
                mst = amber;
            } else if (live && f.phaseKey == "wait") {
                now = "no open tools — waiting on provider tokens";
                if (!f.provider.empty() || !f.model.empty()) {
                    now += " · ";
                    now += f.provider.empty() ? "?" : f.provider;
                    now += "/";
                    now += f.model.empty() ? "?" : f.model;
                }
                mst = cyan;
            } else if (live && f.phaseKey == "think") {
                now = "model streaming thought tokens";
                mst = violet;
            } else if (live && f.phaseKey == "reply") {
                now = "model streaming final reply";
                mst = green;
            } else if (live && f.phaseKey == "ask") {
                now = "blocked on operator — answer the ask card";
                mst = violet;
            } else if (live && !f.openLine.empty()) {
                now = f.openLine;
                mst = amber;
            } else if (!f.statusHint.empty() &&
                       f.statusHint.find('[') != std::string::npos) {
                now = f.statusHint;
                mst = (f.statusHint.find("ERROR") != std::string::npos ||
                       f.statusHint.find("TIMEOUT") != std::string::npos ||
                       f.statusHint.find("403") != std::string::npos)
                          ? warn
                          : amber;
            } else if (!live) {
                now = "idle";
                if (f.turnCount > 0) {
                    now += " · ";
                    now += std::to_string(f.turnCount);
                    now += f.turnCount == 1 ? " user turn complete" : " user turns complete";
                }
                if (!f.agentName.empty()) {
                    now += " · ";
                    now += f.agentName;
                }
                mst = dim;
            } else {
                now = "live — phase unresolved";
                mst = warn;
            }
            mst.bg = bg.bg;
            mst.bold = live || f.failed;
            putLeft(1, now, mst);
        }

        // ROW 2 — CONTEXT PRESSURE (the real bar)
        if (box.h >= 3) {
            int x = x0;
            auto lab = dim;
            lab.bold = true;
            surface.text({x, y0 + 2}, "ctx", lab);
            x += 4;

            std::string nums = fmtTok(used) + "/" + fmtTok(win);
            surface.text({x, y0 + 2}, nums, bright);
            x += inkcell::text::display_width(nums) + 2;

            const int barW = std::min(18, std::max(12, innerW / 4));
            auto on = pct >= 0.85f ? warn : pct >= 0.60f ? amber : green;
            on.bg = bg.bg;
            on.bold = true;
            auto off = inkcell::Style::normal().with_bg(bg.bg);
            off.fg = theme::color(inkcell::Color::rgb(48, 50, 60),
                                  inkcell::Color::rgb(28, 38, 55));
            drawUnitBar(surface, x, y0 + 2, barW, pct, on, off);
            x += barW + 2;

            std::string pl = std::to_string(static_cast<int>(std::lround(pct * 100.0))) + "%";
            auto pst = pct >= 0.85f ? warn : pct >= 0.60f ? amber : text;
            pst.bg = bg.bg;
            pst.bold = true;
            surface.text({x, y0 + 2}, pl, pst);
            x += inkcell::text::display_width(pl) + 3;

            // Compaction status — human states, not a sticky lie.
            //   off | idle (under arm) | armed (≥ arm%, will fire) | just compacted
            {
                std::string tr;
                auto tst = dim;
                if (!f.compactEnabled) {
                    tr = "compact off";
                } else if (f.compactedRecently) {
                    tr = "just compacted";
                    tst = amber;
                    tst.bold = true;
                } else if (trigPct > 0.f) {
                    const int arm = static_cast<int>(std::lround(trigPct * 100.0));
                    tr = "arm@";
                    tr += std::to_string(arm);
                    tr += "%";
                    if (pct + 1e-6f >= trigPct) {
                        tr += " · armed";
                        tst = amber;
                        tst.bold = true;
                    } else {
                        tr += " · idle";
                    }
                } else {
                    tr = "compact on";
                }
                tst.bg = bg.bg;
                surface.text({x, y0 + 2},
                             inkcell::text::truncate(tr, rightEdge - x), tst);
            }
        }

        // ROW 3 — COUNTERS (labeled, spaced)
        if (box.h >= 4) {
            int x = x0;
            auto pair = [&](const char* k, const std::string& v, inkcell::Style vs) {
                if (x >= rightEdge - 6) return;
                surface.text({x, y0 + 3}, k, dim);
                x += inkcell::text::display_width(k) + 1;
                vs.bg = bg.bg;
                vs.bold = true;
                surface.text({x, y0 + 3}, v, vs);
                x += inkcell::text::display_width(v) + 3;
            };
            pair("turn", std::to_string(std::max(0, f.turnCount)), bright);
            {
                std::string iv = std::to_string(std::max(0, f.iterCurrent)) + "/" +
                                 std::to_string(std::max(0, f.iterMax));
                pair("iter", iv, f.iterMax > 200 ? warn : bright);
            }
            {
                std::string tv = std::to_string(std::max(0, f.resultCount)) + "/" +
                                 std::to_string(std::max(0, f.actionCount));
                if (f.pendingOps > 0) {
                    tv += " open=";
                    tv += std::to_string(f.pendingOps);
                }
                pair("tools", tv, f.pendingOps > 0 ? amber : bright);
            }
            if (f.childPending > 0)
                pair("child", std::to_string(f.childPending), violet);
            pair("stream", footerFmtBytes(f.tokenBytes), cyan);
            {
                std::string hv = std::to_string(std::max(0, f.historyUsed)) + "/" +
                                 std::to_string(std::max(0, f.historyMax));
                pair("hist", hv, dim);
            }
            if (f.queuedSteer > 0) pair("steer", "yes", amber);
        }

        // ROW 4 — last/open detail · view mode
        if (box.h >= 5) {
            std::string left;
            if (live && !f.openLine.empty())
                left = f.openLine;
            else if (!f.lastResultLine.empty())
                left = std::string("last  ") + f.lastResultLine;
            else if (!f.statusHint.empty())
                left = f.statusHint;
            else
                left = "—";

            static const char* kView[] = {"stream", "compact", "canvas"};
            int bm = f.bodyMode;
            if (bm < 0 || bm > 2) bm = 0;
            std::string right = kView[bm];
            right += "  ^O";

            auto lst = (left.find("✗") != std::string::npos ||
                        left.find("ERROR") != std::string::npos)
                           ? warn
                       : (live && !f.openLine.empty()) ? amber
                                                       : text;
            lst.bg = bg.bg;
            int rw = inkcell::text::display_width(right) + 2;
            putLeft(4, left, lst, std::max(10, innerW - rw));
            putRight(4, right, cyan);
        }

        // ROW 5 — panes · path · session · model
        if (box.h >= 6) {
            int x = x0;
            paintPanes(5, x);
            if (!f.path.empty()) {
                surface.text({x, y0 + 5},
                             inkcell::text::truncate(f.path, std::max(8, rightEdge - x - 24)),
                             dim);
            }
            std::string right;
            if (!f.model.empty()) right = f.model;
            std::string sid = suffix8(f.sessionId);
            if (!sid.empty()) {
                if (!right.empty()) right += "  ";
                right += sid;
            }
            if (!right.empty()) putRight(5, right, cyan);
        }
        return;
    }

    // ── Session pane ─────────────────────────────────────────────────
    if (f.pane == ChatFooterPane::Session) {
        int x = x0;
        paintPanes(0, x);
        putLeft(0, "", dim);  // panes already painted
        surface.text({x, y0},
                     inkcell::text::truncate(
                         f.sessionId.empty() ? "no session" : f.sessionId,
                         rightEdge - x),
                     bright);
        if (box.h >= 2)
            putLeft(1, f.path.empty() ? "(cwd)" : f.path, text);
        if (box.h >= 3) {
            std::string m = "turns " + std::to_string(f.turnCount);
            joinChip(m, "act " + std::to_string(f.actionCount));
            joinChip(m, "res " + std::to_string(f.resultCount));
            joinChip(m, footerFmtBytes(f.tokenBytes));
            putLeft(2, m, dim);
        }
        if (box.h >= 4) {
            putLeft(3, f.manifestStem.empty() ? "—" : f.manifestStem, cyan);
        }
        return;
    }

    // ── Engine pane ──────────────────────────────────────────────────
    {
        int x = x0;
        paintPanes(0, x);
        std::string eng = (f.provider.empty() ? "?" : f.provider) + "/" +
                          (f.model.empty() ? "?" : f.model);
        surface.text({x, y0}, inkcell::text::truncate(eng, rightEdge - x), bright);
        if (box.h >= 2)
            putLeft(1, f.bodyFmt.empty() ? "fmt default" : f.bodyFmt, text);
        if (box.h >= 3) {
            std::string c;
            if (!f.compactEnabled) {
                c = "compact off · history_cap only";
            } else {
                c = "compact";
                if (f.ctxCompactAt > 0 && f.ctxMaxTokens > 0) {
                    int arm = static_cast<int>(std::lround(
                        100.0 * static_cast<double>(f.ctxCompactAt) /
                        static_cast<double>(std::max(1, f.ctxMaxTokens))));
                    c += " arm@";
                    c += std::to_string(arm);
                    c += "%";
                }
                joinChip(c, f.compactedRecently ? "just ran" : "waiting");
            }
            joinChip(c, "win " + fmtTok(f.ctxMaxTokens));
            joinChip(c, "hist " + std::to_string(f.historyUsed) + "/" +
                            std::to_string(f.historyMax));
            putLeft(2, c, dim);
        }
        if (box.h >= 4) {
            static const char* kView[] = {"stream", "compact", "canvas"};
            int bm = std::max(0, std::min(2, f.bodyMode));
            putLeft(3, std::string("view ") + kView[bm] + "  (^O cycle)", cyan);
        }
    }
}

}  // namespace cortex::mk3::ui::chat
