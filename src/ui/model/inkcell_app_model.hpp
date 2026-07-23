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
#include "src/ui/chat/notification.hpp"
#include "src/ui/chat/ask_dialog_model.hpp"
#include "src/ui/chat/transcript_cache.hpp"
#include "src/ui/components/cmd_palette.hpp"
#include "src/ui/model/dashboard_model.hpp"
#include "src/ui/model/workflow_run_model.hpp"
#include "src/ui/bridge/agent_bridge.hpp"

namespace cortex::mk3::ui {

struct InkcellAppConfig {
    std::string agentName;
    std::string provider;
    std::string model;
    std::string manifestPath;   // active agent.yml (if any)
    std::string manifestDir;    // discovery root override (--manifest-dir or derived)
    std::string harnessPath;
    std::string systemPromptPath;
    std::string personaPath;
    std::string sessionId;
    int toolCount = 0;
    int feedCount = 0;
    int relicCount = 0;
    int subAgentCount = 0;
    // Lifecycle flags (orthogonal):
    //   noSession  → don't load/save session records
    //   ephemeral  → exit the TUI when the agent turn finishes
    bool noSession = false;
    bool ephemeral = false;
    // Optional seed prompt for interactive REPL (-p without --ephemeral).
    std::string initialPrompt;
    // Chat render modifiers (orthogonal to -p / session / lifecycle).
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

// Vet-fix: terminal control bytes (0x00..0x08, 0x0B, 0x0C, 0x0E..0x1F, 0x7F)
// leak into the chat body when raw streaming surfaces mixed wire bytes or
// argv-style binary tokens. Replace each with a printable placeholder so
// the chat transcript never prints Q sym, box-drawing garbage, or worse,
// injects ANSI escapes mid-render.
inline std::string sanitizeForDisplay(const std::string& text) {
    // Vet-fix pass 2: control bytes get replaced with a space, AND the
    // result gets a hard size cap. raw responses that include SSE
    // streams ("data: {…}"), stack traces of httplib/jsoncpp/stl,
    // and assorted server chatter are otherwise rendered verbatim —
    // the previous build only stripped <0x20 bytes. Cap at 16KiB and
    // keep first+last so context is preserved without the operator
    // having to scroll through thousands of rows of demangled binary.
    constexpr std::size_t kCap = 16 * 1024;
    constexpr std::size_t kHead = 8 * 1024;
    constexpr std::size_t kTail = 4 * 1024;
    std::string out;
    out.reserve(std::min<std::size_t>(text.size(), kCap + 64));
    for (unsigned char c : text) {
        if (c == '\n' || c == '\r' || c == '\t') {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x20 || c == 0x7F) {
            out.push_back(' ');
        } else {
            out.push_back(static_cast<char>(c));
        }
        if (out.size() >= kHead + kTail + 256) break; // start trimming earlier
    }
    if (out.size() <= kCap) return out;
    // Hard cap: keep head + tail with a marker.
    const std::size_t dropped = out.size() - kHead - kTail;
    std::string trimmed;
    trimmed.reserve(kCap + 96);
    trimmed.append(out, 0, kHead);
    trimmed.append("\n  … [sanitize: dropped ");
    trimmed.append(std::to_string(dropped));
    trimmed.append(" bytes] …\n");
    if (out.size() > kHead) {
        trimmed.append(out, out.size() - kTail, kTail);
    }
    return trimmed;
}

// Truncate to a width with a unicode-aware ellipsis when needed.
// Display width is a best-effort byte heuristic here; UTF-8 multi-byte
// characters are treated as a single cell so we never split mid-codepoint.
inline std::string safeTruncate(const std::string& text, int maxCells) {
    if (maxCells <= 0) return {};
    int cells = 0;
    size_t i = 0;
    while (i < text.size() && cells < maxCells) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        size_t step = 1;
        if ((c & 0x80) == 0x80) {
            // crude UTF-8 lead-byte advance: 2, 3, 4-byte
            if ((c & 0xE0) == 0xC0) step = 2;
            else if ((c & 0xF0) == 0xE0) step = 3;
            else if ((c & 0xF8) == 0xF0) step = 4;
        }
        if (i + step > text.size()) step = text.size() - i;
        i += step;
        ++cells;
    }
    if (i >= text.size()) return text;
    if (maxCells - cells >= 1) return text + "…";
    return text.substr(0, i) + "…";
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
    } else if (pe.kind == ProtocolEventKind::STATUS) {
        // Runtime notices: max_iterations, finalization, cancel, promote, etc.
        row.kind = TimelineKind::Status;
        row.title = pe.text.rfind("[LIMIT]", 0) == 0 ? "limit"
                    : pe.text.rfind("[FINALIZE]", 0) == 0 ? "finalize"
                    : "status";
        row.body = pe.text;
        row.ok = pe.text.find("⚠") == std::string::npos &&
                 pe.text.find("error") == std::string::npos;
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
    } else if (pe.kind == ProtocolEventKind::RETRY) {
        // Vet-fix: RETRY = protocol-only signal, NOT visible timeline content.
        // Tagged as Log with ok=false so the live apply() can recognize and
        // reset baseline (see apply() handler below). Non-live dump paths
        // (loadSessionRecords / rowsFromAgent) skip Log kind in display.
        row.kind = TimelineKind::Log;
        row.title = "RETRY";
        row.body = pe.text;
        row.ok = false;
    }
    return row;
}

