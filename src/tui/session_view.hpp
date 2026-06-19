// src/tui/session_view.hpp — Reusable viewport renderer for the MK3 REPL TUI.
// Uses full redraws when the viewport moves/resizes, incremental row writes otherwise.
#pragma once

#include <algorithm>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "dialog.hpp"
#include "renderer.hpp"
#include "terminal.hpp"
#include "width.hpp"

namespace cortex::mk3::tui {

struct SessionViewport {
    std::vector<std::string> display;
    std::vector<std::string> visible;
    int startRow = 1;
    int skip = 0;
    int visibleCount = 0;
    int displaySize = 0;
};

class SessionView {
   public:
    SessionView(int width = 80, int height = 24) : width_(width), height_(height) {}

    void setWidthHeight(int width, int height) {
        width_ = width;
        height_ = height;
    }

    SessionViewport build(const std::vector<std::string>& history,
                          const std::vector<std::string>& rendererLines,
                          const std::vector<std::string>& dialogLines, bool showDialog,
                          int& scrollOffset) const {
        SessionViewport vp;
        vp.display = history;

        if (showDialog) {
            vp.display = dialogLines;
        } else {
            vp.display.insert(vp.display.end(), rendererLines.begin(), rendererLines.end());
        }

        const int bodyRows = std::max(1, height_ - 2);
        const int overflow = std::max(0, static_cast<int>(vp.display.size()) - bodyRows);
        scrollOffset = std::clamp(scrollOffset, 0, overflow);

        vp.startRow = height_ - 2 - static_cast<int>(vp.display.size()) + 1 + scrollOffset;
        vp.skip = 0;
        if (vp.startRow < 1) {
            vp.skip = 1 - vp.startRow;
            vp.startRow = 1;
        }

        const int remaining = std::max(0, static_cast<int>(vp.display.size()) - vp.skip);
        vp.visibleCount = std::min(remaining, bodyRows);
        vp.displaySize = std::max(0, static_cast<int>(vp.display.size()) - vp.skip);

        for (int i = vp.skip; i < vp.skip + vp.visibleCount && i < static_cast<int>(vp.display.size()); ++i)
            vp.visible.push_back(vp.display[i]);

        return vp;
    }

    std::string renderFull(const SessionViewport& vp,
                           const std::function<std::string(int)>& statusLine,
                           const std::string& promptLine) const {
        std::ostringstream out;
        out << "\033[H\033[J";
        writeVisible(out, vp);
        writeBottom(out, vp, statusLine, promptLine);
        return out.str();
    }

    std::string render(const SessionViewport& vp,
                       const std::function<std::string(int)>& statusLine,
                       const std::string& promptLine) {
        const bool viewportMoved = !hasLast_ || vp.startRow != last_.startRow ||
                                   vp.visibleCount != last_.visibleCount ||
                                   vp.displaySize != last_.displaySize;
        if (viewportMoved) {
            last_ = vp;
            hasLast_ = true;
            return renderFull(vp, statusLine, promptLine);
        }

        std::ostringstream out;
        int firstChange = -1;
        int lastChange = -1;
        const int rows = static_cast<int>(vp.visible.size());
        for (int i = 0; i < rows; ++i) {
            const std::string oldLine = i < static_cast<int>(last_.visible.size()) ? last_.visible[i] : "";
            if (oldLine != vp.visible[i]) {
                if (firstChange < 0)
                    firstChange = i;
                lastChange = i;
            }
        }

        if (firstChange >= 0) {
            for (int row = firstChange; row <= lastChange; ++row) {
                out << ansi::moveTo(vp.startRow + row, 1) << ansi::clearLine()
                    << padRight(vp.visible[row], width_);
            }
        }
        if (rows < static_cast<int>(last_.visible.size())) {
            for (int row = rows; row < static_cast<int>(last_.visible.size()); ++row)
                out << ansi::moveTo(vp.startRow + row, 1) << ansi::clearLine();
        }
        writeBottom(out, vp, statusLine, promptLine);

        last_ = vp;
        hasLast_ = true;
        return out.str();
    }

   private:
    void writeVisible(std::ostringstream& out, const SessionViewport& vp) const {
        for (int i = 0; i < static_cast<int>(vp.visible.size()); ++i) {
            out << ansi::moveTo(vp.startRow + i, 1) << ansi::clearLine()
                << padRight(vp.visible[i], width_);
        }
    }

    void writeBottom(std::ostringstream& out,
                     const SessionViewport& vp,
                     const std::function<std::string(int)>& statusLine,
                     const std::string& promptLine) const {
        if (height_ >= 2) {
            out << ansi::moveTo(height_ - 1, 1) << ansi::clearLine()
                << padRight(statusLine(vp.displaySize), width_);
            out << padRight(promptLine, width_);
        }
    }

    SessionViewport last_;
    bool hasLast_ = false;
    int width_ = 80;
    int height_ = 24;
};

}  // namespace cortex::mk3::tui
