// tests/tui/render_test.cpp — TUI renderer/grid/protocol smoke tests

#include <iostream>
#include <string>
#include <vector>

#include "../../src/tui/components/markdown.hpp"
#include "../../src/tui/components/protocol.hpp"
#include "../../src/tui/grid.hpp"
#include "../../src/tui/renderer.hpp"
#include "../../src/tui/session_view.hpp"
#include "../../src/tui/surface.hpp"
#include "../../src/tui/width.hpp"

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
        if (line.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

static int findLine(const std::vector<std::string>& lines, const std::string& needle) {
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        if (lines[static_cast<size_t>(i)].find(needle) != std::string::npos)
            return i;
    }
    return -1;
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
        md.setText(
            "## Title\n\n**bold** and [link](https://example.com)\n\n| A | B |\n| - | - |\n| 1 | 2 "
            "|");
        auto lines = md.render();
        check(contains(lines, "Title"), "markdown renders headers");
        check(contains(lines, "bold"), "markdown renders inline bold");
        check(contains(lines, "https://example.com"), "markdown renders links");
        check(contains(lines, "1"), "markdown flushes table at EOF");
    }

    // ANSI/wide char width accounting for box drawing rows.
    {
        std::string row = std::string("  ") + ansi::fg(0, 200, 0) + "╭─ Start ──╮" + ansi::reset();
        check(visibleWidth(row) == 14, "visibleWidth counts box drawing as terminal columns");
        std::string padded = padRight(row, 20);
        check(visibleWidth(padded) == 20, "padRight reaches terminal width");
    }

    // Markdown fenced code blocks must paint the full row before reset.
    {
        Markdown md;
        md.setWidth(16);  // mirrors TuiRenderer terminal width 20 -> markdown width 16
        md.setText("```\nabc\n```");
        auto lines = md.render();
        check(!lines.empty(), "markdown renders fenced code");
        check(visibleWidth(lines[0]) == 20, "markdown code row fills terminal width");
        check(lines[0].find(ansi::bg(20, 20, 40)) != std::string::npos,
              "markdown code row keeps background styling");
    }

    // 2D surface primitives clamp rows, preserve ANSI spans, and diff changed rows.
    {
        TuiSurface surface(10);
        surface.appendLine(ansi::fg(255, 0, 0) + std::string("hello world"));
        surface.appendLine("ok");
        check(surface.rows().size() == 2, "surface appends rows");
        check(visibleWidth(surface.rows()[0]) == 10, "surface clamps row to width");
        check(surface.rows()[0].find(ansi::fg(255, 0, 0)) != std::string::npos,
              "surface preserves ANSI while fitting");
        std::vector<std::string> previous = {surface.rows()[0], "old"};
        auto dirty = TuiSurface::dirtyRows(previous, surface.rows());
        check(dirty.size() == 1 && dirty[0] == 1, "surface dirty diff tracks changed rows");
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
        for (const auto& line : third)
            if (line.find("fs_read") != std::string::npos)
                occurrences++;
        check(contains(first, "fs_read"), "protocol renders action");
        check(!first.empty() && visibleWidth(first.front()) == 80 &&
                  first.front().find("fs_read") == std::string::npos,
              "protocol action has top background padding");
        check(first.size() >= 4 && visibleWidth(first[3]) == 80 &&
                  first[3].find("README") == std::string::npos,
              "protocol action has bottom background padding");
        check(contains(second, "Cortex-Prime MK3"), "protocol renders result");
        check(contains(second, "✓"), "protocol renders result status");
        check(occurrences == 1, "protocol render is idempotent across frames");
    }

    // Agent results render as green background cards with breathing room.
    {
        ProtocolView pv;
        pv.addAction({ActionType::AGENT, "default", "a1", "ping", true});
        pv.addResult({"a1", true, "default agent online.", "default", 0, 1973, 21});
        auto lines = pv.render(80);
        check(contains(lines, "\033[48;2;20;50;30m"), "agent result uses green background card");
        check(contains(lines, "✓ 1973ms 21B"), "agent result card includes metadata");
        check(contains(lines, "default agent online."), "agent result card includes reply");
        check(!contains(lines, "╭") && !contains(lines, "╰"), "agent result avoids border glyphs");
    }

    // Agent result long lines wrap instead of inserting lossy ellipsis truncation.
    {
        ProtocolView pv;
        pv.addAction({ActionType::AGENT, "default", "a1", "ping", true});
        pv.addResult({"a1", true, std::string(120, 'x'), "default", 0, 1, 120});
        auto lines = pv.render(40);
        check(!contains(lines, "..."), "agent result does not ellipsis-truncate long lines");
        int xRows = 0;
        for (const auto& line : lines)
            if (line.find("xxxxxxxx") != std::string::npos)
                xRows++;
        check(xRows >= 2, "agent result wraps long streamed lines");
    }

    // Agent action icon must not reset the background inside the card.
    {
        ProtocolView pv;
        pv.addAction({ActionType::AGENT, "default", "a1", "ping", true});
        auto lines = pv.render(80);
        bool badReset = false;
        for (const auto& line : lines) {
            auto arrow = line.find("→");
            auto name = line.find("default");
            auto reset = line.find("\033[0m");
            if (arrow != std::string::npos && name != std::string::npos &&
                reset != std::string::npos && reset > arrow && reset < name)
                badReset = true;
        }
        check(!badReset, "agent action icon does not reset card background");
    }

    // Tool output ANSI is preserved by default and can be disabled.
    {
        ProtocolView pv;
        std::string red = ansi::fg(255, 0, 0) + "RED" + ansi::reset();
        pv.addResult({"c1", true, red, "diagram_render", 0, 0, red.size()});
        auto colored = pv.render(80);
        check(contains(colored, ansi::fg(255, 0, 0)), "protocol preserves tool output ANSI");
        bool dimWrapped = false;
        for (const auto& line : colored) {
            auto redPos = line.find(ansi::fg(255, 0, 0));
            auto dimPos = line.find(ansi::dim());
            if (redPos != std::string::npos && dimPos != std::string::npos && dimPos < redPos)
                dimWrapped = true;
        }
        check(!dimWrapped, "protocol does not dim-wrap colored tool output");

        ProtocolView plain;
        plain.setAnsiPassthrough(false);
        plain.addResult({"c1", true, red, "diagram_render", 0, 0, red.size()});
        auto stripped = plain.render(80);
        check(!contains(stripped, ansi::fg(255, 0, 0)), "protocol can strip tool output ANSI");
        check(contains(stripped, "RED"), "protocol keeps stripped tool output text");
    }

    // Multiline JSON params must be summarized into single terminal rows.
    {
        ProtocolView pv;
        pv.addAction({ActionType::TOOL, "fs_write", "w1",
                      "{\"path\":\"tmp.py\",\"content\":\"line1\\nline2\\nline3\"}", true});
        auto lines = pv.render(80);
        bool embeddedNewline = false;
        for (const auto& line : lines) {
            if (line.find('\n') != std::string::npos || line.find('\r') != std::string::npos)
                embeddedNewline = true;
        }
        check(!embeddedNewline, "protocol action params have no embedded newlines");
        check(contains(lines, "chars") && contains(lines, "lines"),
              "protocol summarizes multiline content params");
    }

    // FULL mode preserves the existing thought-stream feature.
    {
        TuiRenderer r(80);
        r.setThought("Let me plan this out in detail.");
        r.setResponse("Done.");
        auto lines = r.render();
        check(contains(lines, "Let me plan"), "full renderer shows thought stream");
        check(contains(lines, "Done"), "full renderer still shows response");
        r.setThought("");
        lines = r.render();
        check(!contains(lines, "Let me plan"), "full renderer clears thought stream");
    }

    // Transcript renderer must preserve protocol event order; responses are not
    // hoisted past later thought/action events.
    {
        TuiRenderer r(80);
        std::vector<cortex::mk3::ProtocolEvent> events;
        cortex::mk3::ProtocolEvent firstThought;
        firstThought.kind = cortex::mk3::ProtocolEventKind::THOUGHT;
        firstThought.text = "first thought";
        events.push_back(firstThought);
        cortex::mk3::ProtocolEvent response;
        response.kind = cortex::mk3::ProtocolEventKind::RESPONSE;
        response.text = "ordered response";
        events.push_back(response);
        cortex::mk3::ProtocolEvent secondThought;
        secondThought.kind = cortex::mk3::ProtocolEventKind::THOUGHT;
        secondThought.text = "second thought";
        events.push_back(secondThought);
        auto lines = r.renderTranscript(events, "ordered response", 80);
        int first = findLine(lines, "first thought");
        int resp = findLine(lines, "ordered response");
        int second = findLine(lines, "second thought");
        check(first >= 0 && resp > first && second > resp,
              "renderer preserves thought/response event order");
        int responseCount = 0;
        for (const auto& line : lines)
            if (line.find("ordered response") != std::string::npos)
                responseCount++;
        check(responseCount == 1, "renderer does not duplicate response event text");
    }

    // SessionView viewport should stay contiguous and anchored above bottom bars.
    {
        SessionView view(80, 10);
        std::vector<std::string> history(8);
        for (int i = 0; i < 8; ++i)
            history[i] = "line" + std::to_string(i);
        int scroll = 0;
        auto vp = view.build(history, {}, {}, false, scroll);
        check(vp.visible.size() == 8, "session viewport uses body rows");
        check(vp.startRow == 1, "session viewport anchors above bottom bars");
        for (int i = 0; i < static_cast<int>(vp.visible.size()) - 1; ++i) {
            int a = std::stoi(vp.visible[i].substr(4));
            int b = std::stoi(vp.visible[i + 1].substr(4));
            if (b != a + 1) {
                failures++;
                std::cerr << "FAIL: session viewport is contiguous between " << vp.visible[i]
                          << " and " << vp.visible[i + 1] << "\n";
                break;
            }
        }
        std::string frame = view.renderFull(vp, [](int) { return "status"; }, "prompt");
        check(frame.find("\033[9;1H") != std::string::npos && frame.find("status") != std::string::npos, "session frame renders status bar");
        check(frame.find("prompt") != std::string::npos, "session frame renders prompt line");
    }

    // SessionView starts with a deterministic full draw, then emits row diffs.
    {
        SessionView view(80, 10);
        std::vector<std::string> history(3, "old");
        int scroll = 0;
        auto vp1 = view.build(history, {}, {}, false, scroll);
        auto first = view.render(vp1, [](int) { return "status"; }, "prompt");
        history.insert(history.begin(), "top");
        auto vp2 = view.build(history, {}, {}, false, scroll);
        auto second = view.render(vp2, [](int) { return "status"; }, "prompt");
        auto forced = view.render(vp2, [](int) { return "status"; }, "prompt", true);
        check(first.find("\033[H\033[J") != std::string::npos, "first session render is full");
        check(second.find("\033[H\033[J") == std::string::npos, "second session render uses diff");
        check(forced.find("\033[H\033[J") != std::string::npos, "forced session render is full");
    }

    // Renderer mode names include SEMI.
    {
        check(std::string(TuiRenderer::modeName(RenderMode::SEMI)) == "SEMI",
              "renderer exposes SEMI mode name");
    }

    std::cout << "\n═══ " << (failures ? "FAILED" : "ALL PASSED") << " (" << failures
              << " failures) ═══\n";
    return failures ? 1 : 0;
}
