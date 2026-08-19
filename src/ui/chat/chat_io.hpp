#pragma once
// Chat copy/export helpers with deterministic local fallback.

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "src/ui/model/timeline_codec.hpp"
#include "src/utils/process.hpp"

namespace cortex::mk3::ui::chat {

struct CopyResult {
    bool copied = false;
    std::string destination;
};

inline bool pipeText(const char* command, const std::string& text) {
    // Bounded write — wl-copy/xclip can hang forever if the compositor is gone.
    process::Spec spec;
    spec.shell = true;
    spec.command = command;
    spec.stdinText = text;
    spec.timeoutMs = 1500;
    spec.maxStdout = 256;
    spec.maxStderr = 256;
    process::Result pr = process::run(spec);
    return pr.success();
}

inline CopyResult copyText(const std::string& text, const std::string& fallbackPath) {
    // Veil clipboard helper output to a private fd path so it never
    // reaches stdout/stderr of the TUI. Many xclip/win32yank builds print
    // "wl-copy: error..." on hang/disconnect when the daemon is gone.
    if (std::getenv("WAYLAND_DISPLAY") && pipeText("wl-copy", text)) return {true, "clipboard (wl-copy)"};
    if (std::getenv("DISPLAY") && pipeText("xclip -selection clipboard", text))
        return {true, "clipboard (xclip)"};
    std::ofstream out(fallbackPath, std::ios::trunc);
    if (!out) return {false, fallbackPath};
    out << text;
    return {static_cast<bool>(out), fallbackPath};
}

inline std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) out += line + "\n";
    return out;
}

inline std::vector<std::string> dumpPrompts(const std::vector<std::string>& prompts) {
    std::vector<std::string> messages;
    if (prompts.empty()) return {"no prompts captured — run a prompt first"};
    for (size_t i = 0; i < prompts.size(); ++i) {
        std::string path = "/tmp/mk3-prompt-iter" + std::to_string(i + 1) + ".xml";
        std::ofstream out(path, std::ios::trunc);
        if (!out) {
            messages.push_back("failed to write " + path);
            continue;
        }
        out << "<!-- Cortex MK3 Prompt — Iteration " << (i + 1) << " -->\n" << prompts[i];
        messages.push_back("wrote " + path + " (" + std::to_string(prompts[i].size()) + " bytes)");
    }
    return messages;
}

// Launch the pi `art` fullscreen artifact manager (stdio inherit).
// When manageScreen=true (no Engine suspend hook), leaves/restores alt-screen
// here. Prefer ShellModel::suspendTui/resumeTui so kitty keys + full_frame
// redraw are restored correctly.
inline int launchArtFullscreen(const std::string& args = {}, bool manageScreen = true) {
    const char* home = std::getenv("HOME");
    std::string bin;
    if (home && home[0]) {
        std::string c1 = std::string(home) + "/.pi/agent/bin/art";
        std::string c2 = std::string(home) + "/.local/bin/art";
        if (::access(c1.c_str(), X_OK) == 0) bin = c1;
        else if (::access(c2.c_str(), X_OK) == 0) bin = c2;
    }
    if (bin.empty()) bin = "art";

    char cwdBuf[1024] = {0};
    std::string cwd = ::getcwd(cwdBuf, sizeof(cwdBuf) - 1) ? cwdBuf : ".";
    std::string root = cwd + "/.artifacts";
    if (const char* envRoot = std::getenv("ART_ROOT"))
        if (envRoot[0]) root = envRoot;

    if (manageScreen) {
        // Best-effort leave: alt-screen off, show cursor, reset SGR, disable kitty.
        std::fputs("\x1b[?2004l\x1b[?1004l\x1b[<u\x1b[>4;0m"
                   "\x1b[?1049l\x1b[?25h\x1b[0m\x1b[?7h",
                   stdout);
        std::fflush(stdout);
    }

    std::string cmd = "ART_ROOT=" + root + " EDITOR=\"${EDITOR:-${VISUAL:-nvim}}\" ";
    cmd += "\"" + bin + "\"";
    if (!args.empty()) cmd += " " + args;
    int rc = std::system(cmd.c_str());

    if (manageScreen) {
        // Partial restore — caller without Engine still gets a usable screen.
        std::fputs("\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H", stdout);
        std::fflush(stdout);
    }
    return rc;
}

