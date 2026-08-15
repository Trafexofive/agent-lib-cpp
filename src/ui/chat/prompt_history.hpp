#pragma once
// Persistent chat prompt history.
//
// Format: one prompt per line, with backslash escapes so multi-line prompts
// (Shift+Enter) round-trip through save→load. Backward compatible with the
// legacy one-prompt-per-line files (they contain no escape sequences, so
// unescape is a no-op on old entries).

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace cortex::mk3::ui::chat {

inline std::string defaultPromptHistoryPath() {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.mk3_history" : "/tmp/.mk3_history";
}

inline std::string escapePromptLine(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

inline std::string unescapePromptLine(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[i + 1];
            switch (n) {
                case 'n': out += '\n'; ++i; break;
                case 'r': out += '\r'; ++i; break;
                case 't': out += '\t'; ++i; break;
                case '\\': out += '\\'; ++i; break;
                default: out += s[i]; break;  // lone backslash — keep literal
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

inline std::vector<std::string> loadPromptHistory(const std::string& path = defaultPromptHistoryPath()) {
    std::vector<std::string> history;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::string entry = unescapePromptLine(line);
        if (!entry.empty() && (history.empty() || history.back() != entry))
            history.push_back(std::move(entry));
    }
    return history;
}

inline bool savePromptHistory(const std::vector<std::string>& history,
                              const std::string& path = defaultPromptHistoryPath(),
                              size_t maxEntries = 500) {
    std::error_code ec;
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    std::ofstream out(path + ".tmp", std::ios::trunc);
    if (!out) return false;
    size_t start = history.size() > maxEntries ? history.size() - maxEntries : 0;
    for (size_t i = start; i < history.size(); ++i) {
        std::string line = escapePromptLine(history[i]);
        if (!line.empty()) out << line << '\n';
    }
    out.close();
    if (!out) return false;
    std::filesystem::rename(path + ".tmp", path, ec);
    return !ec;
}

}  // namespace cortex::mk3::ui::chat
