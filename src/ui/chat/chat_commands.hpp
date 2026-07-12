#pragma once
// Chat-local slash command controller. Independent of src/tui and inkcell.

#include <sstream>
#include <string>
#include <vector>

#include "src/session/manager.hpp"
#include "src/ui/chat/chat_command_catalog.hpp"

namespace cortex::mk3::ui::chat {

struct ChatCommandContext {
    std::string manifestPath;
    std::string harnessPath;
    std::string systemPromptPath;
    std::string personaPath;
    int toolCount = 0;
    int feedCount = 0;
    int relicCount = 0;
    int subAgentCount = 0;
};

struct ChatCommandResult {
    bool handled = false;
    bool quit = false;
    bool clearTranscript = false;
    bool toggleThoughts = false;
    bool toggleRaw = false;
    bool toggleTheme = false;
    bool showPrompts = false;
    bool dumpPrompts = false;
    bool copyAll = false;
    bool copyRaw = false;
    std::string title;
    std::string themeName;
    std::vector<std::string> lines;
    std::string composerReplacement;
};

inline ChatCommandResult executeChatCommand(const std::string& command,
                                            const ChatCommandContext& ctx = {}) {
    ChatCommandResult out;
    if (command.empty() || command[0] != '/') return out;
    out.handled = true;

    if (command == "/quit" || command == "/exit") {
        out.quit = true;
        return out;
    }
    if (command == "/clear") {
        out.clearTranscript = true;
        return out;
    }
    if (command == "/thoughts") {
        out.toggleThoughts = true;
        out.title = "thoughts";
        out.lines = {"thought visibility toggled"};
        return out;
    }
    if (command == "/raw") {
        out.toggleRaw = true;
        out.title = "raw";
        out.lines = {"raw stream visibility toggled"};
        return out;
    }
    if (command == "/theme" || command.rfind("/theme ", 0) == 0) {
        out.toggleTheme = true;
        out.title = "theme";
        if (command.size() > 7) {
            out.themeName = trimCommandText(command.substr(7));
            if (out.themeName != "graphite" && out.themeName != "neon") {
                out.toggleTheme = false;
                out.lines = {"expected /theme graphite or /theme neon"};
            }
        }
        return out;
    }
    if (command == "/prompts") {
        out.showPrompts = true;
        return out;
    }
    if (command == "/dump-prompt" || command == "/dp") {
        out.dumpPrompts = true;
        return out;
    }
    if (command == "/cp-all") {
        out.copyAll = true;
        return out;
    }
    if (command == "/cp-raw") {
        out.copyRaw = true;
        return out;
    }
    if (command == "/help" || command == "/commands") {
        out.title = "commands";
        out.lines = {
            "/help, /commands   show this list",
            "/clear             clear visible transcript",
            "/thoughts          toggle thought rows",
            "/raw               toggle raw stream rows",
            "/theme [name]      switch or select graphite / neon",
            "/manifests         inspect active harness surface",
            "/sessions          list recent sessions",
            "/prompts           show captured iteration prompts",
            "/dump-prompt, /dp  write captured prompts to /tmp",
            "/cp-all            copy transcript (file fallback)",
            "/cp-raw            copy raw model output (file fallback)",
            "/quit, /exit       leave chat",
            "Tab                complete command names",
        };
        for (const auto& dynamic : discoverDynamicChatCommands())
            out.lines.push_back(dynamic.name + "  [" + dynamic.kind + "] " + dynamic.description);
        return out;
    }
    if (command == "/manifests") {
        out.title = "active surface";
        out.lines = {
            "manifest  " + (ctx.manifestPath.empty() ? std::string("none") : ctx.manifestPath),
            "harness   " + (ctx.harnessPath.empty() ? std::string("none") : ctx.harnessPath),
            "system    " + (ctx.systemPromptPath.empty() ? std::string("none") : ctx.systemPromptPath),
            "persona   " + (ctx.personaPath.empty() ? std::string("none") : ctx.personaPath),
            "tools     " + std::to_string(ctx.toolCount),
            "feeds     " + std::to_string(ctx.feedCount),
            "relics    " + std::to_string(ctx.relicCount),
            "agents    " + std::to_string(ctx.subAgentCount),
        };
        return out;
    }
    if (command == "/sessions") {
        out.title = "sessions";
        session::SessionManager manager;
        auto sessions = manager.list();
        if (sessions.empty()) {
            out.lines = {"no saved sessions"};
        } else {
            for (size_t i = 0; i < sessions.size() && i < 20; ++i) {
                const auto& s = sessions[i];
                out.lines.push_back(s.id + "  " + s.updated + "  " +
                                    std::to_string(s.turnCount) + " turns");
            }
        }
        return out;
    }

    size_t space = command.find(' ');
    std::string commandName = space == std::string::npos ? command : command.substr(0, space);
    std::string arguments = space == std::string::npos ? std::string() : trimCommandText(command.substr(space + 1));
    auto dynamicCommands = discoverDynamicChatCommands();
    if (const auto* dynamic = findDynamicChatCommand(commandName, dynamicCommands)) {
        out.title = dynamic->name;
        out.composerReplacement = expandDynamicChatCommand(*dynamic, arguments);
        return out;
    }

    out.title = "unknown command";
    out.lines = {command, "use /help for available commands"};
    return out;
}

}  // namespace cortex::mk3::ui::chat
