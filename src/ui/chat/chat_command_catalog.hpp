#pragma once
// Dynamic prompt/skill command discovery for chat. No dependency on src/tui.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace cortex::mk3::ui::chat {

struct DynamicChatCommand {
    std::string name;
    std::string description;
    std::string argumentHint;
    std::string body;
    std::string path;
    std::string kind;
};

inline std::string trimCommandText(std::string value) {
    size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

inline DynamicChatCommand parseDynamicCommandFile(const std::filesystem::path& path,
                                                   const std::string& fallbackName,
                                                   const std::string& kind) {
    DynamicChatCommand out;
    out.name = "/" + fallbackName;
    out.path = path.string();
    out.kind = kind;
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    out.body = text;
    if (text.rfind("---", 0) != 0) return out;
    size_t end = text.find("\n---", 3);
    if (end == std::string::npos) return out;
    std::istringstream front(text.substr(4, end - 4));
    std::string line;
    while (std::getline(front, line)) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trimCommandText(line.substr(0, colon));
        std::string value = trimCommandText(line.substr(colon + 1));
        if (key == "name" && !value.empty()) {
            // Strip leading / and any prior :suffix — we re-apply kind suffix below.
            std::string n = value;
            if (!n.empty() && n[0] == '/') n = n.substr(1);
            auto colon = n.find(':');
            if (colon != std::string::npos) n = n.substr(0, colon);
            out.name = "/" + n;
        } else if (key == "description") out.description = value;
        else if (key == "argument-hint") out.argumentHint = value;
    }
    out.body = trimCommandText(text.substr(end + 5));
    // Canonical suffixes so tab-complete distinguishes skills vs prompts.
    if (out.kind == "skill" && out.name.find(":skill") == std::string::npos)
        out.name += ":skill";
    else if (out.kind == "prompt" && out.name.find(":prompt") == std::string::npos)
        out.name += ":prompt";
    return out;
}

inline std::vector<DynamicChatCommand> discoverDynamicChatCommands(
    const std::filesystem::path& manifestRoot = "manifests") {
    std::vector<DynamicChatCommand> out;
    auto prompts = manifestRoot / "prompts";
    if (std::filesystem::exists(prompts)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(prompts)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".md" ||
                entry.path().filename() == "README.md") continue;
            auto cmd = parseDynamicCommandFile(entry.path(), entry.path().stem().string(), "prompt");
            if (cmd.name.find(":prompt") == std::string::npos) cmd.name += ":prompt";
            out.push_back(std::move(cmd));
        }
    }
    auto skills = manifestRoot / "skills";
    if (std::filesystem::exists(skills)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(skills)) {
            if (!entry.is_regular_file() || entry.path().filename() != "SKILL.md") continue;
            auto cmd = parseDynamicCommandFile(entry.path(),
                                              entry.path().parent_path().filename().string(), "skill");
            if (cmd.name.find(":skill") == std::string::npos) cmd.name += ":skill";
            out.push_back(std::move(cmd));
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return out;
}

inline const DynamicChatCommand* findDynamicChatCommand(
    const std::string& name, const std::vector<DynamicChatCommand>& commands) {
    // Exact first, then bare name match against /foo:skill or /foo:prompt.
    for (const auto& command : commands)
        if (command.name == name) return &command;
    std::string bare = name;
    auto colon = bare.find(':');
    if (colon != std::string::npos) bare = bare.substr(0, colon);
    for (const auto& command : commands) {
        std::string cn = command.name;
        auto c2 = cn.find(':');
        if (c2 != std::string::npos) cn = cn.substr(0, c2);
        if (cn == bare || cn == name) return &command;
    }
    return nullptr;
}

inline std::string expandDynamicChatCommand(const DynamicChatCommand& command,
                                            const std::string& arguments) {
    std::string body = command.body;
    bool replaced = false;
    for (const std::string marker : {"$ARGUMENTS", "${ARGUMENTS}", "{{args}}"}) {
        size_t pos = 0;
        while ((pos = body.find(marker, pos)) != std::string::npos) {
            body.replace(pos, marker.size(), arguments);
            pos += arguments.size();
            replaced = true;
        }
    }
    if (!arguments.empty() && !replaced) body += "\n\nUser arguments: " + arguments;
    return body;
}

// userFacing: omit debug cmds unless Settings · DEV MODE (or CORTEX_DEV_MODE).
inline std::vector<std::string> completeChatCommand(const std::string& prefix,
                                                    bool devMode = false) {
    static const std::vector<std::string> builtins = {
        "/help", "/commands", "/clear", "/thoughts", "/truncate", "/raw", "/theme",
        "/manifests", "/sessions", "/cp-all", "/artifacts", "/art", "/model",
        "/continue", "/cont", "/stop", "/cancel", "/quit", "/exit",
    };
    static const std::vector<std::string> devBuiltins = {
        "/export-chat", "/export-dump", "/prompts", "/dump-prompt", "/dp", "/cp-raw",
    };
    std::string p = prefix;
    if (p.empty()) p = "/";
    std::vector<std::string> out;
    for (const auto& name : builtins)
        if (name.rfind(p, 0) == 0) out.push_back(name);
    if (devMode) {
        for (const auto& name : devBuiltins)
            if (name.rfind(p, 0) == 0) out.push_back(name);
    }
    for (const auto& command : discoverDynamicChatCommands())
        if (command.name.rfind(p, 0) == 0) out.push_back(command.name);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Longest common prefix of matches — bash/readline first-tab behavior.
inline std::string commonPrefixOf(const std::vector<std::string>& items) {
    if (items.empty()) return {};
    std::string prefix = items.front();
    for (size_t i = 1; i < items.size(); ++i) {
        size_t n = 0;
        while (n < prefix.size() && n < items[i].size() && prefix[n] == items[i][n]) ++n;
        prefix.resize(n);
        if (prefix.empty()) break;
    }
    return prefix;
}

}  // namespace cortex::mk3::ui::chat
