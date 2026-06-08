// src/tui/keys.hpp — Key binding abstraction
// MK3 TUI Framework
#pragma once

#include <string>
#include <vector>
#include <unordered_map>

namespace cortex::mk3::tui {

enum class KeyAction {
    NONE,
    ENTER,
    BACKSPACE,
    DELETE_FORWARD,
    CURSOR_LEFT,
    CURSOR_RIGHT,
    CURSOR_HOME,
    CURSOR_END,
    HISTORY_UP,
    HISTORY_DOWN,
    KILL_WORD,
    KILL_TO_START,
    KILL_TO_END,
    YANK,
    WORD_LEFT,    // Alt-B / ESC b
    WORD_RIGHT,   // Alt-F / ESC f
    TAB,
    CANCEL,       // Ctrl-C
    EXIT,         // Ctrl-D on empty
    SCROLL_UP,    // scroll history up
    SCROLL_DOWN,  // scroll history down
    SEARCH,       // Ctrl-R history search
    CLEAR_SCREEN, // Ctrl-L
    PASTE,        // special
    CHAR,         // regular character
};

struct KeyBinding {
    std::string seq;       // escape sequence or raw byte
    KeyAction action;
    // For CHAR: set from the byte; for others: this is the action
};

class KeyMap {
public:
    KeyMap() { initDefaults(); }

    // Look up a single byte or escape sequence → action
    KeyAction resolve(const std::string& seq, char& outChar) const {
        auto it = bindings_.find(seq);
        if (it != bindings_.end()) return it->second;
        // Single printable byte → CHAR
        if (seq.size() == 1 && (unsigned char)seq[0] >= 32 && (unsigned char)seq[0] < 127) {
            outChar = seq[0];
            return KeyAction::CHAR;
        }
        return KeyAction::NONE;
    }

    // Custom binding
    void bind(const std::string& seq, KeyAction action) {
        bindings_[seq] = action;
    }

private:
    std::unordered_map<std::string, KeyAction> bindings_;

    void initDefaults() {
        // Printable handled by resolve(), these are special keys

        // GNU readline-style control characters
        bindings_["\x03"] = KeyAction::CANCEL;         // Ctrl-C
        bindings_["\x04"] = KeyAction::EXIT;           // Ctrl-D (EOF empty, delete-char otherwise)
        bindings_["\x01"] = KeyAction::CURSOR_HOME;    // Ctrl-A
        bindings_["\x05"] = KeyAction::CURSOR_END;     // Ctrl-E
        bindings_["\x02"] = KeyAction::CURSOR_LEFT;    // Ctrl-B
        bindings_["\x06"] = KeyAction::CURSOR_RIGHT;   // Ctrl-F
        bindings_["\x08"] = KeyAction::BACKSPACE;      // Ctrl-H / Backspace
        bindings_["\x7f"] = KeyAction::BACKSPACE;      // DEL / Backspace
        bindings_["\x0b"] = KeyAction::KILL_TO_END;    // Ctrl-K
        bindings_["\x15"] = KeyAction::KILL_TO_START;  // Ctrl-U
        bindings_["\x17"] = KeyAction::KILL_WORD;      // Ctrl-W
        bindings_["\x19"] = KeyAction::YANK;           // Ctrl-Y
        bindings_["\x0c"] = KeyAction::CLEAR_SCREEN;   // Ctrl-L
        bindings_["\x09"] = KeyAction::TAB;            // Tab
        bindings_["\r"]   = KeyAction::ENTER;          // Enter

        // Arrow keys (ANSI — both CSI and SS3 variants)
        bindings_["\x1b[A"] = KeyAction::HISTORY_UP;
        bindings_["\x1b[B"] = KeyAction::HISTORY_DOWN;
        bindings_["\x1b[C"] = KeyAction::CURSOR_RIGHT;
        bindings_["\x1b[D"] = KeyAction::CURSOR_LEFT;
        bindings_["\x1b[H"] = KeyAction::CURSOR_HOME;
        bindings_["\x1b[F"] = KeyAction::CURSOR_END;
        // SS3 variant (some terminals send ESC O instead of ESC [)
        bindings_["\x1bOA"] = KeyAction::HISTORY_UP;
        bindings_["\x1bOB"] = KeyAction::HISTORY_DOWN;
        bindings_["\x1bOC"] = KeyAction::CURSOR_RIGHT;
        bindings_["\x1bOD"] = KeyAction::CURSOR_LEFT;
        // Scroll (Ctrl-O — reliable raw byte)
        bindings_["\x0f"] = KeyAction::SCROLL_UP;    // Ctrl-O
        // Page keys
        bindings_["\x1b[5~"] = KeyAction::SCROLL_UP;   // PageUp
        bindings_["\x1b[6~"] = KeyAction::SCROLL_DOWN; // PageDown
        bindings_["\x12"] = KeyAction::SEARCH;        // Ctrl-R
        bindings_["\x1b" "b"] = KeyAction::WORD_LEFT;   // Alt-B
        bindings_["\x1b" "f"] = KeyAction::WORD_RIGHT;  // Alt-F
        bindings_["\x1b[1;5D"] = KeyAction::WORD_LEFT;   // Ctrl+Left
        bindings_["\x1b[1;5C"] = KeyAction::WORD_RIGHT;  // Ctrl+Right

        // History navigation (readline)
        bindings_["\x10"] = KeyAction::HISTORY_UP;    // Ctrl-P
        bindings_["\x0e"] = KeyAction::HISTORY_DOWN;  // Ctrl-N
    }
};

} // namespace cortex::mk3::tui
