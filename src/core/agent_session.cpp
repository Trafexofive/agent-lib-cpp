// ─────────────────────────────────────────────────────────────────────────────
// agent-lib-MK3 — Session lifecycle: save, load, dump, clear, undo
// ─────────────────────────────────────────────────────────────────────────────
#include "agent.hpp"
#include <fstream>
#include <filesystem>

namespace cortex {
namespace mk3 {

void Agent::dumpSessionArtifacts() const {
    // iterations.md — full prompt + response + results per iteration
    if (!iterationPrompts_.empty()) {
        std::ofstream f("iterations.md");
        for (size_t i = 0; i < iterationPrompts_.size(); i++) {
            f << "## Iteration " << (i + 1) << "\n\n";
            f << "### PROMPT\n\n";
            std::istringstream ss(iterationPrompts_[i]);
            std::string line;
            while (std::getline(ss, line))
                f << line << "\n";
            f << "\n";
            // Append response if we have it
            if (i < iterationOutputs_.size()) {
                f << "### RESPONSE\n\n";
                f << iterationOutputs_[i] << "\n\n";
            }
        }
    }
    // raw.md — complete raw LLM stream with injected <result> tags
    if (!rawLlOutput_.empty()) {
        std::ofstream f("raw.md");
        f << rawLlOutput_;
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Session Management
// ═══════════════════════════════════════════════════════════════════════

void Agent::loadSession(const std::string &id) {
    history_.clear();
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
}

void Agent::saveSession(const std::string &id) {
    Session session =
        sessionMgr_.create(id, config_.name, config_.model, "deepseek");
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



} // namespace mk3
} // namespace cortex
