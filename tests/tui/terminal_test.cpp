// tests/tui/terminal_test.cpp — T1: Terminal ANSI escape sequence tests

#include "../../src/tui/terminal.hpp"
#include <cassert>
#include <iostream>
#include <string>

using namespace cortex::mk3::tui;

int main() {
    int failures = 0;
    auto check = [&](bool cond, const std::string& test) {
        if (!cond) { std::cerr << "FAIL: " << test << "\n"; failures++; }
        else { std::cout << "  OK: " << test << "\n"; }
    };

    std::cout << "═══ Terminal ANSI Tests ═══\n\n";

    // ── T1.1: Cursor visibility ──
    {
        check(ansi::hideCursor() == "\033[?25l", "hideCursor escape");
        check(ansi::showCursor() == "\033[?25h", "showCursor escape");
    }

    // ── T1.2: Alternate screen ──
    {
        check(ansi::altScreenOn() == "\033[?1049h", "altScreenOn escape");
        check(ansi::altScreenOff() == "\033[?1049l", "altScreenOff escape");
    }

    // ── T1.3: Clear operations ──
    {
        check(ansi::clearScreen() == "\033[2J", "clearScreen escape");
        check(ansi::clearLine() == "\033[2K", "clearLine escape");
        check(ansi::clearToEnd() == "\033[0J", "clearToEnd escape");
    }

    // ── T1.4: Cursor positioning ──
    {
        check(ansi::moveTo(1, 1) == "\033[1;1H", "moveTo(1,1)");
        check(ansi::moveTo(10, 5) == "\033[10;5H", "moveTo(10,5)");
        check(ansi::moveTo(0, 0) == "\033[0;0H", "moveTo(0,0)");
        check(ansi::moveUp(3) == "\033[3A", "moveUp(3)");
        check(ansi::moveDown(1) == "\033[1B", "moveDown(1)");
        check(ansi::moveRight(5) == "\033[5C", "moveRight(5)");
        check(ansi::moveLeft(2) == "\033[2D", "moveLeft(2)");
    }

    // ── T1.5: True color foreground ──
    {
        check(ansi::fg(255, 0, 0) == "\033[38;2;255;0;0m", "fg red");
        check(ansi::fg(0, 255, 0) == "\033[38;2;0;255;0m", "fg green");
        check(ansi::fg(0, 0, 255) == "\033[38;2;0;0;255m", "fg blue");
    }

    // ── T1.6: True color background ──
    {
        check(ansi::bg(128, 128, 128) == "\033[48;2;128;128;128m", "bg gray");
    }

    // ── T1.7: Text styling ──
    {
        check(ansi::reset() == "\033[0m", "reset");
        check(ansi::bold() == "\033[1m", "bold");
        check(ansi::italic() == "\033[3m", "italic");
        check(ansi::underline() == "\033[4m", "underline");
        check(ansi::strikethrough() == "\033[9m", "strikethrough");
        check(ansi::dim() == "\033[2m", "dim");
    }

    // ── T1.8: Composite — styled string ──
    {
        std::string s = ansi::bold() + ansi::fg(255, 100, 0) + "WARNING" + ansi::reset();
        std::string expected = "\033[1m\033[38;2;255;100;0mWARNING\033[0m";
        check(s == expected, "composite bold+orange+reset");
    }

    // ── T1.9: moveTo handles edge cases (large values) ──
    {
        // Shouldn't crash, just produce the escape
        std::string s = ansi::moveTo(999, 999);
        check(!s.empty() && s.find("999") != std::string::npos, "moveTo large values");
    }

    std::cout << "\n═══ " << (failures ? "FAILED" : "ALL PASSED")
              << " (" << failures << " failures) ═══\n";
    return failures ? 1 : 0;
}
