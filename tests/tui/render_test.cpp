// tests/tui/render_test.cpp — TUI renderer/grid/protocol smoke tests

#include "../../src/tui/grid.hpp"
#include "../../src/tui/components/protocol.hpp"
#include "../../src/tui/components/markdown.hpp"
#include "../../src/tui/renderer.hpp"
#include <iostream>
#include <string>
#include <vector>

using namespace cortex::mk3::tui;

static int failures = 0;

static void check(bool cond, const std::string& name) {
    if (cond) {
        std::cout << "  OK: " << name << "\n";
    } else {
        std::cerr << "FAIL: " << name << "\n";
        failures++;
    }
}

static bool contains(const std::vector<std::string>& lines, const std::string& needle) {
    for (const auto& line : lines) {
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

int main() {
    std::cout << "═══ TUI Render Tests ═══\n\n";

    // Grid lookup/render smoke.
    {
        GridLayout grid(80, 24);
        grid.addCell("status", 0, 0, 40, 1);
        grid.setContent("status", {"ready"});
        auto rendered = grid.renderFull();
        check(grid.findCell("status") != nullptr, "grid finds cells by id");
        check(rendered.find("ready") != std::string::npos, "grid renders cell content");
    }

    // Markdown renders inline formatting and EOF table rows.
    {
        Markdown md;
        md.setWidth(80);
        md.setText("## Title\n\n**bold** and [link](https://example.com)\n\n| A | B |\n| - | - |\n| 1 | 2 |");
        auto lines = md.render();
        check(contains(lines, "Title"), "markdown renders headers");
        check(contains(lines, "bold"), "markdown renders inline bold");
        check(contains(lines, "https://example.com"), "markdown renders links");
        check(contains(lines, "1"), "markdown flushes table at EOF");
    }

    // ProtocolView incremental action/result rendering.
    {
        ProtocolView pv;
        pv.addAction({ActionType::TOOL, "fs_read", "r1", "{\"path\":\"README.md\"}", true});
        auto first = pv.render(80);
        pv.addResult({"r1", true, "# Cortex-Prime MK3", "fs_read", 0, 1.5, 128});
        auto second = pv.render(80);
        auto third = pv.render(80);
        int occurrences = 0;
        for (const auto& line : third) if (line.find("fs_read") != std::string::npos) occurrences++;
        check(contains(first, "fs_read"), "protocol renders action");
        check(contains(second, "Cortex-Prime MK3"), "protocol renders result");
        check(occurrences == 1, "protocol render is idempotent across frames");
    }

    // Multiline JSON params must be summarized into single terminal rows.
    {
        ProtocolView pv;
        pv.addAction({ActionType::TOOL, "fs_write", "w1", "{\"path\":\"tmp.py\",\"content\":\"line1\\nline2\\nline3\"}", true});
        auto lines = pv.render(80);
        bool embeddedNewline = false;
        for (const auto& line : lines) {
            if (line.find('\n') != std::string::npos || line.find('\r') != std::string::npos) embeddedNewline = true;
        }
        check(!embeddedNewline, "protocol action params have no embedded newlines");
        check(contains(lines, "chars") && contains(lines, "lines"), "protocol summarizes multiline content params");
    }

    // Renderer mode names include SEMI.
    {
        check(std::string(TuiRenderer::modeName(RenderMode::SEMI)) == "SEMI", "renderer exposes SEMI mode name");
    }

    std::cout << "\n═══ " << (failures ? "FAILED" : "ALL PASSED")
              << " (" << failures << " failures) ═══\n";
    return failures ? 1 : 0;
}
