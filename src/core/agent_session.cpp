// ─────────────────────────────────────────────────────────────────────────────
// agent-lib-MK3 — Session lifecycle: save, load, dump, clear, undo
// ─────────────────────────────────────────────────────────────────────────────
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <unistd.h>

#include "agent.hpp"
#include "agent_catalog.hpp"
#include "../session/manager.hpp"

namespace fs = std::filesystem;

namespace cortex::mk3 {

static std::string safeCheckpointPart(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_' || c == '.';
        out.push_back(ok ? c : '_');
    }
    return out.empty() ? "default" : out;
}

static fs::path stateCheckpointPath(const std::string& sessionId) {
    // Stable root (same policy as SessionManager) — never CWD.
    // CWD-local .cortex/state was unreadable after chdir / other-repo launches.
    fs::path dir = session::defaultStateDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / (safeCheckpointPart(sessionId) + ".json");
}

// Read fallback for pre-fix CWD-local checkpoints.
static fs::path legacyStateCheckpointPath(const std::string& sessionId) {
    std::error_code ec;
    return fs::current_path(ec) / ".cortex" / "state" /
           (safeCheckpointPart(sessionId) + ".json");
}

static Json::Value stringSetToJson(const std::set<std::string>& values) {
    Json::Value root(Json::arrayValue);
    for (const auto& v : values)
        root.append(v);
    return root;
}

static Json::Value stringVecToJson(const std::vector<std::string>& values) {
    Json::Value root(Json::arrayValue);
    for (const auto& v : values)
        root.append(v);
    return root;
}

static std::set<std::string> stringSetFromJson(const Json::Value& root) {
    std::set<std::string> values;
    if (!root.isArray())
        return values;
    for (const auto& v : root)
        values.insert(v.asString());
    return values;
}

std::string Agent::devDumpDirectory() const {
    // Always under .cortex/dev — never bare $CORTEX_HOME/dev (that polluted
    // the repo root as agent-lib-cpp/dev/ and confused operators).
    // Priority: $CORTEX_HOME/.cortex/dev → ~/.cortex/dev → CWD/.cortex/dev.
    // CWD was first historically ("lazy open") but that dropped 33MB+ dumps
    // into working trees; home-first keeps repos clean. Bare-CWD copies stay
    // opt-in via CORTEX_DEV_CWD_COPIES=1 (see dumpSessionArtifacts).
    std::error_code ec;
    fs::path base;
    if (const char* ch = std::getenv("CORTEX_HOME")) {
        if (ch[0] != '\0') base = fs::path(ch) / ".cortex" / "dev";
    }
    if (base.empty()) {
        const char* userHome = std::getenv("HOME");
        if (userHome && userHome[0])
            base = fs::path(userHome) / ".cortex" / "dev";
    }
    if (base.empty()) {
        fs::path cwd = fs::current_path(ec);
        if (!ec) base = cwd / ".cortex" / "dev";
    }
    std::string id = lastSessionId_;
    if (id.empty())
        id = "ephemeral-" + std::to_string(static_cast<long long>(::getpid()));
    for (char& c : id) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.'))
            c = '_';
    }
    return (base / id).string();
}