inline std::vector<TimelineRow> rowsFromAgent(Agent* agent) {
    std::vector<TimelineRow> out;
    if (!agent) {
        out.push_back({TimelineKind::Error, "missing agent", "No agent instance at this path.", false});
        return out;
    }

    // Prefer structured protocolEvents_ (same Thought/Action/Result/Response
    // path as the parent chat — full palette). Events now accumulate across
    // non-ephemeral prompt() calls. Fall back to history_ only when events
    // are empty (e.g. session restore without event replay).
    const auto& events = agent->protocolEvents();
    if (!events.empty()) {
        for (const auto& pe : events) {
            TimelineRow row = rowFromProtocol(pe);
            if (row.kind == TimelineKind::Result && agent->hasSubAgent(row.actionName)) {
                row.drillable = true;
                row.actionType = "agent";
                if (row.title.find("↳") == std::string::npos) row.title += "  ↳ enter";
            } else if (row.kind == TimelineKind::Action && row.actionType == "agent") {
                row.drillable = agent->hasSubAgent(row.actionName);
            }
            out.push_back(std::move(row));
        }
        // Parent missions that only exist in history_ (User/Parent lines) are
        // not protocol events — prepend missing initiator turns from history
        // so the nested chat still shows who spoke, then the structured body.
        std::vector<TimelineRow> inits;
        for (const auto& h : agent->history()) {
            TimelineRow row;
            row.ok = true;
            if (h.rfind("User: ", 0) == 0) {
                row.kind = TimelineKind::User;
                row.title = "user";
                row.body = h.substr(6);
            } else if (h.rfind("Parent(", 0) == 0) {
                row.kind = TimelineKind::User;
                auto close = h.find(')');
                std::string from = "parent";
                std::string body = h;
                if (close != std::string::npos) {
                    from = h.substr(7, close - 7);
                    body = (close + 1 < h.size() && h[close + 1] == ':') ? h.substr(close + 2)
                                                                         : h.substr(close + 1);
                    if (!body.empty() && body[0] == ' ') body = body.substr(1);
                }
                row.title = "parent:" + from;
                row.body = body;
            } else {
                continue;
            }
            if (!row.body.empty()) inits.push_back(std::move(row));
        }
        if (!inits.empty()) {
            // Interleave is hard without timestamps; show initiators first then
            // the full structured event stream (matches multi-prompt continuity).
            inits.insert(inits.end(), out.begin(), out.end());
            out.swap(inits);
        }
        return out;
    }

    // History-only fallback (no structured events).
    for (const auto& h : agent->history()) {
        TimelineRow row;
        row.ok = true;
        if (h.rfind("User: ", 0) == 0) {
            row.kind = TimelineKind::User;
            row.title = "user";
            row.body = h.substr(6);
        } else if (h.rfind("Parent(", 0) == 0) {
            row.kind = TimelineKind::User;
            auto close = h.find(')');
            std::string from = "parent";
            std::string body = h;
            if (close != std::string::npos) {
                from = h.substr(7, close - 7);
                body = (close + 1 < h.size() && h[close + 1] == ':') ? h.substr(close + 2)
                                                                     : h.substr(close + 1);
                if (!body.empty() && body[0] == ' ') body = body.substr(1);
            }
            row.title = "parent:" + from;
            row.body = body;
        } else if (h.rfind("Agent: ", 0) == 0) {
            row.kind = TimelineKind::Response;
            row.title = "response";
            row.body = h.substr(7);
        } else if (h.rfind("System: ", 0) == 0) {
            row.kind = TimelineKind::Log;
            row.title = "system";
            row.body = h.substr(8);
        } else {
            row.kind = TimelineKind::Log;
            row.title = "note";
            row.body = h;
        }
        if (!row.body.empty()) out.push_back(std::move(row));
    }

    if (out.empty()) {
        if (!agent->responseOutput().empty()) {
            out.push_back({TimelineKind::Final, "final", agent->responseOutput(), true});
        } else {
            out.push_back({TimelineKind::Log, "empty", "No history recorded for this agent yet.", true});
        }
    }
    return out;
}

