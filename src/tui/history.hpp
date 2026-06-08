// src/tui/history.hpp — Input history with up/down cycling and persistence
// MK3 TUI Framework
#pragma once

#include <string>
#include <deque>
#include <fstream>

namespace cortex::mk3::tui {

class InputHistory {
public:
    // Max entries in memory
    void setMax(int max) { max_ = max; }

    // Add a line to history (duplicate consecutive entries skipped)
    void add(const std::string& line) {
        if (line.empty()) return;
        if (!entries_.empty() && entries_.back() == line) return;
        entries_.push_back(line);
        if ((int)entries_.size() > max_) entries_.pop_front();
        pos_ = (int)entries_.size();  // reset to "after last"
        current_ = {};  // clear ephemeral buffer
    }

    // Navigate up (returns history line or empty if at top)
    std::string up(const std::string& ephemeral) {
        if (entries_.empty()) return {};
        if (pos_ == (int)entries_.size() && !ephemeral.empty()) {
            current_ = ephemeral;  // save ephemeral before navigating
        }
        if (pos_ > 0) pos_--;
        return entries_[pos_];
    }

    // Navigate down (returns next history line or ephemeral buffer)
    std::string down() {
        if (pos_ >= (int)entries_.size()) return current_;
        pos_++;
        if (pos_ == (int)entries_.size()) return current_;  // back to ephemeral
        return entries_[pos_];
    }

    // Set ephemeral buffer after up/down cycle returns to bottom
    void setEphemeral(const std::string& s) { current_ = s; }

    // Save to file
    void save(const std::string& path) {
        std::ofstream f(path);
        for (auto& e : entries_) f << e << "\n";
    }

    // Load from file
    void load(const std::string& path) {
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty()) entries_.push_back(line);
        }
        pos_ = (int)entries_.size();
    }

    void clear() { entries_.clear(); pos_ = 0; current_.clear(); }
    const std::deque<std::string>& entries() const { return entries_; }

private:
    std::deque<std::string> entries_;
    int pos_ = 0;
    int max_ = 500;
    std::string current_;   // ephemeral buffer during history nav
};

} // namespace cortex::mk3::tui