void Agent::dumpSessionArtifacts(bool force) const {
    // Trace dumps when any of: force (/export-dump), verbose, raw, __DEBUG_MODE__,
    // runtime.dev_mode, env CORTEX_DEV_MODE (belt — hub-launched agents may miss setDevMode).
    bool debugEnabled = env_.count("__DEBUG_MODE__") && env_.at("__DEBUG_MODE__") == "true";
    bool envDev = false;
    if (const char* e = std::getenv("CORTEX_DEV_MODE")) {
        std::string v = e;
        envDev = !(v.empty() || v == "0" || v == "false" || v == "FALSE");
    }
    bool devEnabled =
        devMode_ || envDev || (env_.count("__DEV_MODE__") && env_.at("__DEV_MODE__") == "true");
    if (!force && !verbose_ && !raw_ && !debugEnabled && !devEnabled)
        return;

    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (ec) cwd = ".";

    // Primary sink: per-session dev dir under CWD/.cortex/dev (or CORTEX_HOME).
    fs::path dumpDir = devDumpDirectory();
    fs::create_directories(dumpDir, ec);
    if (ec) dumpDir = cwd;
    lastDevDumpDir_ = dumpDir.string();

    auto writeIterations = [&](const fs::path& path) {
        std::ofstream f(path);
        if (!f) return;
        f << "# Cortex iterations dump\n";
        f << "# agent=" << config_.name << " provider=" << config_.provider
          << " model=" << config_.model << "\n";
        f << "# session=" << (lastSessionId_.empty() ? "(none)" : lastSessionId_) << "\n";
        f << "# This PROMPT block is exactly what was assembled for the LLM.\n\n";
        for (size_t i = 0; i < iterationPrompts_.size(); i++) {
            f << "## Iteration " << (i + 1) << "\n\n";
            f << "### PROMPT (LLM-facing)\n\n```\n";
            f << iterationPrompts_[i];
            if (!iterationPrompts_[i].empty() && iterationPrompts_[i].back() != '\n') f << "\n";
            f << "```\n\n";
            if (i < iterationOutputs_.size()) {
                f << "### MODEL/RUNTIME OUTPUT (after prompt; not in prompt above)\n\n```\n";
                f << iterationOutputs_[i];
                if (!iterationOutputs_[i].empty() && iterationOutputs_[i].back() != '\n') f << "\n";
                f << "```\n\n";
            }
        }
        if (!subAgentTraces_.empty()) {
            f << "# Delegated Agent Traces\n\n";
            for (const auto& trace : subAgentTraces_)
                f << trace << "\n";
        }
    };

    auto writeHistory = [&](const fs::path& path) {
        std::ofstream f(path);
        if (!f) return;
        f << "# Rendered agent history_ (User/Parent/Agent/System lines)\n\n";
        for (size_t i = 0; i < history_.size(); ++i)
            f << (i + 1) << ". " << history_[i] << "\n\n";
    };

    auto writeRaw = [&](const fs::path& path) {
        std::ofstream raw(path);
        if (!raw) return;
        raw << rawLlOutput_;
    };

    auto writeProtocol = [&](const fs::path& path) {
        std::ofstream f(path);
        if (!f) return;
        f << "# Protocol events (ordered)\n\n";
        f << "event_count=" << protocolEvents_.size() << "\n";
        f << "history_lines=" << history_.size() << "\n";
        f << "iteration_outputs=" << iterationOutputs_.size() << "\n\n";
        if (protocolEvents_.empty()) {
            f << "_(protocolEvents_ empty — falling back to history scan)_\n\n";
            // History still has the run when events were cleared (retry wipe,
            // ephemeral child, or dump after a path that reset the stream).
            size_t n = 0;
            for (const auto& h : history_) {
                if (h.find("<action") == std::string::npos &&
                    h.find("<result") == std::string::npos &&
                    h.find("<response") == std::string::npos &&
                    h.rfind("System: ", 0) != 0 &&
                    h.rfind("User: ", 0) != 0 &&
                    h.rfind("Agent: ", 0) != 0)
                    continue;
                f << "## history[" << n++ << "]\n";
                f << h << "\n\n";
            }
            if (!iterationOutputs_.empty()) {
                f << "# Iteration outputs (raw+runtime)\n\n";
                for (size_t i = 0; i < iterationOutputs_.size(); ++i) {
                    f << "## iter " << (i + 1) << "\n";
                    f << iterationOutputs_[i] << "\n\n";
                }
            }
            return;
        }
        for (size_t i = 0; i < protocolEvents_.size(); ++i) {
            const auto& pe = protocolEvents_[i];
            f << "## [" << i << "] ";
            switch (pe.kind) {
                case ProtocolEventKind::THOUGHT: f << "THOUGHT\n"; break;
                case ProtocolEventKind::ACTION: f << "ACTION " << pe.action.type << " "
                                                  << pe.action.name << " #" << pe.action.id << "\n";
                    f << pe.action.body << "\n";
                    break;
                case ProtocolEventKind::RESULT:
                    f << "RESULT #" << pe.result.id << (pe.result.ok ? " ok" : " err")
                      << " tool=" << pe.result.toolName << "\n";
                    f << pe.result.summary << "\n";
                    break;
                case ProtocolEventKind::RESPONSE: f << "RESPONSE\n"; break;
                case ProtocolEventKind::STATUS: f << "STATUS\n"; break;
                default: f << "OTHER\n"; break;
            }
            if (!pe.text.empty()) f << pe.text << "\n";
            f << "\n";
        }
    };

    writeIterations(dumpDir / "iterations.md");
    writeRaw(dumpDir / "raw.md");
    writeHistory(dumpDir / "history.md");
    writeProtocol(dumpDir / "protocol.md");

    // Pointer file only — never std::cerr here. TUI owns the alternate screen;
    // any stderr byte mid-frame corrupts cells (looks like "eaten spaces" /
    // overlapping blocks). Operators find dumps via .cortex/dev/ or WHERE.
    {
        std::ofstream where(dumpDir / "WHERE.txt");
        if (where) {
            where << "cortex dev dump\n"
                  << "dir=" << dumpDir.string() << "\n"
                  << "agent=" << config_.name << "\n"
                  << "session=" << (lastSessionId_.empty() ? "(none)" : lastSessionId_)
                  << "\n";
        }
    }

    // Optional CWD convenience copies — OFF by default; enable with
    // CORTEX_DEV_CWD_COPIES=1. Default polluted inkcell/ with iterations.md.
    if (const char* copies = std::getenv("CORTEX_DEV_CWD_COPIES")) {
        std::string v = copies;
        if (!(v.empty() || v == "0" || v == "false")) {
            writeIterations(cwd / "iterations.md");
            writeRaw(cwd / "raw.md");
            writeHistory(cwd / "history.md");
        }
    }

    // NEVER write to stderr from dumps. TUI alt-screen + any cerr = corruption.
    // Path is in lastDevDumpDir_ / WHERE.txt for operators.
    (void)devEnabled;
}

