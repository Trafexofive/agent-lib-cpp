#pragma once
// Domain model for the inkcell AgentShell. Drawing stays out of this file.
// Includes timeline block focus + nested sub-agent history drill-down.

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "inkcell/widgets/scroll_view.hpp"
#include "inkcell/widgets/textarea.hpp"
#include "src/core/agent.hpp"
#include "src/ui/chat/ask_dialog_model.hpp"
#include "src/ui/chat/transcript_cache.hpp"
#include "src/ui/model/dashboard_model.hpp"
#include "src/ui/bridge/agent_bridge.hpp"

namespace cortex::mk3::ui {

struct InkcellAppConfig {
    std::string agentName;
    std::string provider;
    std::string model;
    std::string manifestPath;
    std::string harnessPath;
    std::string systemPromptPath;
    std::string personaPath;
    std::string sessionId;
    int toolCount = 0;
    int feedCount = 0;
    int relicCount = 0;
    int subAgentCount = 0;
    bool ephemeral = false;
    // Chat render modifiers (orthogonal to -p / session flags).
    bool showThoughts = true;
    bool truncateBodies = true;
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
    // Drill-down metadata (action type=agent, or result from agent).
    std::string actionType;  // tool|agent|feed|relic|workflow
    std::string actionName;
    std::string actionId;
    bool drillable = false;
};

inline const char* kindGlyph(TimelineKind k, bool ok = true) {
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
            return ok ? "✓" : "✗";
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

inline TimelineRow rowFromProtocol(const ProtocolEvent& pe) {
    TimelineRow row;
    if (pe.kind == ProtocolEventKind::THOUGHT) {
        row.kind = TimelineKind::Thought;
        row.title = "thought";
        row.body = pe.text;
    } else if (pe.kind == ProtocolEventKind::ACTION) {
        row.kind = TimelineKind::Action;
        row.actionType = pe.action.type;
        row.actionName = pe.action.name;
        row.actionId = pe.action.id;
        row.drillable = (pe.action.type == "agent" && !pe.action.name.empty());
        row.title = pe.action.type + ":" + pe.action.name + " #" + pe.action.id;
        if (row.drillable) row.title += "  ↳ enter";
        row.body = pe.action.body;
    } else if (pe.kind == ProtocolEventKind::RESULT) {
        row.kind = TimelineKind::Result;
        row.ok = pe.result.ok;
        row.actionId = pe.result.id;
        row.actionName = pe.result.toolName;
        // Result drillability is resolved against the actual Agent tree later.
        row.drillable = false;
        row.title = "#" + pe.result.id + " " + pe.result.toolName;
        row.body = pe.result.summary;
        if (pe.result.elapsedMs > 0) row.body += "\n" + std::to_string(static_cast<int>(pe.result.elapsedMs)) + "ms";
    } else if (pe.kind == ProtocolEventKind::RESPONSE) {
        row.kind = TimelineKind::Response;
        row.title = "response";
        row.body = pe.text;
    }
    return row;
}

inline std::vector<TimelineRow> rowsFromAgent(Agent* agent) {
    std::vector<TimelineRow> out;
    if (!agent) {
        out.push_back({TimelineKind::Error, "missing agent", "No agent instance at this path.", false});
        return out;
    }
    const auto& events = agent->protocolEvents();
    if (events.empty()) {
        // Fall back to final response only.
        if (!agent->responseOutput().empty()) {
            out.push_back({TimelineKind::Final, "final", agent->responseOutput(), true});
        } else {
            out.push_back({TimelineKind::Log, "empty", "No protocol events recorded for this agent yet.", true});
        }
        return out;
    }
    for (const auto& pe : events) {
        TimelineRow row = rowFromProtocol(pe);
        if (row.kind == TimelineKind::Result && agent->hasSubAgent(row.actionName)) {
            row.drillable = true;
            row.actionType = "agent";
            if (row.title.find("↳") == std::string::npos) row.title += "  ↳ enter";
        } else if (row.kind == TimelineKind::Result && row.actionType != "agent") {
            // Only mark results drillable when they belong to a real sub-agent.
            if (!agent->hasSubAgent(row.actionName)) {
                row.drillable = false;
                auto pos = row.title.find("  ↳");
                if (pos != std::string::npos) row.title = row.title.substr(0, pos);
            }
        } else if (row.kind == TimelineKind::Action && row.actionType == "agent") {
            row.drillable = agent->hasSubAgent(row.actionName);
            if (!row.drillable) {
                auto pos = row.title.find("  ↳");
                if (pos != std::string::npos) row.title = row.title.substr(0, pos);
            }
        }
        out.push_back(std::move(row));
    }
    if (!agent->responseOutput().empty()) {
        // Avoid duplicate finals if last event already covered it.
        bool hasFinal = false;
        for (const auto& r : out)
            if (r.kind == TimelineKind::Final || r.kind == TimelineKind::Response) hasFinal = true;
        if (!hasFinal) out.push_back({TimelineKind::Final, "final", agent->responseOutput(), true});
    }
    return out;
}

struct ShellModel {
    // Live root transcript (bridge-fed).
    std::vector<TimelineRow> rootRows;
    std::vector<std::string> eventLog;
    std::string raw;
    std::string finalText;
    std::string status = "idle";
    PageState timelineState = PageState::Empty;
    bool running = false;
    int64_t turnStartMs = 0;  // steady_clock ms when the current turn started; 0 when idle. Drives live metrics.
    int64_t lastTurnElapsedMs = 0;  // duration of the most recently completed turn; persists after the turn ends for the "last" summary.
    bool done = false;
    bool failed = false;
    bool showThoughts = true;
    bool showRaw = false;
    // When true, long block bodies are capped (pi-like truncation) with a
    // "… N more lines" note. Toggle via /truncate or CLI --[no-]truncate.
    bool truncateBodies = true;
    static constexpr int kMaxBodyLines = 50;
    int tokenBytes = 0;
    int actionCount = 0;
    int resultCount = 0;
    int pendingOps = 0;
    int wakeCount = 0;
    int routeTicks = 0;
    std::string activePage = "Agent";
    std::string pendingSubmit;
    std::string pendingRoute;  // "agent" | "main" | "quit"
    std::string activeSessionId;
    // Agent display identity for the chat transcript labels. The assistant's own
    // turns (Response/Final) are labeled with agentName + agentModel/agentProvider
    // instead of the generic "CORTEX" sentinel, and subagent Action turns show
    // the subagent name + metadata. Set once by initializeChatModel from the
    // InkcellAppConfig (manifest-resolved).
    std::string agentName;
    std::string agentModel;
    std::string agentProvider;
    model::DashboardState dashboard;
    inkcell::widgets::TextAreaState composer;
    std::vector<std::string> promptHistory;
    int promptHistoryIndex = 0;
    std::string promptHistoryDraft;

    bool helpVisible = false;
    bool askActive = false;
    chat::DialogState askDialog;
    inkcell::widgets::TextAreaState askInput;
    std::set<int> askMultiSelected;
    mutable inkcell::widgets::ScrollViewState transcriptView;
    mutable inkcell::widgets::ScrollViewState inspectorView;

    // Focus + history navigation
    Agent* rootAgent = nullptr;
    std::vector<std::string> agentPath;  // nested sub-agent names from root
    int selectedBlock = 0;               // index into visible focusable blocks
    bool timelineFocus = false;          // false = composer owns keys
    // Maps visible block index -> rootRows/nested row index
    std::vector<int> blockRowIndex;
    // Nested frame cache (rebuilt on enter/refresh)
    std::vector<TimelineRow> nestedRows;

    // Current-turn protocol reducer. Agent protocol entries mutate in place as
    // response text and progress results grow; map each protocol index to one row.
    std::vector<int> activeProtocolRows;
    std::set<std::string> pendingActionIds;
    bool batchingEvents = false;
    bool viewRebuildPending = false;
    uint64_t viewRebuildCount = 0;
    uint64_t transcriptVersion = 0;
    mutable chat::TranscriptWrapCache transcriptWrapCache;
    std::set<std::string> completedResultIds;

    ShellModel() {
        composer.focused = true;
        composer.value.clear();
        rebuildViews();
    }

    void setRootAgent(Agent* agent) {
        rootAgent = agent;
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

    bool atRoot() const { return agentPath.empty(); }

    // Returns the body of the most recent Response timeline row (the
    // streaming/final LLM response text), or empty when there is none yet.
    // The dashboard preview line truncates this for display while a turn is
    // running. Scans backward so the latest response wins.
    std::string lastResponseBody() const {
        for (auto it = rootRows.rbegin(); it != rootRows.rend(); ++it) {
            if (it->kind == TimelineKind::Response) return it->body;
        }
        return {};
    }

    Agent* currentAgent() const {
        if (!rootAgent) return nullptr;
        Agent* cur = rootAgent;
        for (const auto& name : agentPath) {
            cur = cur->getSubAgent(name);
            if (!cur) return nullptr;
        }
        return cur;
    }

    const std::vector<TimelineRow>& activeRows() const {
        return atRoot() ? rootRows : nestedRows;
    }

    std::string breadcrumb() const {
        std::string path = nonempty(rootAgent ? rootAgent->name() : "", "root");
        for (const auto& name : agentPath) path += " / " + name;
        return path;
    }

    void pushRow(TimelineRow row) {
        if (!atRoot()) {
            // Live updates always land on root; nested is a focused historical view.
            rootRows.push_back(std::move(row));
            timelineState = PageState::Populated;
            return;
        }
        rootRows.push_back(std::move(row));
        timelineState = PageState::Populated;
        // Stick selection to bottom while streaming if already near end.
        if (running) selectedBlock = std::max(0, static_cast<int>(countFocusable(rootRows)) - 1);
        rebuildViews();
    }

    static int countFocusable(const std::vector<TimelineRow>& rows, bool showThoughtsFlag = true) {
        int n = 0;
        for (const auto& row : rows) {
            if (row.kind == TimelineKind::Thought && !showThoughtsFlag) continue;
            if (row.kind == TimelineKind::Stream) continue;  // stream is ephemeral status, not a block
            ++n;
        }
        return n;
    }

    void setStreamProgress(int bytes) {
        tokenBytes = bytes;
        if (!atRoot()) return;
        if (!rootRows.empty() && rootRows.back().kind == TimelineKind::Stream) {
            rootRows.back().title = "stream";
            rootRows.back().body = std::to_string(bytes) + " bytes received";
        } else {
            rootRows.push_back({TimelineKind::Stream, "stream", std::to_string(bytes) + " bytes received", true});
            timelineState = PageState::Populated;
        }
        rebuildViews();
    }

    void rebuildViews() {
        if (batchingEvents) {
            viewRebuildPending = true;
            return;
        }
        viewRebuildPending = false;
        ++viewRebuildCount;
        ++transcriptVersion;
        const auto& rows = activeRows();
        transcriptView.lines.clear();
        blockRowIndex.clear();

        if (rows.empty()) {
            if (atRoot()) timelineState = PageState::Empty;
        } else {
            // In a nested sub-agent scope the Response/Final label is the
            // child agent name (its chat), not the parent root agent.
            std::string scopeName = agentName;
            if (!atRoot()) {
                if (Agent* cur = currentAgent()) scopeName = cur->name();
                else if (!agentPath.empty()) scopeName = agentPath.back();
            }
            int focusIdx = 0;
            for (int ri = 0; ri < static_cast<int>(rows.size()); ++ri) {
                const auto& row = rows[static_cast<size_t>(ri)];
                if (row.kind == TimelineKind::Thought && !showThoughts) continue;
                // Empty/whitespace-only thoughts are parser noise or open-tag
                // placeholders — never paint a hollow ▎ THOUGHT block.
                if (row.kind == TimelineKind::Thought) {
                    bool any = false;
                    for (char c : row.body) {
                        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') { any = true; break; }
                    }
                    if (!any) continue;
                }
                if (row.kind == TimelineKind::Stream && !showRaw) continue;

                bool selected = timelineFocus && (focusIdx == selectedBlock);
                blockRowIndex.push_back(ri);

                std::string label;
                switch (row.kind) {
                    case TimelineKind::User:
                        label = "YOU";
                        break;
                    case TimelineKind::Thought:
                        label = "THOUGHT";
                        break;
                    case TimelineKind::Action: {
                        // Subagent turns: lead with the subagent NAME (actionName) and
                        // metadata (type, id, drillable), not a generic "AGENT" sentinel.
                        // For non-agent actions (tools/feeds/relics/workflows) the type is
                        // the kind label (TOOL/FEED/...) and actionName is the tool name.
                        std::string type = row.actionType.empty() ? "ACTION" : row.actionType;
                        std::transform(type.begin(), type.end(), type.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                        if (type == "AGENT" && !row.actionName.empty()) {
                            // Subagent: "AGENT  <name>  #<id>  ↳" — name is the headline.
                            label = "AGENT  " + row.actionName;
                            if (!row.actionId.empty()) label += "  #" + row.actionId;
                            if (row.drillable) label += "  ↳";
                        } else {
                            label = type;
                            if (!row.actionName.empty()) label += "  " + row.actionName;
                            if (!row.actionId.empty()) label += "  #" + row.actionId;
                            if (row.drillable) label += "  ↳";
                        }
                        break;
                    }
                    case TimelineKind::Result:
                        label = row.ok ? "✓ RESULT" : "✗ RESULT";
                        if (!row.actionName.empty()) label += "  " + row.actionName;
                        if (!row.actionId.empty()) label += "  #" + row.actionId;
                        if (row.drillable) label += "  ↳";
                        break;
                    // The assistant's own turns: label with the real agent name +
                    // model/provider metadata instead of the generic "CORTEX" sentinel.
                    // Falls back to "CORTEX" only if the agent identity was never wired
                    // (e.g. standalone unit tests that don't call initializeChatModel).
                    case TimelineKind::Response:
                    case TimelineKind::Final:
                        // Nested scope uses the child agent name (drop-into-its-chat).
                        // Root keeps parent identity + model/provider metadata.
                        label = scopeName.empty() ? "CORTEX" : scopeName;
                        if (atRoot() && (!agentModel.empty() || !agentProvider.empty())) {
                            label += "  ";
                            std::string meta;
                            if (!agentProvider.empty()) meta = agentProvider;
                            if (!agentModel.empty()) {
                                if (!meta.empty()) meta += "/";
                                meta += agentModel;
                            }
                            label += meta;
                        }
                        break;
                    case TimelineKind::Error:
                        label = "✗ ERROR";
                        break;
                    case TimelineKind::Status:
                        label = "STATUS";
                        break;
                    case TimelineKind::Stream:
                        label = "RAW";
                        break;
                    case TimelineKind::Log:
                        label = row.title.empty() ? "NOTICE" : row.title;
                        break;
                }
                transcriptView.lines.push_back(std::string(selected ? "› " : "  ") + label);
                // Always emit the row body. For sub-agent Results this is the
                // child's final response text (summary/output) — not nested
                // child blocks. Full child timeline is manual ↳ Enter only.
                // Optional truncation (pi-like): cap body lines when enabled.
                {
                    auto bodyLines = splitDisplayLines(row.body);
                    int shown = 0;
                    int total = 0;
                    for (const auto& line : bodyLines) {
                        if (line.empty() && row.body.empty()) continue;
                        ++total;
                    }
                    for (const auto& line : bodyLines) {
                        if (line.empty() && row.body.empty()) continue;
                        if (truncateBodies && shown >= kMaxBodyLines) break;
                        transcriptView.lines.push_back("    " + line);
                        ++shown;
                    }
                    if (truncateBodies && total > shown) {
                        transcriptView.lines.push_back(
                            "    … (" + std::to_string(total - shown) +
                            " more lines — /truncate off or ↳ drill to expand)");
                    }
                }
                transcriptView.lines.push_back("");
                ++focusIdx;
            }
            if (atRoot() && !rows.empty()) timelineState = PageState::Populated;
        }

        // Clamp selection.
        int focusable = static_cast<int>(blockRowIndex.size());
        if (focusable <= 0) selectedBlock = 0;
        else selectedBlock = std::max(0, std::min(selectedBlock, focusable - 1));

        // Keep selected block in view when timeline-focused; stick-bottom while running at root.
        if (running && atRoot() && !timelineFocus) {
            transcriptView.stick_bottom = true;
            transcriptView.scroll_to_end();
        } else if (timelineFocus && focusable > 0) {
            transcriptView.stick_bottom = false;
            ensureSelectionVisible();
        } else if (transcriptView.stick_bottom) {
            transcriptView.scroll_to_end();
        }

        inspectorView.lines.clear();
        inspectorView.lines.push_back("path     " + breadcrumb());
        inspectorView.lines.push_back("status   " + status);
        inspectorView.lines.push_back("bytes    " + std::to_string(tokenBytes));
        inspectorView.lines.push_back("actions  " + std::to_string(actionCount));
        inspectorView.lines.push_back("results  " + std::to_string(resultCount));
        inspectorView.lines.push_back("pending  " + std::to_string(pendingOps));
        inspectorView.lines.push_back("wakes    " + std::to_string(wakeCount));
        inspectorView.lines.push_back("focus    " + std::string(timelineFocus ? "timeline" : "composer"));
        inspectorView.lines.push_back("");
        if (eventLog.empty()) {
            inspectorView.lines.push_back("No protocol events yet.");
        } else {
            for (int i = static_cast<int>(eventLog.size()) - 1; i >= 0 && inspectorView.lines.size() < 200; --i)
                inspectorView.lines.push_back(eventLog[static_cast<size_t>(i)]);
        }
        if (inspectorView.stick_bottom) inspectorView.scroll_to_end();
    }

    void ensureSelectionVisible() {
        // Approximate: each block ~3 lines; scroll so selected is mid-view.
        int line = 0;
        int focusIdx = 0;
        const auto& rows = activeRows();
        for (int ri = 0; ri < static_cast<int>(rows.size()); ++ri) {
            const auto& row = rows[static_cast<size_t>(ri)];
            if (row.kind == TimelineKind::Thought && !showThoughts) continue;
            if (row.kind == TimelineKind::Stream && !showRaw) continue;
            if (focusIdx == selectedBlock) {
                transcriptView.offset = std::max(0, line - 1);
                transcriptView.clamp();
                return;
            }
            line += 1 + static_cast<int>(splitDisplayLines(row.body).size()) + 1;
            ++focusIdx;
        }
    }

    void selectDelta(int delta) {
        int n = static_cast<int>(blockRowIndex.size());
        if (n <= 0) return;
        selectedBlock = std::max(0, std::min(n - 1, selectedBlock + delta));
        rebuildViews();
    }

    const TimelineRow* selectedRow() const {
        if (selectedBlock < 0 || selectedBlock >= static_cast<int>(blockRowIndex.size())) return nullptr;
        int ri = blockRowIndex[static_cast<size_t>(selectedBlock)];
        const auto& rows = activeRows();
        if (ri < 0 || ri >= static_cast<int>(rows.size())) return nullptr;
        return &rows[static_cast<size_t>(ri)];
    }

    // Manual drill only (↳ Enter). Never auto-enter on AGENT action —
    // the operator chooses when to open a sub-agent's full chat.
    bool enterSubAgent(const std::string& name) {
        if (name.empty()) return false;
        Agent* parent = currentAgent();
        if (!parent) parent = rootAgent;
        if (!atRoot() && rootAgent && rootAgent->hasSubAgent(name) &&
            (agentPath.size() == 1 || !parent->hasSubAgent(name))) {
            agentPath.clear();
            parent = rootAgent;
        }
        if (!parent) parent = rootAgent;
        if (!parent || !parent->hasSubAgent(name)) {
            status = "no sub-agent: " + name;
            rebuildViews();
            return false;
        }
        if (!agentPath.empty() && agentPath.back() == name) {
            refreshNested();
            return true;
        }
        agentPath.push_back(name);
        nestedRows = rowsFromAgent(parent->getSubAgent(name));
        selectedBlock = 0;
        timelineFocus = true;
        composer.focused = false;
        rebuildViews();
        return true;
    }

    bool enterSelected() {
        const TimelineRow* row = selectedRow();
        if (!row || !row->drillable) return false;
        return enterSubAgent(row->actionName);
    }

    bool goBack() {
        if (agentPath.empty()) return false;
        agentPath.pop_back();
        if (agentPath.empty()) {
            nestedRows.clear();
        } else {
            Agent* parent = rootAgent;
            for (size_t i = 0; i + 1 < agentPath.size(); ++i) {
                if (!parent) break;
                parent = parent->getSubAgent(agentPath[i]);
            }
            nestedRows = rowsFromAgent(parent ? parent->getSubAgent(agentPath.back()) : nullptr);
        }
        selectedBlock = 0;
        timelineFocus = true;
        composer.focused = false;
        rebuildViews();
        return true;
    }

    void refreshNested() {
        if (atRoot()) return;
        nestedRows = rowsFromAgent(currentAgent());
        rebuildViews();
    }

    void focusTimeline() {
        timelineFocus = true;
        composer.focused = false;
        if (selectedBlock < 0) selectedBlock = 0;
        rebuildViews();
    }

    void focusComposer() {
        if (!atRoot()) return;  // nested views are browse-only
        timelineFocus = false;
        composer.focused = true;
        rebuildViews();
    }

    static bool isProgressPlaceholder(const ProtocolResult& result) {
        return result.elapsedMs == 0.0 && result.summary.find(" is running…") != std::string::npos;
    }

    void upsertProtocolRow(size_t index, TimelineRow row) {
        if (activeProtocolRows.size() <= index) activeProtocolRows.resize(index + 1, -1);
        int& mapped = activeProtocolRows[index];
        if (mapped >= 0 && mapped < static_cast<int>(rootRows.size())) {
            rootRows[static_cast<size_t>(mapped)] = std::move(row);
        } else {
            mapped = static_cast<int>(rootRows.size());
            rootRows.push_back(std::move(row));
        }
        timelineState = PageState::Populated;
    }

    void apply(const UiEvent& e) {
        switch (e.kind) {
            case UiEventKind::Status: {
                bool startsTurn = e.text.find("running") != std::string::npos && !running;
                if (startsTurn) {
                    activeProtocolRows.clear();
                    pendingActionIds.clear();
                    completedResultIds.clear();
                    pendingOps = 0;
                    actionCount = 0;
                    resultCount = 0;
                    tokenBytes = 0;
                    raw.clear();
                    done = false;
                    failed = false;
                }
                status = e.text;
                bool wasRunning = running;
                running = e.text.find("running") != std::string::npos;
                if (running && !wasRunning) turnStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                if (running) timelineState = PageState::Loading;
                break;
            }
            case UiEventKind::Log:
                pushRow({TimelineKind::Log, "log", e.text, true});
                break;
            case UiEventKind::Error:
                failed = true;
                running = false;
                if (turnStartMs > 0) {
                    lastTurnElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() - turnStartMs;
                    turnStartMs = 0;
                }
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
                }
                // If the operator manually drilled into a sub-agent, keep its
                // timeline live while child tokens stream.
                if (!atRoot()) refreshNested();
                break;
            case UiEventKind::Protocol: {
                const auto& pe = e.protocol;
                TimelineRow row = rowFromProtocol(pe);
                if (pe.kind == ProtocolEventKind::ACTION) {
                    if (pendingActionIds.insert(pe.action.id).second) {
                        ++actionCount;
                        pendingOps = static_cast<int>(pendingActionIds.size());
                        eventLog.push_back("action " + row.title);
                    }
                    if (row.actionType == "agent" && rootAgent && rootAgent->hasSubAgent(row.actionName)) {
                        row.drillable = true;
                        if (row.title.find("↳") == std::string::npos) row.title += "  ↳ enter";
                    } else if (row.actionType != "agent") {
                        row.drillable = false;
                    }
                    upsertProtocolRow(e.protocolIndex, std::move(row));
                } else if (pe.kind == ProtocolEventKind::RESULT) {
                    if (isProgressPlaceholder(pe.result)) {
                        // Keep progress in the status metrics. Do not render a fake
                        // completed result such as "reader is running…".
                        break;
                    }
                    if (completedResultIds.insert(pe.result.id).second) {
                        ++resultCount;
                        pendingActionIds.erase(pe.result.id);
                        pendingOps = static_cast<int>(pendingActionIds.size());
                        eventLog.push_back(std::string("result ") + (row.ok ? "ok " : "err ") + row.actionId);
                    }
                    if (rootAgent && rootAgent->hasSubAgent(row.actionName)) {
                        row.drillable = true;
                        row.actionType = "agent";
                        if (row.title.find("↳") == std::string::npos) row.title += "  ↳ enter";
                        // Prefer the child's final response text when the
                        // protocol summary is empty/placeholder (e.g. bare name).
                        // rowFromProtocol may append "\nNms" — strip that before
                        // comparing against the placeholder summary.
                        if (Agent* sub = rootAgent->getSubAgent(row.actionName)) {
                            const std::string& finalOut = sub->responseOutput();
                            std::string summaryOnly = pe.result.summary;
                            bool placeholder = summaryOnly.empty() ||
                                               summaryOnly == row.actionName ||
                                               summaryOnly == pe.result.toolName;
                            if (!finalOut.empty() && (row.body.empty() || placeholder)) {
                                row.body = finalOut;
                                if (pe.result.elapsedMs > 0)
                                    row.body += "\n" +
                                        std::to_string(static_cast<int>(pe.result.elapsedMs)) +
                                        "ms";
                            }
                        }
                    } else {
                        row.drillable = false;
                    }
                    upsertProtocolRow(e.protocolIndex, std::move(row));
                } else {
                    if (pe.kind == ProtocolEventKind::THOUGHT) eventLog.push_back("thought");
                    else if (pe.kind == ProtocolEventKind::RESPONSE) eventLog.push_back("response");
                    upsertProtocolRow(e.protocolIndex, std::move(row));
                }
                if (atRoot()) rebuildViews();
                else refreshNested();
                break;
            }
            case UiEventKind::TurnDone: {
                done = true;
                running = false;
                if (turnStartMs > 0) {
                    lastTurnElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() - turnStartMs;
                    turnStartMs = 0;
                }
                bool cancelled = e.text == "[cancelled]";
                status = cancelled ? "cancelled" : failed ? "failed" : "done";
                finalText = e.text;
                timelineState = e.text.empty() ? PageState::Empty : PageState::Populated;
                bool hasResponse = false;
                for (const auto& row : rootRows)
                    if (row.kind == TimelineKind::Response) hasResponse = true;
                if (!hasResponse && !e.text.empty()) pushRow({TimelineKind::Final, "final", e.text, !failed});
                pendingActionIds.clear();
                pendingOps = 0;
                // Re-mark drillable agent rows now that children finished.
                if (rootAgent) {
                    for (auto& row : rootRows) {
                        if ((row.kind == TimelineKind::Action && row.actionType == "agent") ||
                            row.kind == TimelineKind::Result) {
                            if (rootAgent->hasSubAgent(row.actionName)) {
                                row.drillable = true;
                                if (row.title.find("↳") == std::string::npos) row.title += "  ↳ enter";
                            }
                        }
                    }
                }
                if (atRoot()) rebuildViews();
                else refreshNested();
                break;
            }
            case UiEventKind::AskDialog:
                askDialog = chat::parseDialogState(e.json);
                chat::completeNonInteractiveCards(askDialog);
                askActive = !askDialog.done();
                askInput.value.clear();
                askInput.cursor = 0;
                askInput.focused = true;
                askMultiSelected.clear();
                status = askActive ? "waiting human input" : status;
                break;
            case UiEventKind::AskDialogResult:
                askActive = false;
                askInput.value.clear();
                askMultiSelected.clear();
                break;
        }
    }

    void drain(AgentBridge& bridge) {
        auto batch = bridge.drain();
        if (batch.empty()) return;
        ++wakeCount;
        batchingEvents = true;
        for (const auto& e : batch) apply(e);
        batchingEvents = false;
        if (viewRebuildPending) rebuildViews();
    }

    void loadSessionRecords(const std::vector<SessionRecord>& records) {
        rootRows.clear();
        for (const auto& record : records) {
            TimelineRow row;
            row.body = record.content;
            row.ok = true;
            switch (record.role) {
                case SessionRecord::USER:
                    row.kind = TimelineKind::User;
                    row.title = "you";
                    break;
                case SessionRecord::AGENT:
                    row.kind = TimelineKind::Response;
                    row.title = "response";
                    break;
                case SessionRecord::TOOL_CALL:
                    row.kind = TimelineKind::Action;
                    row.title = "tool call";
                    break;
                case SessionRecord::TOOL_RESULT:
                    row.kind = TimelineKind::Result;
                    row.title = "tool result";
                    break;
                case SessionRecord::SYSTEM:
                    row.kind = TimelineKind::Log;
                    row.title = "system";
                    break;
            }
            rootRows.push_back(std::move(row));
        }
        timelineState = rootRows.empty() ? PageState::Empty : PageState::Populated;
        selectedBlock = 0;
        rebuildViews();
    }

    void clearTranscript() {
        rootRows.clear();
        nestedRows.clear();
        activeProtocolRows.clear();
        pendingActionIds.clear();
        completedResultIds.clear();
        pendingOps = 0;
        selectedBlock = 0;
        rebuildViews();
    }

    void appendNotice(const std::string& title, const std::vector<std::string>& lines) {
        TimelineRow row;
        row.kind = TimelineKind::Log;
        row.title = title;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) row.body += "\n";
            row.body += lines[i];
        }
        pushRow(std::move(row));
    }

    bool historyPrevious() {
        if (promptHistory.empty() || running || !atRoot()) return false;
        if (promptHistoryIndex >= static_cast<int>(promptHistory.size())) {
            promptHistoryDraft = composer.value;
            promptHistoryIndex = static_cast<int>(promptHistory.size());
        }
        if (promptHistoryIndex <= 0) return false;
        --promptHistoryIndex;
        composer.value = promptHistory[static_cast<size_t>(promptHistoryIndex)];
        composer.cursor = static_cast<int>(composer.value.size());
        return true;
    }

    bool historyNext() {
        if (promptHistory.empty() || running || !atRoot()) return false;
        if (promptHistoryIndex >= static_cast<int>(promptHistory.size())) return false;
        ++promptHistoryIndex;
        composer.value = promptHistoryIndex == static_cast<int>(promptHistory.size())
                             ? promptHistoryDraft
                             : promptHistory[static_cast<size_t>(promptHistoryIndex)];
        composer.cursor = static_cast<int>(composer.value.size());
        return true;
    }

    bool submitComposer() {
        if (running || !atRoot()) return false;
        std::string text = composer.value;
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ' || text.back() == '\t')) text.pop_back();
        size_t start = 0;
        while (start < text.size() && (text[start] == ' ' || text[start] == '\t' || text[start] == '\n')) ++start;
        text = text.substr(start);
        if (text.empty()) return false;
        pendingSubmit = text;
        if (promptHistory.empty() || promptHistory.back() != text) promptHistory.push_back(text);
        promptHistoryIndex = static_cast<int>(promptHistory.size());
        promptHistoryDraft.clear();
        pushRow({TimelineKind::User, "you", text, true});
        composer.value.clear();
        composer.cursor = 0;
        composer.scroll_row = 0;
        rebuildViews();
        return true;
    }
};

}  // namespace cortex::mk3::ui
