// tests/tui/grid_test.cpp — GridLayout test suite
#include "../../src/tui/grid.hpp"
#include "../../src/tui/terminal.hpp"
#include <cassert>
#include <iostream>
using namespace cortex::mk3::tui;

int main() {
    int failures = 0;
    auto check = [&](bool c, const char* msg) {
        if (!c) { std::cerr << "FAIL: " << msg << "\n"; failures++; }
        else std::cout << "  OK: " << msg << "\n";
    };

    std::cout << "═══ GridLayout ═══\n\n";

    // ── Add cells ──
    {
        GridLayout g(80, 24);
        g.addCell("header", 0, 0, 80, 1);
        g.addCell("body", 2, 0, 80, 20);
        g.addCell("status", 23, 0, 80, 1);
        check(g.cellCount() == 3, "addCell: 3 cells");
        check(g.findCell("body") != nullptr, "findCell: body exists");
        check(g.findCell("none") == nullptr, "findCell: none returns null");
    }

    // ── Set content ──
    {
        GridLayout g(80, 24);
        g.addCell("test", 5, 10, 30, 2);
        g.setContent("test", {"Hello", "World"});
        auto* cell = g.findCell("test");
        check(cell->content.size() == 2, "setContent: 2 lines");
        check(cell->content[0] == "Hello", "setContent: line 0 correct");
    }

    // ── Diff rendering: first render generates full output ──
    {
        GridLayout g(80, 24);
        g.addCell("a", 0, 0, 40, 1);
        g.setContent("a", {"first"});
        std::string out = g.renderDiff();
        check(!out.empty(), "renderDiff: produces output on first render");
        check(out.find("first") != std::string::npos, "renderDiff: content visible");
    }

    // ── Diff rendering: unchanged cells produce no output ──
    {
        GridLayout g(80, 24);
        g.addCell("a", 0, 0, 40, 1);
        g.setContent("a", {"hello"});
        g.renderDiff(); // first render
        std::string out = g.renderDiff();
        check(out.empty(), "renderDiff: no output on unchanged cell");
    }

    // ── Diff rendering: changed cells produce output ──
    {
        GridLayout g(80, 24);
        g.addCell("a", 0, 0, 40, 1);
        g.setContent("a", {"hello"});
        g.renderDiff();
        g.setContent("a", {"world"});
        std::string out = g.renderDiff();
        check(!out.empty(), "renderDiff: produces output on change");
        check(out.find("world") != std::string::npos, "renderDiff: new content visible");
    }

    // ── Full render always produces output ──
    {
        GridLayout g(80, 24);
        g.addCell("a", 0, 0, 40, 1);
        g.setContent("a", {"hello"});
        std::string out1 = g.renderFull();
        std::string out2 = g.renderFull();
        check(!out1.empty(), "renderFull: produces output");
        check(!out2.empty(), "renderFull: always produces output (no diff cache)");
        check(out1 == out2, "renderFull: identical outputs on same state");
    }

    // ── Content truncation to cell width ──
    {
        GridLayout g(80, 24);
        g.addCell("a", 0, 0, 10, 1);
        g.setContent("a", {"this is way too long text here"});
        std::string out = g.renderDiff();
        // The rendered content should be ~10 chars (cell width)
        check(out.find("this is wa") != std::string::npos, "truncate: content truncated to cell width");
    }

    // ── Cell height enforcement ──
    {
        GridLayout g(80, 24);
        g.addCell("a", 0, 0, 40, 2);
        g.setContent("a", {"line1", "line2", "line3"});
        auto* cell = g.findCell("a");
        // Content has 3 lines but cell height is 2
        std::string out = g.renderDiff();
        // Should only render 2 lines
        check(out.find("line3") == std::string::npos, "height: line3 not rendered (exceeds height)");
    }

    // ── Clear resets everything ──
    {
        GridLayout g(80, 24);
        g.addCell("a", 0, 0, 40, 1);
        g.setContent("a", {"hello"});
        g.renderDiff();
        g.clear();
        check(g.cellCount() == 0, "clear: no cells");
        g.addCell("b", 0, 0, 40, 1);
        g.setContent("b", {"fresh"});
        std::string out = g.renderDiff();
        check(!out.empty(), "clear: renders fresh after clear");
    }

    // ── Resize invalidates snapshot ──
    {
        GridLayout g(80, 24);
        g.addCell("a", 0, 0, 40, 1);
        g.setContent("a", {"hello"});
        g.renderDiff();
        g.resize(100, 30);
        std::string out = g.renderDiff();
        check(!out.empty(), "resize: re-renders after resize");
    }

    // ── Multiple cells ──
    {
        GridLayout g(80, 24);
        g.addCell("a", 0, 0, 40, 1);
        g.addCell("b", 0, 40, 40, 1);
        g.setContent("a", {"left"});
        g.setContent("b", {"right"});
        std::string out = g.renderDiff();
        check(out.find("left") != std::string::npos, "multi: left visible");
        check(out.find("right") != std::string::npos, "multi: right visible");
    }

    // ── Only changed cell renders in diff mode ──
    {
        GridLayout g(80, 24);
        g.addCell("a", 0, 0, 40, 1);
        g.addCell("b", 0, 40, 40, 1);
        g.setContent("a", {"left"});
        g.setContent("b", {"right"});
        g.renderDiff();
        g.setContent("a", {"changed"});
        std::string out = g.renderDiff();
        check(out.find("changed") != std::string::npos, "diff-multi: changed cell rendered");
        check(out.find("right") == std::string::npos, "diff-multi: unchanged cell NOT rendered");
    }

    std::cout << "\n═══ " << (failures ? "FAILED" : "ALL PASSED") << " (" << failures << " failures) ═══\n";
    return failures;
}