// ═══════════════════════════════════════════════════════════════════════
// Session Management
// ═══════════════════════════════════════════════════════════════════════

void Agent::loadSession(const std::string& id) {
    history_.clear();
    contextFeeds_.clear();
    actionResults_.clear();
    auto session = sessionMgr_.load(id);
    // Vet-fix: legacy sessions saved before 48582e5 carried an empty
    // agent_name / model / provider field. The on-disk identity the
    // operator sees in the Sessions page is empty until the next save;
    // most operators never hit "save" again before closing the chat,
    // so the empty value sticks. Backfill persisting here — the loaded
    // view shows the right name immediately, and the on-disk file is
    // corrected in the same call so subsequent resumes and listings
    // are clean.
    bool backfilled = false;
    if (session.agentName.empty() && !config_.name.empty()) {
        session.agentName = config_.name;
        backfilled = true;
    }
    if (session.model.empty() && !config_.model.empty()) {
        session.model = config_.model;
        backfilled = true;
    }
    if (session.provider.empty() && !config_.provider.empty()) {
        session.provider = config_.provider;
        backfilled = true;
    }
    if (backfilled) {
        session.updated = session::SessionManager::iso8601();
        sessionMgr_.save(session);
    }
    for (auto& rec : session.records) {
        std::string prefix;
        switch (rec.role) {
            case SessionRecord::USER:
                prefix = "User: ";
                break;
            case SessionRecord::AGENT:
                prefix = "Agent: ";
                break;
            default:
                prefix = "System: ";
                break;
        }
        history_.push_back(prefix + rec.content);
    }
    // AC18 — restore LLM-injected feeds so resumed sessions don't lose context.
    contextFeeds_ = session.contextFeeds;

    // Vet-fix: session JSON can have records:[] while the sibling
    // `.state.json` still holds full history_ (exit flush used to wipe
    // records via the wrong Agent). Prefer checkpoint history when the
    // session file has nothing usable, then re-save session so resume
    // paths that only read session.records also see content.
    if (history_.empty()) {
        loadStateCheckpoint(id);
        if (!history_.empty()) {
            // Rebuild session.records from recovered history_ without
            // going through the empty-history early-return path.
            session.records.clear();
            for (const auto& h : history_) {
                SessionRecord rec;
                rec.timestamp = session::SessionManager::iso8601();
                if (h.rfind("User: ", 0) == 0) {
                    rec.role = SessionRecord::USER;
                    rec.content = h.substr(6);
                } else if (h.rfind("Agent: ", 0) == 0) {
                    rec.role = SessionRecord::AGENT;
                    rec.content = h.substr(7);
                } else if (h.rfind("System: ", 0) == 0) {
                    rec.role = SessionRecord::SYSTEM;
                    rec.content = h.substr(8);
                } else {
                    rec.role = SessionRecord::SYSTEM;
                    rec.content = h;
                }
                session.records.push_back(std::move(rec));
            }
            session.contextFeeds = contextFeeds_;
            session.updated = session::SessionManager::iso8601();
            sessionMgr_.save(session);
        }
    }
}

