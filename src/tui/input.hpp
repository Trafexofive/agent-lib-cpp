// src/tui/input.hpp — Raw stdin input with keybindings, history, vim bindings
// MK3 TUI Framework
#pragma once

#include "terminal.hpp"
#include "keys.hpp"
#include "history.hpp"
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <string>
#include <functional>
#include <cctype>

namespace cortex::mk3::tui {

class Input {
public:
    using Callback = std::function<void(const std::string& line)>;

    using Completer = std::function<std::vector<std::string>(const std::string& prefix)>;

    Input() { old_ = {}; }

    void start(Callback onEnter, Callback onCancel = nullptr, bool rawTerm = true) {
        onEnter_ = onEnter;
        onCancel_ = onCancel;
        escape_ = false;
        if (rawTerm) {
            tcgetattr(STDIN_FILENO, &old_);
            struct termios raw = old_;
            cfmakeraw(&raw);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 1;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        }
        running_ = true;
    }

    void stop() {
        running_ = false;
        if (old_.c_cflag != 0) tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_);
    }

    // Poll for input. Returns true if line submitted (Enter) or exit (Ctrl-D empty).
    bool poll() {
        if (!running_) return false;
        // Flush bare ESC: if we ended last poll mid-escape with no continuation
        if (escBuf_.length() == 1 && escBuf_[0] == 27) {
            struct timeval tv = {0, 50000};  // 50ms
            fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
            if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) == 0) {
                escBuf_.clear();  // bare ESC — signal cancel
                escape_ = true;
                if (onCancel_) onCancel_("");
                return false;
            }
        }
        char buf[256];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) {
            if (!escBuf_.empty() && n == 0) escBuf_.clear();
            return false;
        }

        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            std::string seq;

            // ── Escape sequence accumulation (across poll calls) ──
            if (!escBuf_.empty()) {
                escBuf_ += c;
                bool done = false;

                // CSI: ESC [ params... final
                if (escBuf_.size() >= 2 && escBuf_[1] == '[') {
                    if (escBuf_.size() >= 3 &&
                        (unsigned char)c >= 0x40 && (unsigned char)c <= 0x7e) {
                        seq = escBuf_; escBuf_.clear(); done = true;
                    } else if (escBuf_.size() > 12) { escBuf_.clear(); continue; }
                    else continue; // wait for more
                }
                // SS3: ESC O final
                else if (escBuf_.size() >= 2 && escBuf_[1] == 'O') {
                    if (escBuf_.size() == 3) { seq = escBuf_; escBuf_.clear(); done = true; }
                    else continue;
                }
                // Standard: ESC final
                else if (escBuf_.size() == 2) { seq = escBuf_; escBuf_.clear(); done = true; }
                else { escBuf_.clear(); continue; }

                if (!done) continue;
                // resolved — fall through to dispatch
            }
            // ── Start new escape ──
            else if (c == 27) { escBuf_ = "\x1b"; continue; }
            // ── Plain byte ──
            else { seq = std::string(1, c); }

            // ── Enter (submit line) ──
            if (seq == "\r") {
                if (!buffer_.empty() && buffer_.back() == '\\') {
                    buffer_.pop_back(); buffer_ += '\n'; return false; // continuation
                }
                std::string line = buffer_ + suffix_;
                if (!line.empty()) history_.add(line);
                buffer_.clear(); suffix_.clear();
                if (onEnter_) onEnter_(line);
                return true;
            }

            // ── Resolve through keymap ──
            char outChar = 0;
            KeyAction act = keymap_.resolve(seq, outChar);

            // ── Search mode ──
            if (searching() && act == KeyAction::CHAR) { extendSearch(outChar); continue; }
            if (searching() && (seq == "\x7f" || seq == "\x08")) { backspaceSearch(); continue; }
            if (searching() && (act == KeyAction::CANCEL || act == KeyAction::EXIT)) { cancelSearch(); continue; }
            if (searching() && seq == "\r") { acceptSearch(); if (onEnter_) onEnter_(buffer_); return true; }

            // ── Dispatch ──
            switch (act) {
            case KeyAction::CHAR:
                buffer_ += outChar; break;
            case KeyAction::BACKSPACE:
                if (!buffer_.empty()) buffer_.pop_back();
                break;
            case KeyAction::DELETE_FORWARD:
                if (!suffix_.empty()) suffix_ = suffix_.substr(1);
                break;
            case KeyAction::CURSOR_LEFT:
                if (!buffer_.empty()) { suffix_ = std::string(1, buffer_.back()) + suffix_; buffer_.pop_back(); } break;
            case KeyAction::CURSOR_RIGHT:
                if (!suffix_.empty()) { buffer_ += suffix_[0]; suffix_ = suffix_.substr(1); } break;
            case KeyAction::CURSOR_HOME:
                suffix_ = buffer_ + suffix_; buffer_.clear(); break;
            case KeyAction::CURSOR_END:
                buffer_ += suffix_; suffix_.clear(); break;
            case KeyAction::HISTORY_UP: {
                std::string h = history_.up(buffer_ + suffix_);
                if (!h.empty()) { buffer_ = h; suffix_.clear(); } break; }
            case KeyAction::HISTORY_DOWN: {
                std::string h = history_.down();
                if (!h.empty()) { buffer_ = h; suffix_.clear(); } break; }
            case KeyAction::KILL_WORD: {
                std::string killed;
                while (!buffer_.empty() && buffer_.back() == ' ') { killed = buffer_.back() + killed; buffer_.pop_back(); }
                while (!buffer_.empty() && buffer_.back() != ' ') { killed = buffer_.back() + killed; buffer_.pop_back(); }
                if (!killed.empty()) killBuf_ = killed;
                break;
            }
            case KeyAction::KILL_TO_START:
                if (!buffer_.empty()) { killBuf_ = buffer_; buffer_.clear(); }
                break;
            case KeyAction::KILL_TO_END:
                if (!suffix_.empty()) { killBuf_ = suffix_; suffix_.clear(); }
                break;
            case KeyAction::YANK:
                if (!killBuf_.empty()) buffer_ += killBuf_;
                break;
            case KeyAction::WORD_LEFT:
                while (!buffer_.empty() && buffer_.back() == ' ') { suffix_ = std::string(1, buffer_.back()) + suffix_; buffer_.pop_back(); }
                while (!buffer_.empty() && buffer_.back() != ' ') { suffix_ = std::string(1, buffer_.back()) + suffix_; buffer_.pop_back(); }
                break;
            case KeyAction::WORD_RIGHT:
                while (!suffix_.empty() && suffix_[0] == ' ') { buffer_ += suffix_[0]; suffix_ = suffix_.substr(1); }
                while (!suffix_.empty() && suffix_[0] != ' ') { buffer_ += suffix_[0]; suffix_ = suffix_.substr(1); }
                break;
            case KeyAction::SCROLL_UP:
                if (scrollUp) scrollUp();
                break;
            case KeyAction::SCROLL_DOWN:
                if (scrollDown) scrollDown();
                break;
            case KeyAction::CLEAR_SCREEN:
                if (clearScreen) clearScreen();
                break;
            case KeyAction::SEARCH:
                startSearch(); break;
            case KeyAction::TAB: {
                if (!buffer_.empty() && completer_) {
                    auto matches = completer_(buffer_);
                    for (auto& m : matches) {
                        if (m != buffer_) { buffer_ = m; suffix_.clear(); break; }
                    }
                } break; }
            case KeyAction::CANCEL:
                if (onCancel_) onCancel_("");
                buffer_.clear(); suffix_.clear();
                break;
            case KeyAction::EXIT:
                if (buffer_.empty() && suffix_.empty()) { if (onEnter_) onEnter_("/exit"); return true; }
                if (!suffix_.empty()) suffix_ = suffix_.substr(1);
                break;
            default: break;
            }
        }
        return false;
    }

    std::string line() const { return buffer_ + suffix_; }
    size_t cursorPos() const { return buffer_.size(); }
    bool running() const { return running_; }
    bool escapePressed() const { return escape_; }
    void clearEscape() { escape_ = false; }
    InputHistory& history() { return history_; }

    // ── Search ──
    bool searching() const { return !searchBuf_.empty(); }
    std::string searchLine() const { return "(reverse-search)`'" + searchPattern_ + "': " + searchMatch_; }
    void startSearch() {
        if (!searchBuf_.empty()) { cycleSearch(); return; }
        searchBuf_ = buffer_ + suffix_;
        searchPattern_.clear(); searchPos_ = -1; updateSearch();
    }
    void extendSearch(char c) { if (!searching()) return; searchPattern_ += c; searchPos_ = -1; updateSearch(); }
    void backspaceSearch() { if (!searching()) return; if (!searchPattern_.empty()) searchPattern_.pop_back(); searchPos_ = -1; updateSearch(); }
    void cycleSearch() { if (!searching()) return; searchPos_++; updateSearch(); }
    void cancelSearch() { if (searching()) { buffer_ = searchBuf_; suffix_.clear(); } searchBuf_.clear(); searchPattern_.clear(); searchMatch_.clear(); searchPos_ = -1; }
    void acceptSearch() { if (!searching()) return; if (!searchMatch_.empty()) buffer_ = searchMatch_; suffix_.clear(); searchBuf_.clear(); searchPattern_.clear(); searchMatch_.clear(); searchPos_ = -1; }
    void updateSearch() {
        searchMatch_.clear(); if (searchPattern_.empty()) return;
        auto& entries = history_.entries(); std::string lower = searchPattern_;
        for (auto& c : lower) c = std::tolower(c);
        int start = (searchPos_ >= 0 && searchPos_ < (int)entries.size()) ? searchPos_ : (int)entries.size() - 1;
        for (int i = start; i >= 0; i--) {
            std::string el = entries[i]; for (auto& c : el) c = std::tolower(c);
            if (el.find(lower) != std::string::npos) { searchMatch_ = entries[i]; searchPos_ = i; return; }
        }
        searchPos_ = -1;
    }

    void setCompleter(Completer c) { completer_ = std::move(c); }

    // Scroll callbacks
    std::function<void()> scrollUp;
    std::function<void()> scrollDown;
    std::function<void()> clearScreen;

private:
    struct termios old_ = {};
    bool running_ = false;
    bool escape_ = false;
    std::string buffer_, suffix_, escBuf_, killBuf_;
    std::string searchBuf_, searchPattern_, searchMatch_;
    int searchPos_ = -1;
    Callback onEnter_, onCancel_;
    Completer completer_;
    KeyMap keymap_;
    InputHistory history_;
};

} // namespace cortex::mk3::tui
