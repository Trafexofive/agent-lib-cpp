// tests/tui/input_test.cpp — basic unit checks (not stdin pipe)
#include "../../src/tui/input.hpp"
#include "../../src/tui/history.hpp"
#include "../../src/tui/keys.hpp"
#include <cassert>
#include <iostream>
using namespace cortex::mk3::tui;

int main() {
    int fail = 0;
    auto ok = [&](bool c, const char* m) {
        if (!c) { std::cerr << "FAIL: " << m << "\n"; fail++; }
        else std::cout << "  OK: " << m << "\n";
    };
    std::cout << "═══ Input (v2 unit) ═══\n\n";

    // History
    InputHistory h;
    h.add("one"); h.add("two"); h.add("three");
    ok(h.up("ephemeral") == "three", "history up 1");
    ok(h.up("") == "two", "history up 2");
    ok(h.up("") == "one", "history up 3");
    ok(h.down() == "two", "history down 1");
    ok(h.down() == "three", "history down 2");
    h.save("/tmp/mk3_test_hist");
    InputHistory h2; h2.load("/tmp/mk3_test_hist");
    ok(h2.up("e") == "three", "history load");

    // KeyMap
    KeyMap km;
    char out = 0;
    ok(km.resolve("\x08", out) == KeyAction::CURSOR_LEFT,  "Ctrl-H → left");
    ok(km.resolve("\x0a", out) == KeyAction::HISTORY_DOWN,  "Ctrl-J → down");
    ok(km.resolve("\x0b", out) == KeyAction::HISTORY_UP,    "Ctrl-K → up");
    ok(km.resolve("\x0c", out) == KeyAction::CURSOR_RIGHT, "Ctrl-L → right");
    ok(km.resolve("h", out) == KeyAction::CHAR && out == 'h', "plain h is char");
    ok(km.resolve("j", out) == KeyAction::CHAR && out == 'j', "plain j is char");
    ok(km.resolve("\x1b[A", out) == KeyAction::HISTORY_UP, "ANSI up");
    ok(km.resolve("\x1b[B", out) == KeyAction::HISTORY_DOWN, "ANSI down");
    ok(km.resolve("\x1b[C", out) == KeyAction::CURSOR_RIGHT, "ANSI right");
    ok(km.resolve("\x1b[D", out) == KeyAction::CURSOR_LEFT, "ANSI left");
    ok(km.resolve("x", out) == KeyAction::CHAR && out == 'x', "char x");
    ok(km.resolve("\x7f", out) == KeyAction::BACKSPACE, "DEL");
    ok(km.resolve("\x17", out) == KeyAction::KILL_WORD, "Ctrl-W");

    std::cout << "\n═══ " << (fail ? "FAILED" : "ALL PASSED") << " (" << fail << " fails) ═══\n";
    return fail;
}
