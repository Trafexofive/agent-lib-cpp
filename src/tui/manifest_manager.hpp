#pragma once
// =============================================================================
// Dynamic two-pane Manifest Manager (manifests/ catalog)
// =============================================================================

#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "../core/agent_catalog.hpp"
#include "keys.hpp"
#include "terminal.hpp"

namespace cortex {
namespace mk3 {
namespace tui {

namespace detail {

inline int termCols() {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return (int)ws.ws_col;
    return 80;
}
inline int termRows() {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return (int)ws.ws_row;
    return 24;
}

inline std::string stripAnsi(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')))
                ++i;
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

inline int visibleLen(const std::string& s) {
    return (int)stripAnsi(s).size();
}

inline std::string padVis(const std::string& s, int width) {
    int v = visibleLen(s);
    if (v >= width)
        return s;
    return s + std::string((size_t)(width - v), ' ');
}

inline std::string truncVis(const std::string& s, int width) {
    if (width <= 0)
        return {};
    std::string plain = stripAnsi(s);
    if ((int)plain.size() <= width)
        return s;
    if (width <= 1)
        return "…";
    // Prefer plain truncate when ANSI present (avoid broken escapes)
    return plain.substr(0, (size_t)width - 1) + "…";
}

inline int countKind(const catalog::AgentEntry& a, const char* kind) {
    int n = 0;
    for (const auto& o : a.owned)
        if (o.kind == kind)
            ++n;
    return n;
}

}  // namespace detail

// Returns absolute agent.yml path, or empty on cancel / empty catalog.
inline std::string runManifestManager(const std::string& manifestDirOverride = "") {
    using namespace detail;
    auto agents = catalog::discoverAgents(manifestDirOverride);
    if (agents.empty()) {
        std::cerr << "No agents found under manifests/agents.\n\nmanifests/ roots:\n";
        for (const auto& [root, source] : catalog::manifestsSearchRoots(manifestDirOverride))
            std::cerr << "  [" << source << "] " << root << "\n";
        std::cerr << "\nInstall:\n"
                     "  $CORTEX_HOME/manifests/agents/<name>/agent.yml\n"
                     "  ~/.config/cortex/manifests/agents/<name>/agent.yml\n"
                     "List: cortex-mk3 list --agents\n";
        return {};
    }

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::cerr << "Agents (" << agents.size()
                  << ") under manifests/ — re-run with -m <name> or a TTY:\n\n";
        for (size_t i = 0; i < agents.size(); ++i) {
            for (const auto& line : catalog::formatOwnershipTree(agents[i], false))
                std::cerr << line << "\n";
            if (i + 1 < agents.size())
                std::cerr << "\n";
        }
        return {};
    }

    struct termios oldt;
    tcgetattr(STDIN_FILENO, &oldt);
    struct termios raw = oldt;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    std::cout << ansi::altScreenOn() << ansi::hideCursor() << ansi::clearScreen();
    std::cout.flush();

    auto restore = [&]() {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
        std::cout << ansi::showCursor() << ansi::clearScreen() << ansi::moveTo(1, 1)
                  << ansi::altScreenOff();
        std::cout.flush();
    };

    std::string filter;
    int sel = 0;
    int listScroll = 0;
    int treeScroll = 0;
    bool dirty = true;
    int lastCols = 0, lastRows = 0;

    auto filtered = [&]() {
        std::vector<const catalog::AgentEntry*> out;
        std::string f = filter;
        std::transform(f.begin(), f.end(), f.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        for (const auto& a : agents) {
            if (f.empty()) {
                out.push_back(&a);
                continue;
            }
            std::string hay = a.name + " " + a.summary + " " + a.source + " " + a.provider + " " +
                              a.model;
            std::transform(hay.begin(), hay.end(), hay.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (hay.find(f) != std::string::npos)
                out.push_back(&a);
        }
        return out;
    };

    auto readKey = [&](int timeoutMs) -> std::pair<KeyAction, char> {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        int r = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
        if (r <= 0)
            return {KeyAction::NONE, 0};
        char buf[64];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0)
            return {KeyAction::NONE, 0};
        std::string seq(buf, buf + n);
        if (seq[0] == 27 && seq.size() == 1) {
            timeval tv2{0, 8000};
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv2) > 0) {
                char buf2[64];
                ssize_t n2 = read(STDIN_FILENO, buf2, sizeof(buf2));
                if (n2 > 0)
                    seq.append(buf2, n2);
            }
        }
        char outChar = 0;
        KeyMap keymap;
        return {keymap.resolve(seq, outChar), outChar};
    };

