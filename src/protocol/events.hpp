#pragma once
// =============================================================================
// Protocol event types — pure domain PODs (foundation F1).
//
// Lived in agent.hpp historically; peeled so UI/bridge/adapters can depend on
// protocol without the Agent runtime. Namespace remains cortex::mk3 for
// source compatibility (no mass rename).
//
// Distinct from protocol::ParsedAction / TokenEvent (streaming parser).
// =============================================================================

#include <map>
#include <string>

namespace cortex::mk3 {

struct ProtocolAction {
    std::string type, name, id, body;
    bool sync = true;
    std::string mode = "sync";
    std::map<std::string, std::string> modifiers;
};

struct ProtocolResult {
    std::string id;
    bool ok = true;
    std::string summary;
    std::string toolName;
    int exitCode = 0;
    double elapsedMs = 0;
    size_t outputBytes = 0;
};

enum class ProtocolEventKind { THOUGHT, ACTION, RESULT, RESPONSE, STATUS, RETRY };

struct ProtocolEvent {
    ProtocolEventKind kind = ProtocolEventKind::THOUGHT;
    std::string text;
    ProtocolAction action;
    ProtocolResult result;
};

inline const char* protocolEventKindName(ProtocolEventKind k) {
    switch (k) {
        case ProtocolEventKind::THOUGHT: return "thought";
        case ProtocolEventKind::ACTION: return "action";
        case ProtocolEventKind::RESULT: return "result";
        case ProtocolEventKind::RESPONSE: return "response";
        case ProtocolEventKind::STATUS: return "status";
        case ProtocolEventKind::RETRY: return "retry";
    }
    return "unknown";
}

}  // namespace cortex::mk3
