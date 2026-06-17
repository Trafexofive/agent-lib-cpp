// ─────────────────────────────────────────────────────────────────────────────
// agent-lib-MK3 — Session lifecycle: save, load, dump, clear, undo
// ─────────────────────────────────────────────────────────────────────────────
#include "agent.hpp"
#include <fstream>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace cortex::mk3 {

void Agent::dumpSessionArtifacts() const {
    // AC17 — explicit raw/debug/verbose runs write trace artifacts in CWD
    // so live harness work can find `iterations.md` and `raw.md` next to the
    // run. Skip entirely when the run is not opted into trace dumping.
    bool debugEnabled = env_.count("__DEBUG_MODE__") && env_.at("__DEBUG_MODE__") == "true";
    if (!verbose_ && !raw_ && !debugEnabled) return;

    fs::path cwd = fs::current_path();
    std::error_code ec;
    fs::create_directories(cwd, ec);
    if (ec) return;

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
                f << "\n--- model/runtime output after this prompt (not part of the prompt above) ---\n\n";
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

void Agent::loadSession(const std::string &id) {
    history_.clear();
    contextFeeds_.clear();
    actionResults_.clear();
    auto session = sessionMgr_.load(id);
    for (auto &rec : session.records) {
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

void Agent::saveSession(const std::string &id) {
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
    for (auto &h : history_) {
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




} // namespace cortex::mk3