    auto paint = [&](const std::vector<const catalog::AgentEntry*>& items) {
        int cols = termCols();
        int rows = termRows();
        if (cols < 40)
            cols = 40;
        if (rows < 12)
            rows = 12;

        // Layout: header(2) + body + footer(2)
        int headerH = 2;
        int footerH = 2;
        int bodyH = rows - headerH - footerH;
        if (bodyH < 4)
            bodyH = 4;

        // Left pane ~38% for list, rest for tree
        int leftW = std::max(22, cols * 38 / 100);
        if (leftW > cols - 28)
            leftW = cols - 28;
        if (leftW < 18)
            leftW = cols / 2;
        int rightW = cols - leftW - 1;  // 1 for gutter
        if (rightW < 20) {
            leftW = cols / 2;
            rightW = cols - leftW - 1;
        }

        // Keep selection visible in list
        int listH = bodyH - 1;  // title row
        if (listH < 1)
            listH = 1;
        if (sel < listScroll)
            listScroll = sel;
        if (sel >= listScroll + listH)
            listScroll = sel - listH + 1;
        if (listScroll < 0)
            listScroll = 0;

        std::ostringstream frame;

        // ── Header ──
        std::string title = " MANIFEST MANAGER ";
        std::string sub = " manifests/agents  ·  global catalog ";
        frame << ansi::moveTo(1, 1) << ansi::bg(18, 22, 36) << ansi::fg(120, 200, 255)
              << ansi::bold() << padVis(title, cols) << ansi::reset() << "\r\n";
        frame << ansi::bg(12, 14, 22) << ansi::fg(140, 150, 170)
              << padVis(sub + "  " + std::to_string(items.size()) + "/" +
                            std::to_string(agents.size()) + " agents",
                        cols)
              << ansi::reset() << "\r\n";

        // ── Body rows ──
        const catalog::AgentEntry* selected = nullptr;
        if (!items.empty() && sel >= 0 && sel < (int)items.size())
            selected = items[sel];

        std::vector<std::string> treeLines;
        if (selected)
            treeLines = catalog::formatOwnershipTree(*selected, true);
        // Apply tree scroll clamp
        int treeH = bodyH - 1;
        if (treeH < 1)
            treeH = 1;
        if (treeScroll > std::max(0, (int)treeLines.size() - treeH))
            treeScroll = std::max(0, (int)treeLines.size() - treeH);
        if (treeScroll < 0)
            treeScroll = 0;

        for (int row = 0; row < bodyH; ++row) {
            frame << ansi::moveTo(headerH + 1 + row, 1);

            // Left cell
            std::string leftCell;
            if (row == 0) {
                leftCell = std::string(ansi::fg(100, 180, 255)) + ansi::bold() + " AGENTS" +
                           ansi::reset() + ansi::dim() + "  j/k  / filter" + ansi::reset();
            } else {
                int idx = listScroll + (row - 1);
                if (idx >= 0 && idx < (int)items.size()) {
                    const auto* a = items[idx];
                    bool on = (idx == sel);
                    int t = countKind(*a, "tool");
                    int f = countKind(*a, "feed");
                    int ag = countKind(*a, "agent");
                    std::ostringstream lab;
                    if (on)
                        lab << ansi::bg(40, 70, 120) << ansi::fg(255, 255, 255) << ansi::bold()
                            << " ▸ ";
                    else
                        lab << ansi::fg(200, 210, 230) << "   ";
                    lab << a->name << ansi::reset();
                    if (on)
                        lab << ansi::bg(40, 70, 120);
                    lab << ansi::dim() << " v" << a->version << ansi::reset();
                    if (on)
                        lab << ansi::bg(40, 70, 120);
                    lab << " " << ansi::fg(120, 200, 160) << "t" << t << ansi::reset();
                    if (on)
                        lab << ansi::bg(40, 70, 120);
                    lab << ansi::fg(160, 170, 200) << " f" << f << " a" << ag << ansi::reset();
                    if (on)
                        lab << ansi::bg(40, 70, 120) << ansi::reset();
                    leftCell = lab.str();
                } else {
                    leftCell = ansi::dim() + std::string(" ") + ansi::reset();
                }
            }
            leftCell = padVis(truncVis(leftCell, leftW), leftW);

            // Gutter
            std::string gut = ansi::fg(50, 60, 80) + "│" + ansi::reset();

            // Right cell
            std::string rightCell;
            if (row == 0) {
                rightCell = std::string(ansi::fg(180, 140, 255)) + ansi::bold() + " OWNERSHIP" +
                            ansi::reset() + ansi::dim() + "  ✓ path  ◆ builtin  ✗ missing" +
                            ansi::reset();
            } else if (!selected) {
                rightCell = ansi::dim() + "  (no selection)" + ansi::reset();
            } else {
                int tidx = treeScroll + (row - 1);
                if (tidx >= 0 && tidx < (int)treeLines.size())
                    rightCell = " " + treeLines[tidx];
                else
                    rightCell = " ";
            }
            rightCell = padVis(truncVis(rightCell, rightW), rightW);

            frame << leftCell << gut << rightCell << "\r\n";
        }

        // ── Footer: filter + keys ──
        std::string filterLine = " ╱ ";
        if (filter.empty())
            filterLine += std::string(ansi::dim()) + "type to filter…" + ansi::reset();
        else
            filterLine += std::string(ansi::fg(255, 220, 120)) + filter + ansi::reset() + "▌";
        filterLine += std::string(ansi::dim()) + "   Enter select  ·  Esc quit  ·  PgUp/Dn tree" +
                     ansi::reset();
        frame << ansi::moveTo(rows - 1, 1) << ansi::bg(18, 22, 36)
              << padVis(filterLine, cols) << ansi::reset() << "\r\n";

        std::string pathLine = " ";
        if (selected) {
            pathLine += std::string(ansi::dim()) + selected->manifestPath + ansi::reset();
            if (!selected->summary.empty()) {
                std::string s = selected->summary;
                if ((int)s.size() > 40)
                    s = s.substr(0, 37) + "…";
                pathLine += std::string(ansi::dim()) + "  ·  " + s + ansi::reset();
            }
        }
        frame << ansi::moveTo(rows, 1) << ansi::bg(12, 14, 22) << padVis(pathLine, cols)
              << ansi::reset();

        std::cout << frame.str() << std::flush;
        lastCols = cols;
        lastRows = rows;
        dirty = false;
    };