void Agent::saveSession(const std::string& id) {
    // AC04 — preserve `created` timestamp across saves by loading-then-merging
    //        instead of unconditionally calling create().
    // AC14 — use the agent's actual provider rather than hardcoded "deepseek".
    // Vet-fix: save on every flush, not gated on content. The earlier
    // "empty session litter" guard was correct for CLI-launch phantom
    // file creation — but it also caused Ctrl-C and exit-time saves to
    // silently drop the user's captured text whenever the agent was
    // cancelled mid-prompt (history_ may have only a "User:" line and
    // a short cancellation note). Operators expect: anything they typed
    // lands on disk. We gate on id-only here, and lean on the
    // persistSessionMetadata() main.cpp gate to prevent file creation
    // from bare launches.
    if (id.empty()) return;

    Session session;
    if (sessionMgr_.exists(id)) {
        session = sessionMgr_.load(id);
        // Identity merge:
        //  - never demote a real on-disk agent to builtin
        //  - upgrade empty/builtin → live config when live is a real agent
        //  - always refresh model/provider (live /model switches)
        {
            const bool livePlaceholder = isPlaceholderAgentName(config_.name);
            const bool diskReal = !isPlaceholderAgentName(session.agentName);
            const std::string diskMp =
                session.metadata.count("manifest_path") ? session.metadata.at("manifest_path")
                                                         : std::string{};
            // Never steal a session's agent. Wrong-slot save (hub default after
            // restart) used to overwrite coder → default.
            const bool sameSlot =
                diskMp.empty() || config_.manifestPath.empty() ||
                diskMp == config_.manifestPath;
            if (sameSlot && !livePlaceholder)
                session.agentName = config_.name;
            else if (!diskReal && session.agentName.empty())
                session.agentName = config_.name;
        }
        if (!config_.model.empty()) session.model = config_.model;
        if (!config_.provider.empty()) session.provider = config_.provider;
        // Absolute manifest path is the resume lock. Always write when live has it
        // and we're on the same slot (or disk has none yet).
        {
            const std::string diskMp =
                session.metadata.count("manifest_path") ? session.metadata.at("manifest_path")
                                                         : std::string{};
            if (!config_.manifestPath.empty() &&
                (diskMp.empty() || diskMp == config_.manifestPath))
                session.metadata["manifest_path"] = config_.manifestPath;
            else if (diskMp.empty() && !isPlaceholderAgentName(session.agentName)) {
                std::string err;
                std::string resolved =
                    catalog::resolveAgent(session.agentName, config_.manifestDir, &err);
                if (!resolved.empty())
                    session.metadata["manifest_path"] = resolved;
            }
        }
        session.updated = session::SessionManager::iso8601();
        // Vet-fix: never replace a non-empty on-disk transcript with an
        // empty in-memory history_. Exit flush used to call saveSession on
        // the *CLI* Agent after a hub hot-swap to brainstormer — that
        // agent's history_ was empty, so records:[] wiped the real chat.
        // State checkpoint still had the full history; session file did not.
        if (history_.empty() && !session.records.empty()) {
            sessionMgr_.save(session);  // metadata/touch only
            return;
        }
        session.records.clear();
    } else {
        // No file yet — refuse to create a zero-record orphan. Caller that
        // has real content (seedUserPrompt / end of prompt) will have
        // history_ non-empty and fall through below.
        if (history_.empty() && contextFeeds_.empty()) return;
        session = sessionMgr_.create(id, config_.name, config_.model, config_.provider);
    }
    for (auto& h : history_) {
        SessionRecord rec;
        rec.timestamp = session::SessionManager::iso8601();
        if (h.rfind("User: ", 0) == 0) {
            rec.role = SessionRecord::USER;
            rec.content = h.substr(6);
        } else if (h.rfind("Agent: ", 0) == 0) {
            rec.role = SessionRecord::AGENT;
            rec.content = h.substr(7);
        } else {
            rec.role = SessionRecord::SYSTEM;
            // Strip "System: " prefix to prevent doubling on load
            if (h.rfind("System: ", 0) == 0)
                rec.content = h.substr(8);
            else
                rec.content = h;
        }
        session.records.push_back(rec);
    }
    // AC18 — round-trip the LLM-injected context feeds.
    session.contextFeeds = contextFeeds_;
    if (!config_.manifestPath.empty()) {
        auto it = session.metadata.find("manifest_path");
        if (it == session.metadata.end() || it->second.empty())
            session.metadata["manifest_path"] = config_.manifestPath;
    }
    sessionMgr_.save(session);
}

