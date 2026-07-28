// src/tui/surface.hpp — ANSI-aware 2D row surface + diff renderer
// Lightweight terminal "engine" primitives: clamp, compose, viewport, diff.
#pragma once

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "terminal.hpp"
#include "width.hpp"

namespace cortex::mk3::tui {

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool empty() const { return w <= 0 || h <= 0; }
};

// Row-oriented surface. It preserves ANSI spans within rows and guarantees rows
// fit the requested visible width. Terminal text is not a true pixel grid — this
// intentionally treats rows as the compositing primitive so color escapes remain
// stable and cheap.
class TuiSurface {
   public:
    TuiSurface(int width = 80, int height = 0) { resize(width, height); }

    void resize(int width, int height = 0) {
        width_ = std::max(1, width);
        rows_.assign(std::max(0, height), blankRow());
    }

    int width() const { return width_; }
    int height() const { return static_cast<int>(rows_.size()); }
    const std::vector<std::string>& rows() const { return rows_; }

    void clear(int height = 0) { rows_.assign(std::max(0, height), blankRow()); }

    void fillRow(int y, const std::string& style = "") {
        ensureHeight(y + 1);
        rows_[y] = padRight(style, width_);
    }

    void drawLine(int y, const std::string& line) {
        ensureHeight(y + 1);
        rows_[y] = fitRow(line, width_);
    }

    void appendLine(const std::string& line) { rows_.push_back(fitRow(line, width_)); }

    void appendLines(const std::vector<std::string>& lines) {
        for (const auto& line : lines)
            appendLine(line);
    }

    void blitRows(int y, const std::vector<std::string>& lines) {
        ensureHeight(y + static_cast<int>(lines.size()));
        for (size_t i = 0; i < lines.size(); ++i)
            rows_[y + static_cast<int>(i)] = fitRow(lines[i], width_);
    }

    std::vector<std::string> viewport(int start, int count) const {
        std::vector<std::string> out;
        if (count <= 0)
            return out;
        start = std::clamp(start, 0, height());
        int end = std::min(height(), start + count);
        out.reserve(std::max(0, end - start));
        for (int i = start; i < end; ++i)
            out.push_back(rows_[i]);
        return out;
    }

    static std::string fitRow(const std::string& line, int width) {
        width = std::max(1, width);
        std::string out;
        out.reserve(line.size() + 16);
        int used = 0;

        for (size_t i = 0; i < line.size();) {
            unsigned char c = static_cast<unsigned char>(line[i]);
            if (c == 0x1b) {
                const size_t start = i++;
                if (i < line.size() && line[i] == '[') {
                    ++i;
                    while (i < line.size()) {
                        unsigned char ec = static_cast<unsigned char>(line[i++]);
                        if (ec >= 0x40 && ec <= 0x7e)
                            break;
                    }
                }
                out.append(line, start, i - start);
                continue;
            }

            size_t before = i;
            uint32_t cp = readUtf8(line, i);
            int cw = isWideCodepoint(cp) ? 2 : 1;
            if (used + cw > width)
                break;
            out.append(line, before, i - before);
            used += cw;
        }

        if (used < width)
            out.append(static_cast<size_t>(width - used), ' ');
        out += ansi::reset();
        return out;
    }

    static std::vector<int> dirtyRows(const std::vector<std::string>& previous,
                                      const std::vector<std::string>& current) {
        std::vector<int> dirty;
        const size_t n = std::max(previous.size(), current.size());
        for (size_t i = 0; i < n; ++i) {
            const bool changed = i >= previous.size() || i >= current.size() || previous[i] != current[i];
            if (changed)
                dirty.push_back(static_cast<int>(i));
        }
        return dirty;
    }

    static std::string renderDiff(const std::vector<std::string>& previous,
                                  const std::vector<std::string>& current, int startRow = 1) {
        std::ostringstream out;
        for (int row : dirtyRows(previous, current)) {
            out << ansi::moveTo(startRow + row, 1) << ansi::clearLine();
            if (row >= 0 && row < static_cast<int>(current.size()))
                out << current[static_cast<size_t>(row)];
        }
        return out.str();
    }

   private:
    void ensureHeight(int h) {
        if (h > height())
            rows_.resize(static_cast<size_t>(h), blankRow());
    }

    std::string blankRow() const { return padRight("", width_); }

    int width_ = 80;
    std::vector<std::string> rows_;
};

}  // namespace cortex::mk3::tui
