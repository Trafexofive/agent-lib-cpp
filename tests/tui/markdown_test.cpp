// tests/tui/markdown_test.cpp — T3: Markdown renderer tests

#include "../../src/tui/components/markdown.hpp"
#include "../../src/tui/terminal.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace cortex::mk3::tui;

// ── Helper: strip ANSI escape sequences for content validation ──
static std::string stripAnsi(const std::string& s) {
    std::string out;
    bool inEscape = false;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\033') {
            inEscape = true;
            continue;
        }
        if (inEscape) {
            if (s[i] == 'm') inEscape = false;
            continue;
        }
        out += s[i];
    }
    return out;
}

// ── Helper: check if string contains ANSI escape ──
static bool containsAnsi(const std::string& s) {
    return s.find("\033[") != std::string::npos;
}

int main() {
    int failures = 0;
    auto check = [&](bool cond, const std::string& test) {
        if (!cond) { std::cerr << "FAIL: " << test << "\n"; failures++; }
        else { std::cout << "  OK: " << test << "\n"; }
    };

    std::cout << "═══ Markdown Renderer Tests ═══\n\n";

    Markdown md;
    md.setWidth(80);

    // ── T3.1: Bold text ──
    {
        md.setText("**bold text**");
        auto lines = md.render();
        check(lines.size() >= 1, "bold produces at least 1 line");
        if (!lines.empty()) {
            check(containsAnsi(lines[0]), "bold output contains ANSI styling");
            check(stripAnsi(lines[0]) == "bold text", "bold content preserved after strip");
        }
    }

    // ── T3.2: Italic text ──
    {
        md.setText("*italic text*");
        auto lines = md.render();
        if (!lines.empty()) {
            check(stripAnsi(lines[0]) == "italic text", "italic content preserved");
        }
    }

    // ── T3.3: Inline code ──
    {
        md.setText("use `printf()` here");
        auto lines = md.render();
        if (!lines.empty()) {
            std::string stripped = stripAnsi(lines[0]);
            check(stripped.find("printf()") != std::string::npos, "inline code content preserved");
        }
    }

    // ── T3.4: Headings ──
    {
        md.setText("# Heading 1\n## Heading 2\n### Heading 3");
        auto lines = md.render();
        check(lines.size() >= 3, "headings produce 3 lines");
        if (lines.size() >= 3) {
            check(stripAnsi(lines[0]).find("Heading 1") != std::string::npos, "h1 content");
            check(stripAnsi(lines[1]).find("Heading 2") != std::string::npos, "h2 content");
            check(stripAnsi(lines[2]).find("Heading 3") != std::string::npos, "h3 content");
        }
    }

    // ── T3.5: Unordered list ──
    {
        md.setText("- Item A\n- Item B\n- Item C");
        auto lines = md.render();
        check(lines.size() >= 3, "list produces at least 3 lines");
        if (lines.size() >= 3) {
            check(stripAnsi(lines[0]).find("Item A") != std::string::npos, "list item A");
            check(stripAnsi(lines[1]).find("Item B") != std::string::npos, "list item B");
            check(stripAnsi(lines[2]).find("Item C") != std::string::npos, "list item C");
        }
    }

    // ── T3.6: Width-aware wrapping ──
    {
        md.setWidth(20);
        md.setText("This is a very long sentence that should wrap across multiple lines.");
        auto lines = md.render();
        check(lines.size() > 1, "narrow width causes wrapping (got " + std::to_string(lines.size()) + " lines)");
        for (auto& line : lines) {
            std::string stripped = stripAnsi(line);
            // Lines should not exceed width (accounting for ANSI codes)
            check(stripped.size() <= 20 + 2, "line within width: " + std::to_string(stripped.size()) + " <= 22");
        }
    }

    // ── T3.7: setText() incremental — matches full render ──
    {
        Markdown incremental;
        incremental.setWidth(80);
        incremental.setText("The");
        incremental.setText("The quick");
        incremental.setText("The quick **brown**");
        incremental.setText("The quick **brown** fox");
        auto incLines = incremental.render();

        Markdown full;
        full.setWidth(80);
        full.setText("The quick **brown** fox");
        auto fullLines = full.render();

        check(incLines == fullLines, "incremental setText matches full setText");
    }

    // ── T3.8: Empty input ──
    {
        md.setText("");
        auto lines = md.render();
        check(lines.empty() || (lines.size() == 1 && lines[0].empty()), "empty input produces no output");
    }

    // ── T3.9: Very long single line (stress test) ──
    {
        md.setWidth(40);
        std::string longText;
        for (int i = 0; i < 100; i++) longText += "word ";
        md.setText(longText);
        auto lines = md.render();
        check(lines.size() > 5, "long text produces many lines (got " + std::to_string(lines.size()) + ")");
    }

    std::cout << "\n═══ " << (failures ? "FAILED" : "ALL PASSED")
              << " (" << failures << " failures) ═══\n";
    return failures ? 1 : 0;
}
