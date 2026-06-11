// ─────────────────────────────────────────────────────────────────────────────
// agent-lib-MK3 — Session lifecycle: save, load, dump, clear, undo
// ─────────────────────────────────────────────────────────────────────────────
#include "agent.hpp"
#include <fstream>
#include <filesystem>

namespace cortex::mk3 {

void Agent::dumpSessionArtifacts() const {
    // AC17 — never pollute CWD. Artifacts go under the session manager's
    // base directory in a `dumps/<id>/` folder (or `dumps/ephemeral-<ts>/`
    // when the session isn't keyed). Skip entirely if nothing to dump.
    if (iterationPrompts_.empty() && rawLlOutput_.empty()) return;
    if (!verbose_ && !raw_) return;  // only dump when explicitly opted-in

    std::string id = config_.name.empty() ? "anon" : config_.name;
    std::string base = sessionMgr_.baseDir().empty()
                           ? std::string("./dumps")
                           : sessionMgr_.baseDir() + "/dumps";
    std::string ts = session::SessionManager::iso8601();
    // Sanitise timestamp for filename safety (replace ':' with '-').
    for (auto &c : ts) if (c == ':') c = '-';
    std::string dir = base + "/" + id + "-" + ts;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return;  // give up silently rather than crashing the run

    if (!iterationPrompts_.empty()) {
        std::ofstream f(dir + "/iterations.md");
        for (size_t i = 0; i < iterationPrompts_.size(); i++) {
            f << "## Iteration " << (i + 1) << "\n\n";
            f << "### PROMPT\n\n";
            std::istringstream ss(iterationPrompts_[i]);
            std::string line;
            while (std::getline(ss, line))
                f << line << "\n";
            f << "\n";
            if (i < iterationOutputs_.size()) {
                f << "### RESPONSE\n\n";
                f << iterationOutputs_[i] << "\n\n";
            }
        }
    }
    if (!rawLlOutput_.empty()) {
        std::ofstream f(dir + "/raw.md");
        f << rawLlOutput_;
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Session Management
// ═══════════════════════════════════════════════════════════════════════

void Agent::loadSession(const std::string &id) {
    history_.clear();
    contextFeeds_.clear();
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
