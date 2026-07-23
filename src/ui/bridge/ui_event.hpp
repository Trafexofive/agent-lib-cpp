#pragma once
// =============================================================================
// Cortex MK3 — inkcell UI event contract
// =============================================================================

#include <json/json.h>

#include <cstddef>
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
    // Notification = transient operator-facing event card. The chat TUI
    // surfaces these as a small banner above the composer (not part of
    // the persistent transcript). Distinct from Log ([a]ttention detail
    // that lives in the body) and Error (rh/fatal failures). See
    // src/ui/chat/notification.hpp for the renderer. Retry mechanisms,
    // rate-limit warnings, and recoverable tool hiccups land here.
    Notification,
};

inline const char* uiEventKindName(UiEventKind kind) {
    switch (kind) {
        case UiEventKind::Token: return "token";
        case UiEventKind::Protocol: return "protocol";
        case UiEventKind::Status: return "status";
        case UiEventKind::AskDialog: return "ask_dialog";
        case UiEventKind::AskDialogResult: return "ask_dialog_result";
        case UiEventKind::TurnDone: return "turn_done";
        case UiEventKind::Error: return "error";
        case UiEventKind::Log: return "log";
        case UiEventKind::Notification: return "notification";
    }
    return "unknown";
}

struct UiEvent {
    UiEventKind kind = UiEventKind::Log;
    std::string text;
    std::string id;
    ProtocolEvent protocol;
    size_t protocolIndex = 0;
    Json::Value json;
    bool final = false;

    // Notification extras: severity (info/warn/error), source tag (e.g. "retry",
    // "rate-limit"), optional id for de-duplication. The chat TUI collapses
    // batches with the same id+source so retries become one badge.
    std::string severity;     // "info" | "warn" | "error"
    std::string source;       // short label for grouping ("empty-response", "curl-28")
    int attempt = 0;          // retry sequence index
    int maxAttempts = 0;      // total configured attempts

    static UiEvent token(std::string chunk, bool isFinal = false) {
        UiEvent e;
        e.kind = UiEventKind::Token;
        e.text = std::move(chunk);
        e.final = isFinal;
        return e;
    }

    static UiEvent protocolEvent(ProtocolEvent ev, size_t index = 0) {
        UiEvent e;
        e.kind = UiEventKind::Protocol;
        e.protocol = std::move(ev);
        e.protocolIndex = index;
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

    // Retry notification factory. ids are stable per (source + turn); the
    // model collapses successive notifications with the same id into a
    // single badge so a retry sequence becomes one banner.
    static UiEvent notification(std::string source, std::string severity,
                                 std::string text, int attempt = 0,
                                 int maxAttempts = 0, std::string dedupeId = {}) {
        UiEvent e;
        e.kind = UiEventKind::Notification;
        e.source = std::move(source);
        e.severity = std::move(severity);
        e.text = std::move(text);
        e.attempt = attempt;
        e.maxAttempts = maxAttempts;
        e.id = dedupeId; // empty id => always fresh
        return e;
    }
};

struct UiSnapshot {
    std::vector<UiEvent> events;
    std::string status;
    bool running = false;
};

}  // namespace cortex::mk3::ui
