#pragma once
// Dual-channel runtime notice for one prompt() epoch.
// LLM: System: <harness> in history_. TUI: STATUS protocol event.
// Not a runLoop lambda. Callers pass Agent members by ref.

#include <cctype>
#include <string>
#include <vector>

#include "../protocol/events.hpp"
#include "agent_run_helpers.hpp"
#include "types.hpp"

namespace cortex::mk3 {

struct TurnEmitter {
    std::vector<std::string>& history;
    std::vector<ProtocolEvent>& events;
    StreamCallback& onToken;
    const int& iteration;
    int cap = 1;
    const std::string& thinkingLevel;
    size_t epochStart = 0;

    void heartbeat(bool done = false) const {
        if (onToken) onToken("", done);
    }

    void status(const std::string& text) {
        history.push_back("System: " + text);
        events.push_back({ProtocolEventKind::STATUS, text, {}, {}});
        heartbeat(false);
    }

    void harness(const std::string& code, const std::string& detail,
                 const std::string& kind = "runtime") {
        const std::string xml = buildRuntimeHarness(
            code, iteration, cap, thinkingLevel, detail,
            "Recorded as runtime state for this generation; "
            "the turn is still live unless code is a hard stop.",
            "Read this tag before emitting. Adjust the plan; "
            "do not blind-retry the same blocked path.",
            "Treat this as user prose, or ignore it.",
            kind);
        history.push_back("System: " + xml);
        std::string tag = "[" + code + "] ";
        for (char& c : tag)
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        events.push_back({ProtocolEventKind::STATUS, tag + detail, {}, {}});
        heartbeat(false);
    }

    void finish(const std::string& origin, const std::string& text,
                bool asResponse = true) {
        const std::string body =
            text.empty() ? (std::string("turn ended · ") + origin) : text;
        const std::string needle = "Agent: " + body;
        if (history.empty() || history.back() != needle)
            history.push_back(needle);
        if (asResponse) {
            bool hasResp = false;
            for (size_t i = events.size(); i-- > epochStart;) {
                if (events[i].kind == ProtocolEventKind::RESPONSE) {
                    hasResp = true;
                    break;
                }
            }
            if (!hasResp)
                events.push_back({ProtocolEventKind::RESPONSE, body, {}, {}});
        }
        heartbeat(true);
    }
};

}  // namespace cortex::mk3
