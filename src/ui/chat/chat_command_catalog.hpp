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
        if (key == "name" && !value.empty()) out.name = "/" + value;
        else if (key == "description") out.description = value;
        else if (key == "argument-hint") out.argumentHint = value;
    }
    out.body = trimCommandText(text.substr(end + 5));
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
            out.push_back(parseDynamicCommandFile(entry.path(), entry.path().stem().string(), "prompt"));
        }
    }
    auto skills = manifestRoot / "skills";
    if (std::filesystem::exists(skills)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(skills)) {
            if (!entry.is_regular_file() || entry.path().filename() != "SKILL.md") continue;
            out.push_back(parseDynamicCommandFile(entry.path(), entry.path().parent_path().filename().string(), "skill"));
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return out;
}

inline const DynamicChatCommand* findDynamicChatCommand(
    const std::string& name, const std::vector<DynamicChatCommand>& commands) {
    for (const auto& command : commands)
        if (command.name == name) return &command;
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

inline std::vector<std::string> completeChatCommand(const std::string& prefix) {
    static const std::vector<std::string> builtins = {
        "/help", "/commands", "/clear", "/thoughts", "/raw", "/manifests",
        "/sessions", "/prompts", "/dump-prompt", "/dp", "/cp-all", "/cp-raw",
        "/quit", "/exit",
    };
    std::vector<std::string> out;
    for (const auto& name : builtins)
        if (name.rfind(prefix, 0) == 0) out.push_back(name);
    for (const auto& command : discoverDynamicChatCommands())
        if (command.name.rfind(prefix, 0) == 0) out.push_back(command.name);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace cortex::mk3::ui::chat
