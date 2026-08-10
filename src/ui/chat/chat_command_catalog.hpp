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

// ────────────────────────────────────────────────────────────────────────
// Composer command batching — /skill:x /prompt:y <user text> in one prompt.
//
// Lets the operator load several skills/prompts into a single submit, each
// tag applying the trailing user text as its $ARGUMENTS, then threads a
// plain instruction alongside. This is the prompt-box generalization of the
// single /foo:skill flow: a batch of command tags + one user ask.
// ────────────────────────────────────────────────────────────────────────
struct ComposerBatchToken {
    const DynamicChatCommand* command = nullptr;  // resolved (null if unknown)
    std::string name;       // e.g. "/manifest:skill" (as typed / canonical)
    std::string kind;       // "skill" | "prompt"
    std::string arguments;  // text that followed this tag (trimmed)
    std::string expanded;   // command.body with $ARGUMENTS substituted
    bool resolved = false;
};

struct ComposerBatch {
    std::vector<ComposerBatchToken> commands;  // in input order
    std::string plainText;   // non-tag user text (the core instruction)
    bool anyCommand = false;
    bool allResolved = true;

    bool empty() const { return commands.empty() && plainText.empty(); }
};

// True if `tok` looks like a /name:kind command tag (e.g. "/manifest:skill",
// "/review:prompt", "/bare" — bare /x matches any kind).
inline bool looksLikeCommandTag(const std::string& tok) {
    if (tok.size() < 2 || tok[0] != '/') return false;
    if (tok.find(' ') != std::string::npos) return false;
    // A /-prefixed token with no whitespace is a command candidate. Allow
    // '/name:skill' and bare '/name' (resolved to whichever kind exists).
    return true;
}

// Parse a composer input into a batch of command tags + trailing plain text.
// Each '/name:kind' (or bare '/name') token captures the text up to the next
// tag as its arguments. The remaining non-tag text becomes plainText.
inline ComposerBatch parseComposerBatch(
    const std::string& input,
    const std::vector<DynamicChatCommand>& commands) {
    ComposerBatch batch;
    if (trimCommandText(input).empty()) return batch;

    auto resolve = [&](const std::string& tag) -> const DynamicChatCommand* {
        size_t colon = tag.find(':');
        if (colon == std::string::npos)
            return findDynamicChatCommand(tag, commands);
        std::string left = tag.substr(1, colon - 1);  // before ':' (after '/')
        std::string right = tag.substr(colon + 1);   // after ':'
        // User writes /kind:name (e.g. /skill:manifest); canonical is /name:kind
        // (e.g. /manifest:skill). Try both orientations.
        if (const auto* c = findDynamicChatCommand("/" + right + ":" + left, commands))
            return c;   // /kind:name → /name:kind
        if (const auto* c = findDynamicChatCommand("/" + left + ":" + right, commands))
            return c;   // canonical /name:kind direct
        if (const auto* c = findDynamicChatCommand("/" + right, commands))
            return c;   // bare /name fallback
        return nullptr;
    };

    // Tokenize: split on whitespace but keep /...:kind tags as atomic tokens;
    // everything between tags belongs to the preceding tag's args, unless no
    // tag has started yet (then it's leading plain text).
    std::string currentPlain;
    ComposerBatchToken* cur = nullptr;

    std::istringstream ss(input);
    std::string tok;
    std::vector<std::string> tokens;
    while (ss >> tok) tokens.push_back(tok);

    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string& tok = tokens[i];
        if (looksLikeCommandTag(tok)) {
            ComposerBatchToken t;
            t.name = tok;
            size_t colon = tok.find(':');
            t.kind = colon == std::string::npos ? "" : tok.substr(colon + 1);
            // Resolve across both /kind:name and /name:kind orientations.
            if (const auto* c = resolve(tok)) {
                t.command = c;
                t.resolved = true;
                t.kind = c->kind;
                t.name = c->name;  // canonical
            }
            if (t.kind.empty() && t.command) t.kind = t.command->kind;
            if (!t.resolved) batch.allResolved = false;
            batch.anyCommand = true;
            batch.commands.push_back(std::move(t));
            cur = &batch.commands.back();
        } else {
            // Free text between two tags → that tag's inline args. Free text
            // after the LAST tag → the primary plain ask (threaded last), not
            // swallowed by the preceding command. A single lone tag keeps its
            // trailing text as $ARGUMENTS for backward compatibility.
            const bool afterLastTag = !batch.commands.empty();
            const bool moreTagsAhead =
                [&]() {
                    for (size_t j = i + 1; j < tokens.size(); ++j)
                        if (looksLikeCommandTag(tokens[j])) return true;
                    return false;
                }();
            if (afterLastTag && !moreTagsAhead && batch.commands.size() > 1) {
                if (!currentPlain.empty()) currentPlain += ' ';
                currentPlain += tok;
            } else if (cur) {
                if (!cur->arguments.empty()) cur->arguments += ' ';
                cur->arguments += tok;
            } else {
                if (!currentPlain.empty()) currentPlain += ' ';
                currentPlain += tok;
            }
        }
    }
    for (auto& t : batch.commands)
        if (t.resolved && t.command)
            t.expanded = expandDynamicChatCommand(*t.command, trimCommandText(t.arguments));
    batch.plainText = trimCommandText(currentPlain);
    return batch;
}

// Compose the batch into a single agent-facing prompt: command bodies first
// (each expanded with its args), then the plain user instruction.
inline std::string composeBatchPrompt(const ComposerBatch& batch) {
    std::ostringstream out;
    for (const auto& t : batch.commands) {
        if (!t.expanded.empty()) {
            if (out.tellp() > 0) out << "\n\n";
            out << t.expanded;
        }
    }
    std::string plain = trimCommandText(batch.plainText);
    if (!plain.empty()) {
        if (out.tellp() > 0) out << "\n\n";
        out << plain;
    }
    return out.str();
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
