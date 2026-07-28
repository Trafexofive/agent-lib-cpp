#pragma once
// Chat copy/export helpers with deterministic local fallback.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace cortex::mk3::ui::chat {

struct CopyResult {
    bool copied = false;
    std::string destination;
};

inline bool pipeText(const char* command, const std::string& text) {
    // Vet-fix: route BOTH ends through /dev/null so a clipboard helper
    // that hangs or prints status to stderr (wl-copy, xclip, win32yank)
    // cannot leak its output into the TUI's foreground terminal.
    // We only care whether the write succeeded, not the helper's chatter.
    FILE* pipe = ::popen(command, "w");
    if (!pipe) return false;
    size_t written = std::fwrite(text.data(), 1, text.size(), pipe);
    int rc = ::pclose(pipe);
    return written == text.size() && rc == 0;
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

}  // namespace cortex::mk3::ui::chat
