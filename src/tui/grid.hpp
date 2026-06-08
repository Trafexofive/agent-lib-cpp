// src/tui/grid.hpp — GridLayout: 2D cell placement + diff-based rendering
// MK3 TUI Framework
#pragma once

#include "terminal.hpp"
#include <string>
#include <vector>
#include <map>
#include <functional>

namespace cortex::mk3::tui {

// ── GridLayout: manages a resizable 2D grid of cells ──
// Cells are referenced by id. Layout is recalculated on render.
// Diff tracking: only redraws cells whose content changed.
class GridLayout {
public:
    struct Cell {
        std::string id;
        int row = 0, col = 0;       // grid position
        int width = 0, height = 1;  // size in cols/rows
        std::vector<std::string> content;  // lines to render
        bool visible = true;
    };

    GridLayout(int termWidth = 80, int termHeight = 24)
        : width_(termWidth), height_(termHeight) {}

    // ── Cell management ──
    Cell& addCell(const std::string& id, int row, int col, int w, int h) {
        cells_.push_back({id, row, col, w, std::max(1, h), {}, true});
        return cells_.back();
    }

    Cell* findCell(const std::string& id) {
        for (auto& c : cells_) if (c.id == id) return &c;
        return nullptr;
    }

    void setContent(const std::string& id, const std::vector<std::string>& lines) {
        auto* cell = findCell(id);
        if (cell) { cell->content = lines; cell->height = std::max(1, (int)lines.size()); }
    }

    void clear() { cells_.clear(); snapshot_.clear(); }

    // ── ANSI-aware cell content fitter ──
    // Truncates or pads to exact visible width, closing any open escape sequences.
    static std::string fitToWidth(const std::string& s, int w) {
        size_t vis = 0, cutAt = s.size();
        bool esc = false;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '\033') { esc = true; continue; }
            if (esc) { if (s[i] == 'm') esc = false; continue; }
            vis++;
            if ((int)vis == w) { cutAt = i + 1; break; }
        }
        std::string result = s.substr(0, cutAt);
        // Close any open styling at truncation point
        if (cutAt < s.size()) result += "\033[0m";
        size_t padLen = ((int)vis < w) ? (size_t)(w - (int)vis) : 0;
        return result + std::string(padLen, ' ');
    }

    // ── Diff-based render ──
    // Returns ANSI escape sequences to redraw only changed cells.
    // Compare current content against previous snapshot.
    std::string renderDiff() {
        std::string output;
        output += "\033[0;0H";  // always start from top-left
        for (auto& cell : cells_) {
            if (!cell.visible) continue;
            auto it = snapshot_.find(cell.id);
            bool same = (it != snapshot_.end()) && (it->second == cell.content);
            if (same) continue;

            // Move to cell position
            output += ansi::moveTo(cell.row + 1, cell.col + 1);
            // Render cell content
            for (size_t i = 0; i < (size_t)cell.height && i < cell.content.size(); i++) {
                if (i > 0) output += ansi::moveTo(cell.row + 1 + (int)i, cell.col + 1);
                std::string line = fitToWidth(cell.content[i], cell.width);
                output += line;
                output += "\033[0K"; // clear to end of line
            }
            // Clear remaining rows if cell shrank
            if (it != snapshot_.end()) {
                int oldH = (int)it->second.size();
                for (int r = (int)cell.content.size(); r < oldH && r < cell.height; r++) {
                    output += ansi::moveTo(cell.row + 1 + r, cell.col + 1);
                    output += std::string(cell.width, ' ');
                }
            }
        }
        // Save snapshot
        snapshot_.clear();
        for (auto& cell : cells_) snapshot_[cell.id] = cell.content;
        return output;
    }

    // ── Full render (no diff) ──
    std::string renderFull() {
        snapshot_.clear();
        std::string output;
        output += "\033[0;0H";  // always start from top-left
        for (auto& cell : cells_) {
            if (!cell.visible) continue;
            output += ansi::moveTo(cell.row + 1, cell.col + 1);
            for (size_t i = 0; i < (size_t)cell.height && i < cell.content.size(); i++) {
                if (i > 0) output += ansi::moveTo(cell.row + 1 + (int)i, cell.col + 1);
                std::string line = fitToWidth(cell.content[i], cell.width);
                output += line;
                output += "\033[0K";
            }
            snapshot_[cell.id] = cell.content;
        }
        return output;
    }

    void resize(int w, int h) { width_ = w; height_ = h; snapshot_.clear(); }

    int width() const { return width_; }
    int height() const { return height_; }
    size_t cellCount() const { return cells_.size(); }

private:
    int width_, height_;
    std::vector<Cell> cells_;
    std::map<std::string, std::vector<std::string>> snapshot_;
};

} // namespace cortex::mk3::tui
