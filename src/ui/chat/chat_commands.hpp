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
    std::string userPath;
    int toolCount = 0;
    int feedCount = 0;
    int relicCount = 0;
    int subAgentCount = 0;
    // Settings · DEV MODE (or CORTEX_DEV_MODE). Gates debug-only slash cmds.
    bool devMode = false;
};

struct ChatCommandResult {
    bool handled = false;
    bool quit = false;
    bool clearTranscript = false;
    bool toggleThoughts = false;
    bool toggleTruncate = false;
    bool toggleRaw = false;
    bool toggleTheme = false;
    bool showPrompts = false;
    bool dumpPrompts = false;
    bool exportChat = false;
    bool exportDump = false;
    bool openArtifacts = false;
    std::string artifactsArgs;
    bool switchModel = false;
    std::string modelSpec;  // "provider/model" or "model" or empty (show)
    bool copyAll = false;
    bool copyRaw = false;
    bool stopLoop = false;
    bool continueLoop = false;  // /continue — silent resume, no user text
    std::string title;
    std::string themeName;
    std::vector<std::string> lines;
    std::string composerReplacement;
};

inline ChatCommandResult devDenied(const char* cmd) {
    ChatCommandResult out;
    out.handled = true;
    out.title = "dev mode";
    out.lines = {
        std::string(cmd) + " requires DEV MODE",
        "enable: Settings → DEV → DEV MODE  (or CORTEX_DEV_MODE=1)",
    };
    return out;
}

inline ChatCommandResult executeChatCommand(const std::string& rawCommand,
                                            const ChatCommandContext& ctx = {}) {
    ChatCommandResult out;
    // Trim + collapse internal runs of spaces so "/help  " / "/model  x" work.
    std::string command = trimCommandText(rawCommand);
    {
        std::string collapsed;
        collapsed.reserve(command.size());
        bool sp = false;
        for (char c : command) {
            if (c == ' ' || c == '\t') {
                if (!sp) collapsed.push_back(' ');
                sp = true;
            } else {
                collapsed.push_back(c);
                sp = false;
            }
        }
        while (!collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();
        command = std::move(collapsed);
    }
    if (command.empty() || command[0] != '/') return out;
    out.handled = true;

    if (command == "/quit" || command == "/exit") {
        out.quit = true;
        return out;
    }
    if (command == "/stop" || command == "/cancel") {
        out.stopLoop = true;
        out.title = "stop";
        out.lines = {"stopping agent loop (g_running=false)"};
        return out;
    }
    // Silent resume: another turn from existing history, no User: inject.
    if (command == "/continue" || command == "/cont") {
        out.continueLoop = true;
        out.title = "continue";
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
    if (command == "/truncate") {
        out.toggleTruncate = true;
        out.title = "truncate";
        out.lines = {"body truncation toggled"};
        return out;
    }
    if (command == "/raw") {
        // Raw stream rows are a normal visibility toggle (not a dump).
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
        if (!ctx.devMode) return devDenied("/prompts");
        out.showPrompts = true;
        return out;
    }
    if (command == "/dump-prompt" || command == "/dp") {
        if (!ctx.devMode) return devDenied(command.c_str());
        out.dumpPrompts = true;
        return out;
    }
    if (command == "/export-chat") {
        if (!ctx.devMode) return devDenied("/export-chat");
        out.exportChat = true;
        return out;
    }
    if (command == "/export-dump") {
        if (!ctx.devMode) return devDenied("/export-dump");
        out.exportDump = true;
        return out;
    }
    // /artifacts [/art] — fullscreen artifact manager (reuses ~/.pi/agent/bin/art).
    if (command == "/artifacts" || command == "/art" ||
        command.rfind("/artifacts ", 0) == 0 || command.rfind("/art ", 0) == 0) {
        out.openArtifacts = true;
        size_t sp = command.find(' ');
        if (sp != std::string::npos)
            out.artifactsArgs = trimCommandText(command.substr(sp + 1));
        out.title = "artifacts";
        return out;
    }
    // /model [provider/model|model] — show or hot-swap cognitive engine.
    if (command == "/model" || command.rfind("/model ", 0) == 0) {
        out.switchModel = true;
        out.title = "model";
        if (command.size() > 7)
            out.modelSpec = trimCommandText(command.substr(7));
        return out;
    }
    if (command == "/cp-all") {
        out.copyAll = true;
        return out;
    }
    if (command == "/cp-raw") {
        if (!ctx.devMode) return devDenied("/cp-raw");
        out.copyRaw = true;
        return out;
    }
    if (command == "/help" || command == "/commands") {
        out.title = "commands";
        out.lines = {
            "/help, /commands   show this list",
            "/clear             clear visible transcript",
            "/thoughts          toggle thought rows",
            "/truncate          toggle long-body truncation (pi-like)",
            "j / k              select block (history focus; viewport follows)",
            "Ctrl-J / Ctrl-K    fine scroll transcript ±1 (also while typing)",
            "↑ / ↓ · PgUp/PgDn  scroll lines / half-page",
            "Home / End         jump transcript top / bottom",
            "/raw               toggle raw stream rows",
            "/theme [name]      switch or select graphite / neon",
            "/manifests         inspect active harness surface",
            "/sessions          list recent sessions",
            "/cp-all            copy transcript (file fallback)",
            "/artifacts, /art   fullscreen artifact manager (.artifacts/)",
            "/model [prov/mod]  show or switch provider/model (live)",
            "body fmt           Settings → CHAT → BODY FMT (json|yaml|raw)",
            "/continue, /cont   resume loop silently (no user text injected)",
            "/stop, /cancel     stop agent loop mid-turn (same as Ctrl-C)",
            "/quit, /exit       leave chat",
            "Tab / Shift-Tab    complete slash cmds (LCP then cycle)",
            "Ctrl-C / Ctrl-X    stop running turn",
        };
        if (ctx.devMode) {
            out.lines.push_back("--- dev mode ---");
            out.lines.push_back("/export-chat       write rendered chat view to /tmp");
            out.lines.push_back("/export-dump       force .cortex/dev dump (iters/raw/history)");
            out.lines.push_back("/prompts           show captured iteration prompts");
            out.lines.push_back("/dump-prompt, /dp  write captured prompts to /tmp");
            out.lines.push_back("/cp-raw            copy raw model output (file fallback)");
        } else {
            out.lines.push_back("dev cmds hidden — enable Settings → DEV MODE");
        }
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
            "user      " + (ctx.userPath.empty() ? std::string("none") : ctx.userPath),
            "tools     " + std::to_string(ctx.toolCount),
            "feeds     " + std::to_string(ctx.feedCount),
            "relics    " + std::to_string(ctx.relicCount),
            "agents    " + std::to_string(ctx.subAgentCount),
            "dev_mode  " + std::string(ctx.devMode ? "on" : "off"),
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
