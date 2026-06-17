#pragma once
// =============================================================================
// agent-lib-MK3 — Slash Command Catalog
// Discovery/completion only. Command execution remains in main until the REPL
// controller is extracted.
// =============================================================================

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace cortex::mk3::tui {

struct SlashCommandEntry {
    std::string name;
    std::string summary;
    std::string category;
    std::string path;
    std::string kind;
    std::string argumentHint;
    std::string body;
};

namespace slash_detail {

static std::string trim(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

static std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string firstBodyLine(const std::string& body) {
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.rfind("# ", 0) == 0) line = trim(line.substr(2));
        if (!line.empty()) return line;
    }
    return "";
}

struct FrontMatter {
    std::string name;
    std::string description;
    std::string argumentHint;
    std::string body;
};

static FrontMatter parseFrontMatter(const std::string& text) {
    FrontMatter out;
    out.body = text;
    if (text.rfind("---", 0) != 0) {
        out.description = firstBodyLine(text);
        return out;
    }

    size_t end = text.find("\n---", 3);
    if (end == std::string::npos) return out;
    out.body = trim(text.substr(end + 5));

    std::istringstream fm(text.substr(4, end - 4));
    std::string line;
    std::string activeFoldedKey;
    bool inDescription = false;
    while (std::getline(fm, line)) {
        if (inDescription) {
            if (!line.empty() && line[0] != ' ' && line[0] != '\t' && line.find(':') != std::string::npos) {
                inDescription = false;
            } else if (!line.empty()) {
                out.description += " " + trim(line);
                continue;
            }
        }

        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(line.substr(0, colon));
        std::string val = trim(line.substr(colon + 1));
        if (key == "name") out.name = val;
        else if (key == "description") {
            out.description.clear();
            if (val.empty() || val == ">" || val == "|") {
                inDescription = true;
                activeFoldedKey = key;
            } else {
                out.description = val;
            }
        }
        else if (key == "argument-hint") out.argumentHint = val;
    }

    if (out.description.empty()) out.description = firstBodyLine(out.body);
    return out;
}

static SlashCommandEntry promptEntry(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto meta = parseFrontMatter(text);
    std::string name = meta.name.empty() ? p.stem().string() : meta.name;
    std::string summary = meta.description;
    if (!meta.argumentHint.empty()) summary += " [arg: " + meta.argumentHint + "]";
    summary += " [prompt]";
    return {"/" + name, summary, "prompt", p.string(), "prompt", meta.argumentHint, meta.body};
}

static SlashCommandEntry skillEntry(const std::filesystem::path& dir) {
    std::filesystem::path p = dir / "SKILL.md";
    std::ifstream f(p);
    if (!f) return {};
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto meta = parseFrontMatter(text);
    std::string name = meta.name.empty() ? dir.filename().string() : meta.name;
    std::string summary = meta.description;
    if (!meta.argumentHint.empty()) summary += " [arg: " + meta.argumentHint + "]";
    summary += " [skill]";
    return {"/" + name, summary, "skill", p.string(), "skill", meta.argumentHint, meta.body};
}

static std::vector<SlashCommandEntry> libraryEntries() {
    std::vector<SlashCommandEntry> entries;
    std::filesystem::path prompts = "manifests/prompts";
    if (std::filesystem::exists(prompts)) {
        for (const auto& it : std::filesystem::recursive_directory_iterator(prompts)) {
            if (!it.is_regular_file() || it.path().extension() != ".md") continue;
            if (it.path().filename() == "README.md") continue;
            auto e = promptEntry(it.path());
            if (!e.name.empty()) entries.push_back(std::move(e));
        }
    }

    std::filesystem::path skills = "manifests/skills";
    if (std::filesystem::exists(skills)) {
        for (const auto& it : std::filesystem::recursive_directory_iterator(skills)) {
            if (!it.is_directory()) continue;
            if (!std::filesystem::exists(it.path() / "SKILL.md")) continue;
            auto e = skillEntry(it.path());
            if (!e.name.empty()) entries.push_back(std::move(e));
        }
    }

    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        int cat = a.category.compare(b.category);
        if (cat != 0) return cat < 0;
        return a.name < b.name;
    });
    return entries;
}

static std::string commandKey(const std::string& cmd) {
    std::string k = cmd;
    if (k.rfind("/prompt/", 0) == 0) k.erase(0, 8);
    else if (k.rfind("/skill/", 0) == 0) k.erase(0, 7);
    else if (!k.empty() && k[0] == '/') k.erase(0, 1);
    return lower(k);
}

} // namespace slash_detail

class SlashCommands {
public:
    static std::vector<SlashCommandEntry> entries() {
        std::vector<SlashCommandEntry> cmds = {
            {"/commands",    "Show grouped slash command catalog", "help", "", "", "", ""},
            {"/help",        "Show slash commands", "help", "", "", "", ""},
            {"/manifests",   "Show active tools, feeds, relics, agents, workflows", "inspect", "", "", "", ""},
            {"/prompts",     "Toggle prompt inspection mode", "inspect", "", "", "", ""},
            {"/dump-prompt", "Write captured prompts to /tmp/mk3-prompt-iterN.xml", "inspect", "", "", "", ""},
            {"/dp",          "Alias for /dump-prompt", "inspect", "", "", "", ""},
            {"/dump-render", "Write TUI render/debug state to dump path", "inspect", "", "", "", ""},
            {"/dr",          "Alias for /dump-render", "inspect", "", "", "", ""},
            {"/sessions",    "List saved sessions", "session", "", "", "", ""},
            {"/cp-all",      "Copy visible history + output to clipboard or /tmp/mk3-cp-all.txt", "copy", "", "", "", ""},
            {"/cp-raw",      "Copy raw LLM output to clipboard or /tmp/mk3-cp-raw.txt", "copy", "", "", "", ""},
            {"/exit",        "Exit the REPL", "control", "", "", "", ""},
            {"/quit",        "Exit the REPL", "control", "", "", "", ""},
        };
        auto libs = slash_detail::libraryEntries();
        cmds.insert(cmds.end(), libs.begin(), libs.end());
        return cmds;
    }

    static const SlashCommandEntry* find(const std::string& cmd) {
        if (cmd.empty() || cmd[0] != '/') return nullptr;
        std::string key = slash_detail::commandKey(cmd);
        for (const auto& e : entries()) {
            if (slash_detail::commandKey(e.name) == key) return &e;
        }
        return nullptr;
    }

    static bool isDynamic(const std::string& cmd) {
        auto* e = find(cmd);
        return e && (e->kind == "prompt" || e->kind == "skill");
    }

    static std::vector<std::string> renderDynamic(const std::string& cmd) {
        std::vector<std::string> lines;
        auto* e = find(cmd);
        if (!e) return lines;
        lines.push_back("─── " + e->name + " ───");
        lines.push_back("kind: " + e->kind);
        lines.push_back("path: " + e->path);
        if (!e->argumentHint.empty()) lines.push_back("hint: " + e->argumentHint);
        lines.push_back("");
        std::istringstream in(e->body);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
        if (lines.size() == 4) lines.push_back("(no body)");
        return lines;
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
        std::string current;
        for (const auto& e : entries()) {
            if (e.category != current) {
                current = e.category;
                lines.push_back("");
                lines.push_back("[" + current + "]");
            }
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
