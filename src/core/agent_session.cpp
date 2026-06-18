// ─────────────────────────────────────────────────────────────────────────────
// agent-lib-MK3 — Session lifecycle: save, load, dump, clear, undo
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

#include "agent.hpp"

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
    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) : fs::temp_directory_path();
    return base / ".cortex" / "state" / (safeCheckpointPart(sessionId) + ".json");
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

void Agent::dumpSessionArtifacts() const {
    // AC17 — explicit raw/debug/verbose runs write trace artifacts in CWD
    // so live harness work can find `iterations.md` and `raw.md` next to the
    // run. Skip entirely when the run is not opted into trace dumping.
    bool debugEnabled = env_.count("__DEBUG_MODE__") && env_.at("__DEBUG_MODE__") == "true";
    if (!verbose_ && !raw_ && !debugEnabled)
        return;

    fs::path cwd = fs::current_path();
    std::error_code ec;
    fs::create_directories(cwd, ec);
    if (ec)
        return;

    if (!iterationPrompts_.empty()) {
        std::ofstream f(cwd / "iterations.md");
        for (size_t i = 0; i < iterationPrompts_.size(); i++) {
            f << "## Iteration " << (i + 1) << "\n\n";
            f << "### PROMPT\n\n";
            std::istringstream ss(iterationPrompts_[i]);
            std::string line;
            while (std::getline(ss, line))
                f << line << "\n";
            f << "\n";
            if (i < iterationOutputs_.size()) {
                f << "\n--- model/runtime output after this prompt (not part of the prompt above) "
                     "---\n\n";
                f << iterationOutputs_[i] << "\n\n";
            }
        }
        if (!subAgentTraces_.empty()) {
            f << "# Delegated Agent Traces\n\n";
            for (const auto& trace : subAgentTraces_) {
                f << trace << "\n";
            }
        }
    }

    // Always create raw.md for explicit trace runs. If the provider produced no
    // bytes, an empty file is still the correct signal that dumping ran.
    std::ofstream raw(cwd / "raw.md");
    raw << rawLlOutput_;

    // Keep stdout clean, but make trace location discoverable for harness work.
    std::cerr << "[trace] wrote " << cwd << "\n";
}

// ═══════════════════════════════════════════════════════════════════════
// Session Management
// ═══════════════════════════════════════════════════════════════════════

void Agent::loadSession(const std::string& id) {
    history_.clear();
    contextFeeds_.clear();
    actionResults_.clear();
    auto session = sessionMgr_.load(id);
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
}

void Agent::saveSession(const std::string& id) {
    // AC04 — preserve `created` timestamp across saves by loading-then-merging
    //        instead of unconditionally calling create().
    // AC14 — use the agent's actual provider rather than hardcoded "deepseek".
    Session session;
    if (sessionMgr_.exists(id)) {
        session = sessionMgr_.load(id);
        // Update mutable fields; keep `created`/`id` stable.
        session.agentName = config_.name;
        session.model = config_.model;
        session.provider = config_.provider;
        session.updated = session::SessionManager::iso8601();
        session.records.clear();
    } else {
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
    if (!fs::exists(path))
        return;
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
