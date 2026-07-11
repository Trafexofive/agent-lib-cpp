#pragma once
// =============================================================================
// Cortex MK3 — inkcell UI event contract
// =============================================================================

#include <json/json.h>

#include <string>
#include <vector>

#include "src/core/agent.hpp"

namespace cortex::mk3::ui {

enum class UiEventKind {
    Token,
    Protocol,
    Status,
    AskDialog,
    AskDialogResult,
    TurnDone,
    Error,
    Log,
};

inline const char* uiEventKindName(UiEventKind kind) {
    switch (kind) {
        case UiEventKind::Token:
            return "token";
        case UiEventKind::Protocol:
            return "protocol";
        case UiEventKind::Status:
            return "status";
        case UiEventKind::AskDialog:
            return "ask_dialog";
        case UiEventKind::AskDialogResult:
            return "ask_dialog_result";
        case UiEventKind::TurnDone:
            return "turn_done";
        case UiEventKind::Error:
            return "error";
        case UiEventKind::Log:
            return "log";
    }
    return "unknown";
}

struct UiEvent {
    UiEventKind kind = UiEventKind::Log;
    std::string text;
    std::string id;
    ProtocolEvent protocol;
    Json::Value json;
    bool final = false;

    static UiEvent token(std::string chunk, bool isFinal = false) {
        UiEvent e;
        e.kind = UiEventKind::Token;
        e.text = std::move(chunk);
        e.final = isFinal;
        return e;
    }

    static UiEvent protocolEvent(ProtocolEvent ev) {
        UiEvent e;
        e.kind = UiEventKind::Protocol;
        e.protocol = std::move(ev);
        return e;
    }

    static UiEvent status(std::string text) {
        UiEvent e;
        e.kind = UiEventKind::Status;
        e.text = std::move(text);
        return e;
    }

    static UiEvent error(std::string text) {
        UiEvent e;
        e.kind = UiEventKind::Error;
        e.text = std::move(text);
        return e;
    }

    static UiEvent log(std::string text) {
        UiEvent e;
        e.kind = UiEventKind::Log;
        e.text = std::move(text);
        return e;
    }
};

struct UiSnapshot {
    std::vector<UiEvent> events;
    std::string status;
    bool running = false;
};

}  // namespace cortex::mk3::ui
