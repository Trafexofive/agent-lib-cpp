#pragma once
// Chat-local slash command controller. Independent of src/tui and inkcell.

#include <sstream>
#include <string>
#include <vector>

#include "src/session/manager.hpp"

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
    std::string title;
    std::vector<std::string> lines;
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
    if (command == "/help" || command == "/commands") {
        out.title = "commands";
        out.lines = {
            "/help, /commands   show this list",
            "/clear             clear visible transcript",
            "/thoughts          toggle thought rows",
            "/raw               toggle raw stream rows",
            "/manifests         inspect active harness surface",
            "/sessions          list recent sessions",
            "/quit, /exit       leave chat",
        };
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

    out.title = "unknown command";
    out.lines = {command, "use /help for available commands"};
    return out;
}

}  // namespace cortex::mk3::ui::chat
