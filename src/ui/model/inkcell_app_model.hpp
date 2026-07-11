#pragma once
// Domain model for the inkcell AgentShell. Drawing stays out of this file.

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "inkcell/widgets/scroll_view.hpp"
#include "inkcell/widgets/textarea.hpp"
#include "src/core/agent.hpp"
#include "src/ui/bridge/agent_bridge.hpp"

namespace cortex::mk3::ui {

struct InkcellAppConfig {
    std::string agentName;
    std::string provider;
    std::string model;
    std::string manifestPath;
    std::string sessionId;
    bool ephemeral = false;
};

enum class PageState { Loading, Populated, Empty, Error };
enum class TimelineKind { User, Status, Stream, Thought, Action, Result, Response, Final, Error, Log };

inline bool snapshotMode() {
    const char* s = std::getenv("MK3_TUI_SNAPSHOT");
    return (s && s[0]) || !isatty(STDOUT_FILENO);
}

inline std::string nonempty(const std::string& value, const std::string& fallback) {
    return value.empty() ? fallback : value;
}

inline std::vector<std::string> splitDisplayLines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) out.push_back(line);
    if (out.empty()) out.push_back("");
    return out;
}

struct TimelineRow {
    TimelineKind kind = TimelineKind::Log;
    std::string title;
    std::string body;
    bool ok = true;
};

inline const char* kindGlyph(TimelineKind k) {
    switch (k) {
        case TimelineKind::User:
            return ">";
        case TimelineKind::Status:
            return "◐";
        case TimelineKind::Stream:
            return "…";
        case TimelineKind::Thought:
            return "·";
        case TimelineKind::Action:
            return "◆";
        case TimelineKind::Result:
            return "✓";
        case TimelineKind::Response:
            return "▸";
        case TimelineKind::Final:
            return "■";
        case TimelineKind::Error:
            return "✗";
        case TimelineKind::Log:
            return " ";
    }
    return " ";
}

struct ShellModel {
    std::vector<TimelineRow> rows;
    std::vector<std::string> eventLog;
    std::string raw;
    std::string finalText;
    std::string status = "idle";
    PageState timelineState = PageState::Empty;
    bool running = false;
    bool done = false;
    bool failed = false;
    bool showThoughts = false;
    bool showRaw = false;
    int tokenBytes = 0;
    int actionCount = 0;
    int resultCount = 0;
    int pendingOps = 0;
    int wakeCount = 0;
    int routeTicks = 0;
    std::string activePage = "Agent";
    std::string pendingSubmit;  // set by composer; app loop drains
    inkcell::widgets::TextAreaState composer;
    mutable inkcell::widgets::ScrollViewState transcriptView;
    mutable inkcell::widgets::ScrollViewState inspectorView;

    ShellModel() {
        composer.focused = true;
        composer.value.clear();
        rebuildViews();
    }

    void routeTo(std::string page) {
        activePage = std::move(page);
        routeTicks = snapshotMode() ? 0 : 10;
    }

    void tickRoute() {
        if (routeTicks > 0) --routeTicks;
    }

    int transitionInset() const {
        if (snapshotMode() || routeTicks <= 0) return 0;
        return std::max(0, routeTicks / 3);
    }

    void pushRow(TimelineRow row) {
        rows.push_back(std::move(row));
        timelineState = PageState::Populated;
        rebuildViews();
    }

    void setStreamProgress(int bytes) {
        tokenBytes = bytes;
        if (!rows.empty() && rows.back().kind == TimelineKind::Stream) {
            rows.back().title = "stream";
            rows.back().body = std::to_string(bytes) + " bytes received";
        } else {
            rows.push_back({TimelineKind::Stream, "stream", std::to_string(bytes) + " bytes received", true});
            timelineState = PageState::Populated;
        }
        rebuildViews();
    }

