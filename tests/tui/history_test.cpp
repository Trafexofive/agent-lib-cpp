// tests/tui/history_test.cpp — InputHistory tests
#include "../../src/tui/history.hpp"
#include <cassert>
#include <iostream>
using namespace cortex::mk3::tui;

int main() {
    int failures = 0;
    auto check = [&](bool c, const char* m) {
        if (!c) { std::cerr << "FAIL: " << m << "\n"; failures++; }
        else std::cout << "  OK: " << m << "\n";
    };

    std::cout << "═══ InputHistory ═══\n\n";

    InputHistory h;
    check(h.up("x") == "", "empty: up returns empty");
    check(h.down() == "", "empty: down returns empty");

    h.add("cmd1");
    h.add("cmd2");
    h.add("cmd3");

    // Up navigation
    check(h.up("ephemeral") == "cmd3", "up from end → cmd3");
    check(h.up("") == "cmd2", "up again → cmd2");
    check(h.up("") == "cmd1", "up again → cmd1");
    check(h.up("") == "cmd1", "up at top → stays cmd1");

    // Down navigation
    check(h.down() == "cmd2", "down → cmd2");
    check(h.down() == "cmd3", "down → cmd3");
    check(h.down() == "ephemeral", "down past end → ephemeral");
    check(h.down() == "ephemeral", "down still → ephemeral");

    // Duplicate suppression
    h.add("cmd3");
    check(h.down() == "ephemeral", "duplicate not added");

    // Save/load
    h.save("/tmp/mk3_hist_test");
    InputHistory h2;
    h2.load("/tmp/mk3_hist_test");
    check(h2.up("e") == "cmd3", "load + up → cmd3");
    check(h2.up("") == "cmd2", "load up → cmd2");

    std::cout << "\n═══ " << (failures ? "FAILED" : "ALL PASSED") << " (" << failures << " fails) ═══\n";
    return failures;
}
