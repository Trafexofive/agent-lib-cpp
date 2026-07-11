#pragma once
// Persistent chat prompt history. Compatible with legacy one-prompt-per-line files.

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

inline std::vector<std::string> loadPromptHistory(const std::string& path = defaultPromptHistoryPath()) {
    std::vector<std::string> history;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && (history.empty() || history.back() != line)) history.push_back(line);
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
        std::string line = history[i];
        for (char& c : line)
            if (c == '\n' || c == '\r') c = ' ';
        if (!line.empty()) out << line << '\n';
    }
    out.close();
    if (!out) return false;
    std::filesystem::rename(path + ".tmp", path, ec);
    return !ec;
}

}  // namespace cortex::mk3::ui::chat