    void rebuildViews() {
        transcriptView.lines.clear();
        if (rows.empty()) {
            transcriptView.lines = {
                "Nothing here yet.",
                "Type a prompt below and press Enter to send.",
                "Esc focuses timeline scroll · i returns to composer.",
            };
            timelineState = PageState::Empty;
        } else {
            for (const auto& row : rows) {
                if (row.kind == TimelineKind::Thought && !showThoughts) continue;
                if (row.kind == TimelineKind::Stream && showRaw) continue;
                std::string head = std::string(kindGlyph(row.kind)) + " " + row.title;
                if (row.kind == TimelineKind::Result && !row.ok) head = "✗ " + row.title;
                transcriptView.lines.push_back(head);
                for (const auto& line : splitDisplayLines(row.body)) {
                    if (line.empty() && row.body.empty()) continue;
                    transcriptView.lines.push_back("  " + line);
                }
                transcriptView.lines.push_back("");
            }
        }
        if (transcriptView.stick_bottom) transcriptView.scroll_to_end();

        inspectorView.lines.clear();
        inspectorView.lines.push_back("status   " + status);
        inspectorView.lines.push_back("bytes    " + std::to_string(tokenBytes));
        inspectorView.lines.push_back("actions  " + std::to_string(actionCount));
        inspectorView.lines.push_back("results  " + std::to_string(resultCount));
        inspectorView.lines.push_back("pending  " + std::to_string(pendingOps));
        inspectorView.lines.push_back("wakes    " + std::to_string(wakeCount));
        inspectorView.lines.push_back("");
        if (eventLog.empty()) {
            inspectorView.lines.push_back("No protocol events yet.");
        } else {
            for (int i = static_cast<int>(eventLog.size()) - 1; i >= 0 && inspectorView.lines.size() < 200; --i)
                inspectorView.lines.push_back(eventLog[static_cast<size_t>(i)]);
        }
        if (inspectorView.stick_bottom) inspectorView.scroll_to_end();
    }

    void apply(const UiEvent& e) {
        switch (e.kind) {
            case UiEventKind::Status:
                status = e.text;
                running = e.text.find("running") != std::string::npos;
                if (running) timelineState = PageState::Loading;
                pushRow({TimelineKind::Status, "status", e.text, true});
                break;
            case UiEventKind::Log:
                pushRow({TimelineKind::Log, "log", e.text, true});
                break;
            case UiEventKind::Error:
                failed = true;
                running = false;
                status = "error";
                timelineState = PageState::Error;
                pushRow({TimelineKind::Error, "error", e.text, false});
                break;
            case UiEventKind::Token:
                raw += e.text;
                tokenBytes += static_cast<int>(e.text.size());
                if (showRaw) {
                    for (auto& line : splitDisplayLines(e.text))
                        pushRow({TimelineKind::Stream, "raw", line, true});
                } else {
                    setStreamProgress(tokenBytes);
                }
                break;
            case UiEventKind::Protocol: {
                const auto& pe = e.protocol;
                if (pe.kind == ProtocolEventKind::THOUGHT) {
                    pushRow({TimelineKind::Thought, "thought", pe.text, true});
                    eventLog.push_back("thought");
                } else if (pe.kind == ProtocolEventKind::ACTION) {
                    ++actionCount;
                    ++pendingOps;
                    std::string title = pe.action.type + ":" + pe.action.name + " #" + pe.action.id;
                    pushRow({TimelineKind::Action, title, pe.action.body, true});
                    eventLog.push_back("action " + title);
                } else if (pe.kind == ProtocolEventKind::RESULT) {
                    ++resultCount;
                    if (pendingOps > 0) --pendingOps;
                    std::string title = "#" + pe.result.id + " " + pe.result.toolName;
                    std::string body = pe.result.summary;
                    if (pe.result.elapsedMs > 0) body += "\n" + std::to_string((int)pe.result.elapsedMs) + "ms";
                    pushRow({TimelineKind::Result, title, body, pe.result.ok});
                    eventLog.push_back(std::string("result ") + (pe.result.ok ? "ok " : "err ") + pe.result.id);
                } else if (pe.kind == ProtocolEventKind::RESPONSE) {
                    pushRow({TimelineKind::Response, "response", pe.text, true});
                    eventLog.push_back("response");
                }
                break;
            }
            case UiEventKind::TurnDone:
                done = true;
                running = false;
                status = failed ? "failed" : "done";
                finalText = e.text;
                timelineState = e.text.empty() ? PageState::Empty : PageState::Populated;
                pushRow({TimelineKind::Final, "final", e.text, !failed});
                break;
            case UiEventKind::AskDialog:
            case UiEventKind::AskDialogResult:
                pushRow({TimelineKind::Status, uiEventKindName(e.kind), e.text, true});
                break;
        }
    }

    void drain(AgentBridge& bridge) {
        auto batch = bridge.drain();
        if (!batch.empty()) ++wakeCount;
        for (const auto& e : batch) apply(e);
    }

    bool submitComposer() {
        if (running) return false;
        // trim
        std::string text = composer.value;
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ' || text.back() == '\t')) text.pop_back();
        size_t start = 0;
        while (start < text.size() && (text[start] == ' ' || text[start] == '\t' || text[start] == '\n')) ++start;
        text = text.substr(start);
        if (text.empty()) return false;
        pendingSubmit = text;
        pushRow({TimelineKind::User, "you", text, true});
        composer.value.clear();
        composer.cursor = 0;
        composer.scroll_row = 0;
        return true;
    }
};

}  // namespace cortex::mk3::ui
