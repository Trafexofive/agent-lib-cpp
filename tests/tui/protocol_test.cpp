// tests/tui/protocol_test.cpp — T4: Protocol renderer tests

#include "../../src/tui/components/protocol.hpp"
#include "../../src/tui/terminal.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace cortex::mk3::tui;

// ── Helpers ──
static std::string stripAnsi(const std::string& s) {
    std::string out;
    bool esc = false;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\033') { esc = true; continue; }
        if (esc) { if (s[i] == 'm') esc = false; continue; }
        out += s[i];
    }
    return out;
}

static bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

int main() {
    int failures = 0;
    auto check = [&](bool cond, const std::string& test) {
        if (!cond) { std::cerr << "FAIL: " << test << "\n"; failures++; }
        else { std::cout << "  OK: " << test << "\n"; }
    };

    std::cout << "═══ Protocol Renderer Tests ═══\n\n";

    // ── T4.1: Tool action renders with icon ──
    {
        ProtocolView pv;
        pv.addAction({ActionType::TOOL, "list", "ls1"});
        auto lines = pv.render(80);
        check(!lines.empty(), "tool action produces output");
        if (!lines.empty()) {
            std::string s = stripAnsi(lines[0]);
            check(contains(s, "list"), "tool action shows name");
        }
    }

    // ── T4.2: Agent action has different icon ──
    {
        ProtocolView pv;
        pv.addAction({ActionType::AGENT, "builder", "b1"});
        auto lines = pv.render(80);
        if (!lines.empty()) {
            std::string s = stripAnsi(lines[0]);
            check(contains(s, "builder"), "agent action shows name");
        }
    }

    // ── T4.3: Relic action ──
    {
        ProtocolView pv;
        pv.addAction({ActionType::RELIC, "postgres_main", "pg1"});
        auto lines = pv.render(80);
        if (!lines.empty()) {
            check(contains(stripAnsi(lines[0]), "postgres_main"), "relic action shows name");
        }
    }

    // ── T4.4: Feed action ──
    {
        ProtocolView pv;
        pv.addAction({ActionType::FEED, "ci-status", "f1"});
        auto lines = pv.render(80);
        if (!lines.empty()) {
            check(contains(stripAnsi(lines[0]), "ci-status"), "feed action shows name");
        }
    }

    // ── T4.5: Successful result (green/checkmark) ──
    {
        ProtocolView pv;
        pv.addAction({ActionType::TOOL, "grep", "g1"});
        pv.addResult({"g1", true, "found 3 matches"});
        auto lines = pv.render(80);
        // Should have both action and result lines
        check(lines.size() >= 2, "action + result produces >= 2 lines");
    }

    // ── T4.6: Failed result (red/X mark) ──
    {
        ProtocolView pv;
        pv.addAction({ActionType::TOOL, "exec", "e1"});
        pv.addResult({"e1", false, "command not found: xyz"});
        auto lines = pv.render(80);
        check(lines.size() >= 2, "failed action + result produces >= 2 lines");
    }

    // ── T4.7: Response renders as-is ──
    {
        ProtocolView pv;
        pv.addContent({BlockType::RESPONSE, "Hello world", true});
        auto lines = pv.render(80);
        check(!lines.empty(), "response produces output");
        if (!lines.empty()) {
            check(contains(stripAnsi(lines[0]), "Hello world"), "response content preserved");
        }
    }

    // ── T4.8: Multi-turn sequence in order ──
    {
        ProtocolView pv;
        pv.addAction({ActionType::TOOL, "list", "ls1"});
        pv.addResult({"ls1", true, "3 files"});
        pv.addAction({ActionType::TOOL, "grep", "g1"});
        pv.addResult({"g1", true, "1 match"});
        pv.addContent({BlockType::RESPONSE, "Done.", true});
        auto lines = pv.render(80);
        check(lines.size() >= 5, "2 actions + 2 results + response = >= 5 lines");
        // Verify order via stripped content
        std::string all;
        for (auto& l : lines) all += stripAnsi(l) + "\n";
        size_t listPos = all.find("list");
        size_t grepPos = all.find("grep");
        size_t donePos = all.find("Done.");
        check(listPos < grepPos && grepPos < donePos,
              "order: list → grep → Done");
    }

    // ── T4.9: Empty view produces nothing ──
    {
        ProtocolView pv;
        auto lines = pv.render(80);
        check(lines.empty() || (lines.size() == 1 && lines[0].empty()),
              "empty view produces no output");
    }

    // ── T4.10: Streaming — incremental produces new lines only ──
    {
        ProtocolView pv;
        // First batch
        pv.addAction({ActionType::TOOL, "list", "ls1"});
        pv.addResult({"ls1", true, "3 files"});
        auto batch1 = pv.incremental(80);
        check(batch1.size() == 2, "batch1: 2 new lines (action+result)");

        // Second batch
        pv.addAction({ActionType::TOOL, "grep", "g1"});
        auto batch2 = pv.incremental(80);
        check(batch2.size() == 1, "batch2: 1 new line (action only, no result yet)");

        // Third batch
        pv.addResult({"g1", true, "5 matches"});
        auto batch3 = pv.incremental(80);
        check(batch3.size() == 1, "batch3: 1 new line (result for grep)");
    }

    // ── T4.11: Streaming — no dupe after re-render ──
    {
        ProtocolView pv;
        pv.addAction({ActionType::TOOL, "exec", "e1"});
        auto first = pv.incremental(80);
        auto second = pv.incremental(80);  // no new events
        check(second.empty(), "incremental with no new events returns empty");
    }

    // ── T4.12: Streaming — reset restarts cursor ──
    {
        ProtocolView pv;
        pv.addAction({ActionType::TOOL, "list", "ls1"});
        pv.addResult({"ls1", true, "ok"});
        auto all = pv.incremental(80);
        check(all.size() == 2, "first incremental gets all lines");
        pv.resetStream();
        auto again = pv.incremental(80);
        check(again.size() == 2, "after reset, incremental returns all lines again");
    }

    std::cout << "\n═══ " << (failures ? "FAILED" : "ALL PASSED")
              << " (" << failures << " failures) ═══\n";
    return failures ? 1 : 0;
}