struct ShellModel {
    chat::NotificationStack notificationStack;
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
    int64_t lastNestedRefreshMs = 0;  // vet-fix: rate-limit refreshNested() so live sub-agent nesting doesn't rebuild O(protocolEvents) per token.
    bool done = false;
    bool failed = false;
    bool showThoughts = true;
    bool showRaw = false;
    // Vet-fix: chat-body field underlay opt-in. Off by default — the chat
    // surface stays crisp unless the operator opts in. Hub field on/off is
    // shared via gfx:: and this is the chat-side gate.
    bool chatFieldEnabled = false;
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
    // Hub Enter on a launchable agent sets this path; REPL tick hot-swaps the
    // live Agent then routes to the chat scene. Cleared after attempt.
    std::string pendingLaunchManifest;
    std::string launchError;  // last hot-swap failure (surfaced on Home/app bar)
    // Hub Enter on kind=workflow — REPL tick runs WorkflowEngine on a worker.
    std::string pendingRunWorkflow;
    bool pendingStopWorkflow = false;
    // Shared live run hub (worker writes, UI snapshots).
    model::WorkflowRunHub workflowRun;
    std::string activeSessionId;
    // Agent display identity for the chat transcript labels. The assistant's own
    // turns (Response/Final) are labeled with agentName + agentModel/agentProvider
    // instead of the generic "CORTEX" sentinel, and subagent Action turns show
    // the subagent name + metadata. Set once by initializeChatModel from the
    // InkcellAppConfig (manifest-resolved).
    std::string agentName;
    std::string agentModel;
    std::string agentProvider;
    std::string activeManifestPath;  // live agent.yml — updated on hub launch
    model::DashboardState dashboard;
    inkcell::widgets::TextAreaState composer;
    std::vector<std::string> promptHistory;
    int promptHistoryIndex = 0;
    std::string promptHistoryDraft;

    bool helpVisible = false;
    components::CmdPalette cmdPalette;
    std::string pendingPaletteAction;  // set on palette Enter; scenes execute
    bool askActive = false;
    chat::DialogState askDialog;
    inkcell::widgets::TextAreaState askInput;
    std::set<int> askMultiSelected;

    // Readline-style slash completion (LCP then cycle).
    std::vector<std::string> tabMatches;
    int tabMatchIndex = -1;
    std::string tabStem;  // prefix used to build tabMatches