    while (true) {
        auto items = filtered();
        if (sel >= (int)items.size())
            sel = std::max(0, (int)items.size() - 1);
        if (sel < 0)
            sel = 0;

        int cols = termCols(), rows = termRows();
        if (cols != lastCols || rows != lastRows)
            dirty = true;

        if (dirty)
            paint(items);

        auto [act, ch] = readKey(80);  // 80ms poll → resize feels live
        if (act == KeyAction::NONE && ch == 0)
            continue;

        dirty = true;

        if (act == KeyAction::ENTER) {
            if (items.empty())
                continue;
            std::string path = items[sel]->manifestPath;
            restore();
            return path;
        }
        if (act == KeyAction::CANCEL || act == KeyAction::EXIT) {
            restore();
            return {};
        }
        if (act == KeyAction::HISTORY_DOWN || (act == KeyAction::CHAR && ch == 'j')) {
            if (sel < (int)items.size() - 1) {
                sel++;
                treeScroll = 0;
            }
        } else if (act == KeyAction::HISTORY_UP || (act == KeyAction::CHAR && ch == 'k')) {
            if (sel > 0) {
                sel--;
                treeScroll = 0;
            }
        } else if (act == KeyAction::SCROLL_DOWN) {
            treeScroll += 3;
        } else if (act == KeyAction::SCROLL_UP) {
            treeScroll = std::max(0, treeScroll - 3);
        } else if (act == KeyAction::BACKSPACE) {
            if (!filter.empty())
                filter.pop_back();
            sel = 0;
            treeScroll = 0;
        } else if (act == KeyAction::CHAR && ch == '/') {
            // focus filter (already always-on); ignore
        } else if (act == KeyAction::CHAR && ch && std::isprint((unsigned char)ch)) {
            // j/k navigate unless filter already non-empty (then all chars filter)
            if (filter.empty() && (ch == 'j' || ch == 'k')) {
                // already handled above when filter empty — wait, we check CHAR j after HISTORY
                // Actually HISTORY handles j/k first. If we get here with j/k, HISTORY didn't match.
                // keys.hpp might map j/k to HISTORY. If not:
                if (ch == 'j' && sel < (int)items.size() - 1) {
                    sel++;
                    treeScroll = 0;
                } else if (ch == 'k' && sel > 0) {
                    sel--;
                    treeScroll = 0;
                } else if (ch != 'j' && ch != 'k') {
                    filter.push_back(ch);
                    sel = 0;
                    treeScroll = 0;
                }
            } else {
                filter.push_back(ch);
                sel = 0;
                treeScroll = 0;
            }
        }
    }
}

}  // namespace tui
}  // namespace mk3
}  // namespace cortex