Json::Value Agent::stateCheckpointJson() const {
    Json::Value root;
    root["format"] = "cortex-agent-state";
    root["version"] = 1;
    root["agent_name"] = config_.name;
    root["provider"] = config_.provider;
    root["model"] = config_.model;

    Json::Value history(Json::arrayValue);
    for (const auto& h : history_)
        history.append(h);
    root["history"] = history;
    root["context_feeds"] = stringVecToJson(contextFeeds_);
    root["feeds"] = stringSetToJson(feeds_);
    root["relics"] = stringSetToJson(relics_);
    root["disabled_builtins"] = stringSetToJson(disabledBuiltins_);

    Json::Value pinned(Json::objectValue);
    for (const auto& [path, entry] : pinned_) {
        Json::Value item;
        item["displayPath"] = entry.displayPath;
        item["content"] = entry.content;
        item["bytes"] = (Json::UInt64)entry.bytes;
        pinned[path] = item;
    }
    root["pinned"] = pinned;

    Json::Value peeking(Json::objectValue);
    for (const auto& [path, entry] : peeking_) {
        Json::Value item;
        item["displayPath"] = entry.displayPath;
        item["content"] = entry.content;
        item["bytes"] = (Json::UInt64)entry.bytes;
        item["cyclesRemaining"] = entry.cyclesRemaining;
        peeking[path] = item;
    }
    root["peeking"] = peeking;

    Json::Value executedActions(Json::objectValue);
    for (const auto& [key, value] : executedActions_)
        executedActions[key] = value;
    root["executed_actions"] = executedActions;

    Json::Value actionResults(Json::objectValue);
    for (const auto& [key, value] : actionResults_)
        actionResults[key] = value;
    root["action_results"] = actionResults;

    Json::Value env(Json::objectValue);
    for (const auto& [key, value] : env_)
        env[key] = value;
    root["env"] = env;

    Json::Value subAgents(Json::objectValue);
    for (const auto& [name, agent] : subAgents_) {
        if (agent)
            subAgents[name] = agent->stateCheckpointJson();
    }
    root["sub_agents"] = subAgents;

    return root;
}