// Legacy: dump display lines as painted (noisy — prefer exportTimelineMarkdown).
inline CopyResult exportRenderedChat(const std::vector<std::string>& renderedLines,
                                     const std::string& header,
                                     const std::string& path = "/tmp/mk3-chat-export.txt") {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return {false, path};
    if (!header.empty()) out << header;
    if (!header.empty() && header.back() != '\n') out << '\n';
    out << "---\n";
    for (const auto& line : renderedLines) {
        // Strip selection marker for slightly cleaner dump.
        if (line.rfind("› ", 0) == 0)
            out << "  " << line.substr(2) << '\n';
        else
            out << line << '\n';
    }
    out << "---\n# lines=" << renderedLines.size() << '\n';
    return {static_cast<bool>(out), path};
}

// Structured export from timeline rows — readable markdown, full bodies,
// no truncation notes / unknown-command spam / double meta.
template <typename RowRange>
inline CopyResult exportTimelineMarkdown(const RowRange& rows, const std::string& header,
                                         const std::string& path = "/tmp/mk3-chat-export.md") {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return {false, path};
    if (!header.empty()) out << header;
    if (!header.empty() && header.back() != '\n') out << '\n';
    out << "\n";
    int n = 0;
    for (const auto& row : rows) {
        using K = decltype(row.kind);
        // Skip pure chrome / stream / empty logs.
        if (static_cast<int>(row.kind) == static_cast<int>(TimelineKind::Stream))
            continue;
        if (static_cast<int>(row.kind) == static_cast<int>(TimelineKind::Log)) {
            // Drop slash-unknown / render-cycle notices from export.
            if (row.title == "unknown command" || row.title == "render" ||
                row.title == "commands" || row.title == "model")
                continue;
        }
        std::string h;
        switch (row.kind) {
            case TimelineKind::User:
                h = row.title.rfind("parent:", 0) == 0 ? "### Parent" : "### You";
                break;
            case TimelineKind::Thought: h = "### Thought"; break;
            case TimelineKind::Action: {
                h = "### Action";
                if (!row.actionType.empty()) h += " · " + row.actionType;
                if (!row.actionName.empty()) h += " · " + row.actionName;
                if (!row.actionId.empty()) h += " · #" + row.actionId;
                break;
            }
            case TimelineKind::Result:
                h = row.ok ? "### Result · ok" : "### Result · error";
                if (!row.actionName.empty()) h += " · " + row.actionName;
                if (!row.actionId.empty()) h += " · #" + row.actionId;
                break;
            case TimelineKind::Response: h = "### Response"; break;
            case TimelineKind::Error: h = "### Error"; break;
            case TimelineKind::Status: h = "### Status · " + row.title; break;
            default: h = "### " + (row.title.empty() ? std::string("note") : row.title); break;
        }
        out << h << "\n\n";
        // Body: drop trailing meta-only duplicate line if it's already in header sense.
        std::string body = row.body;
        // Strip UI truncation footer if present.
        auto truncAt = body.find("… (");
        if (truncAt != std::string::npos && body.find("more lines") != std::string::npos) {
            // Prefer full body from row — truncation is display-only; body should be full.
        }
        if (!body.empty()) {
            out << body;
            if (body.back() != '\n') out << '\n';
        }
        out << "\n";
        ++n;
    }
    out << "---\n# blocks=" << n << '\n';
    return {static_cast<bool>(out), path};
}

}  // namespace cortex::mk3::ui::chat
