// src/tui/session_view.hpp — Reusable viewport/frame compositor for the MK3 REPL TUI.
// Builds a complete logical screen, then emits either a deterministic full draw
// or an ANSI-aware dirty-row diff. This keeps scrolling/layout safe while
// avoiding full-screen repaint bytes on spinner/input ticks.
#pragma once

#include <algorithm>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "dialog.hpp"
#include "renderer.hpp"
#include "surface.hpp"
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
        width_ = std::max(1, width);
        height_ = std::max(1, height);
        invalidate();
    }

    void invalidate() { previousFrame_.clear(); }

    SessionViewport build(const std::vector<std::string>& history,
                          const std::vector<std::string>& rendererLines,
                          const std::vector<std::string>& dialogLines, bool showDialog,
                          int& scrollOffset) const {
        SessionViewport vp;

        // Do not concatenate history + rendererLines here. In real sessions both
        // can be thousands of ANSI rows, while the viewport only needs bodyRows.
        // Treat the display as a virtual sequence and materialize visible rows.
        const int historyRows = showDialog ? 0 : static_cast<int>(history.size());
        const int rendererRows = showDialog ? 0 : static_cast<int>(rendererLines.size());
        const int dialogRows = showDialog ? static_cast<int>(dialogLines.size()) : 0;
        const int totalRows = showDialog ? dialogRows : historyRows + rendererRows;

        const int bodyRows = std::max(1, height_ - 2);
        const int overflow = std::max(0, totalRows - bodyRows);
        scrollOffset = std::clamp(scrollOffset, 0, overflow);

        vp.startRow = height_ - 2 - totalRows + 1 + scrollOffset;
        vp.skip = 0;
        if (vp.startRow < 1) {
            vp.skip = 1 - vp.startRow;
            vp.startRow = 1;
        }

        const int remaining = std::max(0, totalRows - vp.skip);
        vp.visibleCount = std::min(remaining, bodyRows);
        vp.displaySize = std::max(0, totalRows - vp.skip);
        vp.visible.reserve(static_cast<size_t>(vp.visibleCount));

        for (int i = vp.skip; i < vp.skip + vp.visibleCount && i < totalRows; ++i) {
            if (showDialog) {
                vp.visible.push_back(dialogLines[static_cast<size_t>(i)]);
            } else if (i < historyRows) {
                vp.visible.push_back(history[static_cast<size_t>(i)]);
            } else {
                vp.visible.push_back(rendererLines[static_cast<size_t>(i - historyRows)]);
            }
        }
        vp.display = vp.visible;  // retained for debug callers; no full scrollback copy.

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
                       const std::string& promptLine, bool forceFull = false) {
        auto frame = buildFrame(vp, statusLine, promptLine);
        if (forceFull || previousFrame_.empty() || previousFrame_.size() != frame.size()) {
            previousFrame_ = frame;
            return renderFrameFull(frame);
        }
        std::string diff = TuiSurface::renderDiff(previousFrame_, frame, 1);
        previousFrame_ = std::move(frame);
        return diff;
    }

   private:
    std::vector<std::string> buildFrame(const SessionViewport& vp,
                                        const std::function<std::string(int)>& statusLine,
                                        const std::string& promptLine) const {
        std::vector<std::string> frame(static_cast<size_t>(height_), padRight("", width_));
        for (int i = 0; i < static_cast<int>(vp.visible.size()); ++i) {
            const int row = vp.startRow + i - 1;
            if (row >= 0 && row < height_)
                frame[static_cast<size_t>(row)] = TuiSurface::fitRow(vp.visible[i], width_);
        }
        if (height_ >= 2) {
            frame[static_cast<size_t>(height_ - 2)] = TuiSurface::fitRow(statusLine(vp.displaySize), width_);
            frame[static_cast<size_t>(height_ - 1)] = TuiSurface::fitRow(promptLine, width_);
        }
        return frame;
    }

    std::string renderFrameFull(const std::vector<std::string>& frame) const {
        std::ostringstream out;
        out << "\033[H\033[J";
        for (int row = 0; row < static_cast<int>(frame.size()); ++row)
            out << ansi::moveTo(row + 1, 1) << ansi::clearLine() << frame[static_cast<size_t>(row)];
        return out.str();
    }

    void writeVisible(std::ostringstream& out, const SessionViewport& vp) const {
        for (int i = 0; i < static_cast<int>(vp.visible.size()); ++i) {
            out << ansi::moveTo(vp.startRow + i, 1) << ansi::clearLine()
                << TuiSurface::fitRow(vp.visible[i], width_);
        }
    }

    void writeBottom(std::ostringstream& out,
                     const SessionViewport& vp,
                     const std::function<std::string(int)>& statusLine,
                     const std::string& promptLine) const {
        if (height_ >= 2) {
            out << ansi::moveTo(height_ - 1, 1) << ansi::clearLine()
                << TuiSurface::fitRow(statusLine(vp.displaySize), width_);
            out << ansi::moveTo(height_, 1) << ansi::clearLine() << TuiSurface::fitRow(promptLine, width_);
        }
    }

    int width_ = 80;
    int height_ = 24;
    std::vector<std::string> previousFrame_;
};

}  // namespace cortex::mk3::tui
