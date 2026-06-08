#pragma once
// =============================================================================
// agent-lib-MK3 — Slash Command Catalog
// Discovery/completion only. Command execution remains in main until the REPL
// controller is extracted.
// =============================================================================

#include <string>
#include <vector>

namespace cortex::mk3::tui {

struct SlashCommandEntry {
    std::string name;
    std::string summary;
};

class SlashCommands {
public:
    static const std::vector<SlashCommandEntry>& entries() {
        static const std::vector<SlashCommandEntry> cmds = {
            {"/help",        "Show slash commands"},
            {"/exit",        "Exit the REPL"},
            {"/quit",        "Exit the REPL"},
            {"/prompts",     "Toggle prompt inspection mode"},
            {"/dump-prompt", "Write captured prompts to /tmp/mk3-prompt-iterN.xml"},
            {"/dp",          "Alias for /dump-prompt"},
            {"/sessions",    "List saved sessions"},
            {"/cp-all",      "Copy visible history + output to clipboard or /tmp/mk3-cp-all.txt"},
            {"/cp-raw",      "Copy raw LLM output to clipboard or /tmp/mk3-cp-raw.txt"},
        };
        return cmds;
    }

    static std::vector<std::string> complete(const std::string& prefix) {
        std::vector<std::string> out;
        if (prefix.empty() || prefix[0] != '/') return out;
        for (const auto& e : entries()) {
            if (e.name.rfind(prefix, 0) == 0) out.push_back(e.name);
        }
        return out;
    }

    static std::vector<std::string> helpLines() {
        std::vector<std::string> lines;
        lines.push_back("─── Slash Commands ───");
        for (const auto& e : entries()) {
            lines.push_back("  " + e.name + " — " + e.summary);
        }
        lines.push_back("");
        lines.push_back("Readline keys: Ctrl-A/E start/end, Ctrl-B/F left/right, Alt-B/F word move,");
        lines.push_back("               Ctrl-P/N history, Ctrl-R search, Ctrl-U/K kill line,");
        lines.push_back("               Ctrl-W kill word, Ctrl-Y yank, Ctrl-L redraw, Esc cancel.");
        return lines;
    }
};

} // namespace cortex::mk3::tui