    void clearTabCompletion() {
        tabMatches.clear();
        tabMatchIndex = -1;
        tabStem.clear();
    }
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
        const_cast<ShellModel*>(this)->applyRowBans(std::move(row));
    }

    // Vet-fix real perf: when transcript grows unbounded (raw mode + many
    // small deltas), the wrap path eventually dominates the tick. We cap the
    // transcript at kRootRowCap, dropping oldest Stream/Thought rows first.
    // User/Response/Action/Result pairs are protected — never lose shipped
    // answers or tool boundaries.
    static constexpr int kRootRowCap = 1500;
    void enforceRowCap() {
        if (static_cast<int>(rootRows.size()) < kRootRowCap) return;
        int excess = static_cast<int>(rootRows.size()) - kRootRowCap;
        size_t drop = 0;
        // Drop oldest first, but never delete the leading header or any
        // Action/Result pair anchor.
        while (drop < excess) {
            const auto& r = rootRows[drop];
            if (r.kind == TimelineKind::Thought || r.kind == TimelineKind::Stream) {
                ++drop;
            } else {
                // We hit a protected row. Look forward — spool through all
                // contiguous Action/Result/Response/User rows at the head
                // only if the orphan would dangle: otherwise just stop.
                break;
            }
        }
        if (drop == 0) return; // can't honor cap without losing protected content
        rootRows.erase(rootRows.begin(), rootRows.begin() + drop);
        selectedBlock = std::max(0, selectedBlock - static_cast<int>(drop));
        // Active protocol indices must shift correspondingly.
        for (auto& idx : activeProtocolRows) {
            if (idx >= 0) idx -= static_cast<int>(drop);
        }
        timelineState = PageState::Populated;
        rebuildViews();
    }
    void applyRowBans(TimelineRow row) {
        if (!row.body.empty() && row.kind != TimelineKind::User &&
            row.kind != TimelineKind::Response && row.kind != TimelineKind::Thought &&
            row.kind != TimelineKind::Action && row.kind != TimelineKind::Result) {
            row.body = sanitizeForDisplay(row.body);
        }
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
                        // Nested sub-agent chat: parent-agent missions are not the
                        // human operator — label them PARENT <name>.
                        if (row.title.rfind("parent:", 0) == 0)
                            label = "PARENT  " + row.title.substr(7);
                        else
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
                        if (row.title == "limit") label = "⚠ LIMIT";
                        else if (row.title == "finalize") label = "▣ FINALIZE";
                        else label = "STATUS";
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
        // Vet-fix: refreshNested runs per child token and rebuilds an entire
        // timeline view from the agent's protocol event log — at 50+ tokens/sec
        // noticeable stalls come from this path, NOT from the apply() route.
        // Cache: budget gated by elapsed wall time; correctness gated by
        // child protocol events growth (size + last child sees a *new*
        // finely-delivered token).
        Agent* cur = currentAgent();
        if (cur) {
            const int64_t now =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            if (now - lastNestedRefreshMs < 100) return;
            lastNestedRefreshMs = now;
        }
        nestedRows = rowsFromAgent(cur);
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
                // A fresh "running" status always opens a new protocol-mapping
                // epoch. The agent clears protocolEvents_ at runLoop start so
                // indices restart at 0; if we keep the previous turn's
                // activeProtocolRows, upsertProtocolRow OVERWRITES the top of
                // the transcript (second-query clobber bug).
                //
                // Do NOT gate this on !running: the REPL may set running=true
                // before the worker publishes status, which would skip the
                // reset and corrupt contiguity.
                bool isRunningStatus = e.text.find("running") != std::string::npos;
                if (isRunningStatus) {
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
                    if (!running) {
                        turnStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count();
                    }
                    timelineState = PageState::Loading;
                }
                status = e.text;
                running = isRunningStatus;
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
            case UiEventKind::Notification: {
                // Vet-fix: pipe all retry / hiccup signals through the
                // Notification stack so the chat TUI can render a uniform
                // banner instead of leaking text into the transcript or stderr.
                chat::Notification n;
                n.id = e.id;
                n.source = e.source;
                n.severity = e.severity.empty() ? "info" : e.severity;
                n.title = e.text;
                n.attempt = e.attempt;
                n.maxAttempts = e.maxAttempts;
                n.lifetimeMs = 0; // sticky until dismissed
                notificationStack.push(std::move(n));
                break;
            }
            case UiEventKind::Token:
                raw += e.text;
                tokenBytes += static_cast<int>(e.text.size());
                if (showRaw) {
                    // Vet-fix: model bytes can carry non-printable noise
                    // when the wire stream straddles tool transitions or
                    // carries embedded escapes. Strip the noise so the chat
                    // body never produces gibberish likesymbol rows in the
                    // transcript.
                    std::string sanitized = sanitizeForDisplay(e.text);
                    for (auto& line : splitDisplayLines(sanitized))
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

    // If ask_tool opened a dialog that is already finished (all notes/info, or
    // scene finished cards without the scene holding the bridge), unblock the
    // worker waiting on requestAsk(). Safe no-op when no ask is pending.
    void settleAsk(AgentBridge& bridge) {
        if (!bridge.askPending()) return;
        if (askDialog.cancelled) {
            askActive = false;
            bridge.cancelAsk();
            return;
        }
        if (askDialog.done()) {
            askActive = false;
            bridge.completeAsk(askDialog.results);
            if (!running && status == "waiting human input") status = "ready";
        }
    }

    void drain(AgentBridge& bridge) {
        auto batch = bridge.drain();
        if (!batch.empty()) {
            ++wakeCount;
            batchingEvents = true;
            for (const auto& e : batch) apply(e);
            batchingEvents = false;
            if (viewRebuildPending) rebuildViews();
            // Vet-fix: cap transcript after the batch settles so the cap
            // becomes part of the same tick budget as the rebuild rather
            // than a second O(N) pass mid-tick.
            enforceRowCap();
        }
        settleAsk(bridge);
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