void Agent::loadStateCheckpointJson(const Json::Value& root) {
    if (!root.isObject())
        return;

    if (root.isMember("history") && root["history"].isArray()) {
        history_.clear();
        for (const auto& h : root["history"])
            history_.push_back(h.asString());
    }
    if (root.isMember("context_feeds") && root["context_feeds"].isArray()) {
        contextFeeds_.clear();
        for (const auto& v : root["context_feeds"])
            contextFeeds_.push_back(v.asString());
    }
    if (root.isMember("feeds") && root["feeds"].isArray())
        feeds_ = stringSetFromJson(root["feeds"]);
    if (root.isMember("relics") && root["relics"].isArray())
        relics_ = stringSetFromJson(root["relics"]);
    if (root.isMember("disabled_builtins") && root["disabled_builtins"].isArray())
        disabledBuiltins_ = stringSetFromJson(root["disabled_builtins"]);

    if (root.isMember("pinned") && root["pinned"].isObject()) {
        pinned_.clear();
        for (const auto& path : root["pinned"].getMemberNames()) {
            const Json::Value& item = root["pinned"][path];
            PinnedEntry entry;
            entry.displayPath = item.get("displayPath", path).asString();
            entry.content = item.get("content", "").asString();
            entry.bytes = item.get("bytes", entry.content.size()).asUInt64();
            pinned_[path] = entry;
        }
    }

    if (root.isMember("peeking") && root["peeking"].isObject()) {
        peeking_.clear();
        for (const auto& path : root["peeking"].getMemberNames()) {
            const Json::Value& item = root["peeking"][path];
            PeekEntry entry;
            entry.displayPath = item.get("displayPath", path).asString();
            entry.content = item.get("content", "").asString();
            entry.bytes = item.get("bytes", entry.content.size()).asUInt64();
            entry.cyclesRemaining = item.get("cyclesRemaining", 1).asInt();
            peeking_[path] = entry;
        }
    }

    if (root.isMember("executed_actions") && root["executed_actions"].isObject()) {
        executedActions_.clear();
        for (const auto& key : root["executed_actions"].getMemberNames())
            executedActions_[key] = root["executed_actions"][key].asString();
    }
    if (root.isMember("action_results") && root["action_results"].isObject()) {
        actionResults_.clear();
        for (const auto& key : root["action_results"].getMemberNames())
            actionResults_[key] = root["action_results"][key];
    }
    if (root.isMember("env") && root["env"].isObject()) {
        env_.clear();
        for (const auto& key : root["env"].getMemberNames())
            env_[key] = root["env"][key].asString();
    }

    if (root.isMember("sub_agents") && root["sub_agents"].isObject()) {
        for (const auto& name : root["sub_agents"].getMemberNames()) {
            auto it = subAgents_.find(name);
            if (it != subAgents_.end() && it->second)
                it->second->loadStateCheckpointJson(root["sub_agents"][name]);
        }
    }
}

void Agent::loadStateCheckpoint(const std::string& sessionId) {
    fs::path path = stateCheckpointPath(sessionId);
    if (!fs::exists(path)) {
        // Pre-fix: checkpoints lived under CWD/.cortex/state/
        fs::path legacy = legacyStateCheckpointPath(sessionId);
        if (fs::exists(legacy))
            path = legacy;
        else
            return;
    }
    try {
        std::ifstream f(path);
        Json::Value root;
        Json::CharReaderBuilder reader;
        std::string errs;
        if (Json::parseFromStream(reader, f, &root, &errs))
            loadStateCheckpointJson(root);
    } catch (...) {
    }
}

void Agent::saveStateCheckpoint(const std::string& sessionId) const {
    try {
        // Vet-fix: never create a standalone checkpoint file. The
        // `<id>.state.json` shadow needs a sibling records file, or the
        // listing duplication bug comes back and Sessions shows phantom
        // checkpoint rows. saveSession creates both atomically; this
        // function only updates the checkpoint once the records file
        // exists.
        if (!sessionMgr_.exists(sessionId))
            return;
        fs::path path = stateCheckpointPath(sessionId);
        fs::create_directories(path.parent_path());
        std::ofstream f(path.string() + ".tmp");
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "  ";
        f << Json::writeString(writer, stateCheckpointJson());
        f.close();
        fs::rename(path.string() + ".tmp", path);
    } catch (...) {
    }
}

void Agent::clearHistory() {
    history_.clear();
    executedActions_.clear();
    actionResults_.clear();
    contextFeeds_.clear();
    protocolActions_.clear();
    protocolResults_.clear();
    protocolEvents_.clear();
    responseOutput_.clear();
    thoughtOutput_.clear();
    rawLlOutput_.clear();
    bareTextReminded_ = false;
}

void Agent::undoLastInteraction() {
    if (history_.size() >= 2) {
        history_.pop_back();
        history_.pop_back();
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Tool Management
// ═══════════════════════════════════════════════════════════════════════

}  // namespace cortex::mk3
