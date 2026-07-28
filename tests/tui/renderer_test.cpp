// tests/tui/renderer_test.cpp — quick smoke test for TuiRenderer
#include "../../src/tui/renderer.hpp"

#include <cassert>
#include <iostream>
using namespace cortex::mk3::tui;

int main() {
    int failures = 0;
    auto check = [&](bool c, const char* msg) {
        if (!c) {
            std::cerr << "FAIL: " << msg << "\n";
            failures++;
        } else
            std::cout << "  OK: " << msg << "\n";
    };
    auto contains = [](const std::vector<std::string>& rows, const std::string& needle) {
        for (const auto& row : rows) {
            if (row.find(needle) != std::string::npos)
                return true;
        }
        return false;
    };

    std::cout << "═══ TuiRenderer Smoke ═══\n\n";

    TuiRenderer r(80);

    // FULL mode with response only
    r.setMode(RenderMode::FULL);
    r.setRawStream("raw stream");
    r.setResponse("Hello world");
    auto lines = r.render();
    check(!lines.empty(), "FULL: response produces lines");
    if (!lines.empty())
        check(lines[0].find("Hello") != std::string::npos, "FULL: response text visible");

    // SEMI mode
    r.setMode(RenderMode::SEMI);
    lines = r.render();
    check(lines.size() >= 1, "SEMI: produces lines");
    if (!lines.empty()) {
        check(lines[0].find("Hello") != std::string::npos, "SEMI: right column has response text");
        check(lines[0].find("│") != std::string::npos, "SEMI: has separator");
    }

    // FULL mode with protocol actions
    r.clear();
    r.setMode(RenderMode::FULL);
    r.setRawStream("<action type=\"tool\" name=\"list\" id=\"ls1\">...</action>");
    r.setResponse("files found");
    r.addProtocolAction("tool", "list", "ls1", true);
    r.addProtocolResult("ls1", true, "3 files");
    lines = r.render();
    check(lines.size() >= 6, "FULL with actions: card rows rendered");
    check(contains(lines, "list"), "FULL: action rendered");
    check(contains(lines, "3 files"), "FULL: result rendered");

    // SEMI mode with actions
    r.setMode(RenderMode::SEMI);
    lines = r.render();
    check(lines.size() >= 3, "SEMI with actions: >= 3 lines");
    if (lines.size() >= 3) {
        check(lines[0].find("│") != std::string::npos, "SEMI: line 0 has separator");
        check(contains(lines, "3 files"), "SEMI: result visible");
    }

    // RAW mode
    r.setMode(RenderMode::RAW);
    lines = r.render();
    check(lines.size() >= 1, "RAW: produces lines");
    check(lines[0] == "<action type=\"tool\" name=\"list\" id=\"ls1\">...</action>",
          "RAW: raw stream shown");

    // Mode cycle
    r.cycleMode();
    check(r.mode() == RenderMode::FULL, "cycle: RAW→FULL");

    // ── Streaming consistency: multiple renders with same data match ──
    {
        TuiRenderer rs(80);
        rs.setMode(RenderMode::FULL);
        rs.setRawStream("<action name='a'>...</action>");
        rs.setResponse("response");
        rs.addProtocolAction("tool", "list", "ls1", true);
        rs.addProtocolResult("ls1", true, "ok");
        auto r1 = rs.render();
        auto r2 = rs.render();  // re-render without changes
        check(r1.size() == r2.size(), "streaming: consistent size on re-render");
        for (size_t i = 0; i < r1.size() && i < r2.size(); i++)
            check(r1[i] == r2[i], "streaming: line identical on re-render");
    }

    // ── Incremental rendering: adding content produces new lines ──
    {
        TuiRenderer ri(80);
        ri.setMode(RenderMode::FULL);
        ri.setRawStream("");
        ri.setResponse("");
        ri.addProtocolAction("tool", "list", "ls1", true);
        auto r1 = ri.render();
        check(r1.size() >= 3, "incremental: action card rows rendered");

        ri.addProtocolResult("ls1", true, "3 files");
        auto r2 = ri.render();
        check(r2.size() > r1.size(), "incremental: result appends rows");
        check(r2[0] == r1[0], "incremental: first line preserved");
    }

    // ── Mode toggle preserves data ──
    {
        TuiRenderer rm(80);
        rm.setRawStream("raw");
        rm.setResponse("hello");
        rm.addProtocolAction("tool", "list", "ls1", true);
        rm.addProtocolResult("ls1", true, "ok");
        rm.setMode(RenderMode::FULL);
        auto f = rm.render();
        rm.setMode(RenderMode::SEMI);
        auto s = rm.render();
        rm.setMode(RenderMode::RAW);
        auto w = rm.render();
        check(!f.empty(), "toggle: FULL has output");
        check(!s.empty(), "toggle: SEMI has output");
        check(!w.empty(), "toggle: RAW has output");
        check(f != s, "toggle: FULL != SEMI");
        check(s != w, "toggle: SEMI != RAW");
    }

    std::cout << "\n═══ " << (failures ? "FAILED" : "ALL PASSED") << " ═══\n";
    return failures;
}
