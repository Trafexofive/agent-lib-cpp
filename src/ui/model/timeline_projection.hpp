#pragma once
// Timeline → transcriptView projection (F4). Out-of-line ShellModel methods.
// Included at the bottom of inkcell_app_model.hpp after ShellModel is complete.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "inkcell/text.hpp"

namespace cortex::mk3::ui {

// Display-line count for one source row (aligned with chat wrap semantics).
// Kept local to avoid pulling chat_view.hpp into the model include graph.
inline int countSourceDisplaySpan(const std::string& original, int width) {
    width = std::max(1, width);
    if (original.empty()) return 1;
    // hard-wrap by display width — same family as chat hardWrapUtf8.
    int columns = 0;
    int lines = 1;
    for (size_t i = 0; i < original.size();) {
        size_t len = 1;
        unsigned char ch = static_cast<unsigned char>(original[i]);
        if ((ch & 0xE0) == 0xC0) len = 2;
        else if ((ch & 0xF0) == 0xE0) len = 3;
        else if ((ch & 0xF8) == 0xF0) len = 4;
        if (i + len > original.size()) len = 1;
        int gw = inkcell::text::display_width(original.substr(i, len));
        if (gw <= 0) gw = (len == 1 && original[i] == ' ') ? 1 : 0;
        if (gw == 0) {
            i += len;
            continue;
        }
        if (columns + gw > width && columns > 0) {
            ++lines;
            columns = 0;
        }
        columns += gw;
        i += len;
    }
    return std::max(1, lines);
}

// Compact live well under a spawned child — same height budget as truncated
// tool results (14). Drill for the full chat. Never dump the child's protocol.
inline void appendChildWell(std::vector<std::string>& lines, Agent* child,
                            const std::string& name, bool live, int maxLines) {
    maxLines = std::max(4, std::min(maxLines, 14));
    const std::string nm = name.empty() ? "subagent" : name;
    std::string head = "    ┌ " + nm + (live ? "  ·  ● LIVE" : "  ·  ○ done");
    if (child) {
        const auto& evs = child->protocolEvents();
        int acts = 0, ress = 0;
        std::string last;
        for (const auto& pe : evs) {
            if (pe.kind == ProtocolEventKind::ACTION) {
                ++acts;
                last = pe.action.name.empty() ? pe.action.type : pe.action.name;
            } else if (pe.kind == ProtocolEventKind::RESULT) {
                ++ress;
            } else if (pe.kind == ProtocolEventKind::THOUGHT && last.empty()) {
                last = "thinking";
            } else if (pe.kind == ProtocolEventKind::RESPONSE) {
                last = "reply";
            }
        }
        if (!last.empty()) head += "  ·  " + last;
        lines.push_back(head);
        int used = 1;
        char meta[80];
        std::snprintf(meta, sizeof(meta), "    │  act%d · res%d · hist %zu", acts, ress,
                      child->history().size());
        lines.push_back(meta);
        ++used;
        // Tail of last few actions — one token each, no bodies.
        int room = maxLines - used - 1;
        std::vector<std::string> tail;
        for (auto it = evs.rbegin(); it != evs.rend() && static_cast<int>(tail.size()) < room; ++it) {
            if (it->kind != ProtocolEventKind::ACTION && it->kind != ProtocolEventKind::RESULT)
                continue;
            std::string t;
            if (it->kind == ProtocolEventKind::ACTION)
                t = std::string("    │  ▸ ") + (it->action.name.empty() ? it->action.type : it->action.name);
            else
                t = std::string(it->result.ok ? "    │  ✓ " : "    │  ✗ ") + it->result.toolName;
            if (t.size() > 56) t = t.substr(0, 54) + "…";
            tail.push_back(std::move(t));
        }
        std::reverse(tail.begin(), tail.end());
        for (auto& t : tail) {
            lines.push_back(std::move(t));
            ++used;
        }
        lines.push_back(live ? "    └ ↳  enter · live" : "    └ ↳  enter");
    } else {
        lines.push_back(head);
        lines.push_back("    │  (no instance yet)");
        lines.push_back("    └ ↳  enter");
    }
}

// Emit one root/nested row into transcriptView.lines (+ optional block index).
// Returns true if the row produced a focusable block.
inline bool ShellModel::projectOneRow(const TimelineRow& row, int ri, int& focusIdx, const std::string& scopeName,
                   bool recordRootLineStart) {
    if (row.kind == TimelineKind::Thought && !showThoughts) {
        if (recordRootLineStart) rootRowLineStart.push_back(-1);
        return false;
    }
    if (row.kind == TimelineKind::Thought) {
        bool any = false;
        for (char c : row.body) {
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                any = true;
                break;
            }
        }
        if (!any) {
            if (recordRootLineStart) rootRowLineStart.push_back(-1);
            return false;
        }
    }
    if (row.kind == TimelineKind::Stream && !showRaw) {
        if (recordRootLineStart) rootRowLineStart.push_back(-1);
        return false;
    }

    bool selected = timelineFocus && (focusIdx == selectedBlock);
    if (recordRootLineStart)
        rootRowLineStart.push_back(static_cast<int>(transcriptView.lines.size()));
    blockRowIndex.push_back(ri);

    std::string label;
    switch (row.kind) {
        case TimelineKind::User:
            if (row.title.rfind("parent:", 0) == 0)
                label = "PARENT  " + row.title.substr(7);
            else
                label = "YOU";
            break;
        case TimelineKind::Thought:
            label = "THOUGHT";
            break;
        case TimelineKind::Action: {
            std::string type = row.actionType.empty() ? "ACTION" : row.actionType;
            std::transform(type.begin(), type.end(), type.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            // Normalize tool → TOOL, feed → FEED, etc.
            if (type == "AGENT" && !row.actionName.empty()) {
                label = "AGENT  " + row.actionName;
            } else {
                label = type;
                if (!row.actionName.empty()) label += "  " + row.actionName;
            }
            if (!row.actionId.empty()) label += "  #" + row.actionId;
            // Rich meta chips from JSON body (path/cmd/url…).
            {
                std::string chips = actionBodyMetaChips(row.body);
                if (!chips.empty()) label += "  ·  " + chips;
            }
            if (row.drillable) label += "  ↳";
            break;
        }
        case TimelineKind::Result: {
            label = row.ok ? "✓ RESULT" : "✗ RESULT";
            if (!row.actionName.empty()) label += "  " + row.actionName;
            if (!row.actionId.empty()) label += "  #" + row.actionId;
            // Meta chips: prefer trailing meta line (…\nms · bytes), else first line.
            if (!row.body.empty()) {
                std::string metaLine;
                size_t lastNl = row.body.find_last_of('\n');
                if (lastNl != std::string::npos && lastNl + 1 < row.body.size()) {
                    std::string tail = row.body.substr(lastNl + 1);
                    if (tail.find("ms") != std::string::npos || tail.find('B') != std::string::npos ||
                        tail.find("exit") != std::string::npos)
                        metaLine = tail;
                }
                if (metaLine.empty()) {
                    size_t nl = row.body.find('\n');
                    metaLine = nl == std::string::npos ? row.body : row.body.substr(0, nl);
                }
                if (!metaLine.empty() &&
                    (metaLine.find("ms") != std::string::npos || metaLine.find('B') != std::string::npos ||
                     metaLine.find("exit") != std::string::npos || !row.ok)) {
                    if (metaLine.size() > 52) metaLine = metaLine.substr(0, 50) + "…";
                    label += "  ·  " + metaLine;
                }
            }
            if (row.drillable) label += "  ↳";
            break;
        }
        case TimelineKind::Response:
        case TimelineKind::Final:
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
            else if (row.title == "steer") label = "⟹ STEER";
            else if (row.title == "timeout") label = "⏱ TIMEOUT";
            else if (row.title == "error") label = "✗ ERROR";
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
    {
        if (row.collapsed) {
            transcriptView.lines.push_back("    ▸ collapsed · za to expand");
            transcriptView.lines.push_back("");
            ++focusIdx;
            return true;
        }
        const bool childRow =
            (row.kind == TimelineKind::Action || row.kind == TimelineKind::Result) &&
            (row.actionType == "agent" ||
             (rootAgent && !row.actionName.empty() && rootAgent->hasSubAgent(row.actionName)));
        if (childRow) {
            Agent* ch = rootAgent ? rootAgent->getSubAgent(row.actionName) : nullptr;
            bool live = !row.actionId.empty() && pendingActionIds.count(row.actionId);
            appendChildWell(transcriptView.lines, ch, row.actionName, live, 14);
            transcriptView.lines.push_back("");
            ++focusIdx;
            return true;
        }
        // Thoughts are operator-secondary; hard-cap tighter so a runaway
        // stream cannot paint 50×wide lines every frame and stall input.
        // Actions/results denser under truncate (ctrl-o); full when expanded.
        int bodyCap = kMaxBodyLines;
        if (row.kind == TimelineKind::Thought) bodyCap = 12;
        else if (truncateBodies && row.kind == TimelineKind::Action) bodyCap = 10;
        else if (truncateBodies && row.kind == TimelineKind::Result) bodyCap = 14;
        else if (truncateBodies && row.kind == TimelineKind::Status) bodyCap = 4;

        // Render mode for action/result bodies (pretty / compact / raw).
        std::string renderedBody = row.body;
        if (row.kind == TimelineKind::Action)
            renderedBody = formatActionBodyForChat(row.body, actionBodyMode);
        else if (row.kind == TimelineKind::Result)
            renderedBody = formatStoredResultBody(row.body, resultBodyMode);
        int shown = 0;
        int total = 0;
        bool more = false;
        size_t start = 0;
        const std::string& body = renderedBody;
        while (start <= body.size()) {
            size_t end = body.find('\n', start);
            if (end == std::string::npos) end = body.size();
            // Skip a single empty body (no lines).
            if (!(start == 0 && end == 0 && body.empty())) {
                ++total;
                if (!truncateBodies || shown < bodyCap) {
                    transcriptView.lines.push_back("    " + body.substr(start, end - start));
                    ++shown;
                } else {
                    more = true;
                    // Keep counting remaining newlines cheaply for the note.
                    if (truncateBodies) {
                        size_t p = end;
                        while (p < body.size()) {
                            if (body[p] == '\n') ++total;
                            ++p;
                        }
                        // last line after final newline already counted via loop structure
                        break;
                    }
                }
            }
            if (end == body.size()) break;
            start = end + 1;
        }
        if (truncateBodies && more && total > shown) {
            transcriptView.lines.push_back(
                "    … " + std::to_string(total - shown) + " more lines · " +
                std::to_string(body.size()) + "B — za or /truncate");
        }
    }
    transcriptView.lines.push_back("");
    ++focusIdx;
    return true;
}

inline void ShellModel::rebuildInspector() {
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
        for (int i = static_cast<int>(eventLog.size()) - 1;
             i >= 0 && inspectorView.lines.size() < 200; --i)
            inspectorView.lines.push_back(eventLog[static_cast<size_t>(i)]);
    }
    if (inspectorView.stick_bottom) inspectorView.scroll_to_end();
}

inline void ShellModel::finishRebuildScroll() {
    int focusable = static_cast<int>(blockRowIndex.size());
    if (focusable <= 0) selectedBlock = 0;
    else selectedBlock = std::max(0, std::min(selectedBlock, focusable - 1));

    // Live lock policy:
    //   stick_bottom is the single source of truth for auto-follow.
    //   We only auto-follow while stick_bottom is set (set when the operator
    //   is on the last block / scrolls to end). Never force-lock just because
    //   running && composer-focused — that stole navigation mid-turn.
    //   ensureSelectionVisible only on explicit selection moves, otherwise
    //   Ctrl-J/K free-scroll is snapped back every rebuild (last-block fight).
    if (transcriptView.stick_bottom) {
        transcriptView.scroll_to_end();
        selectionNavPending = false;
        // Belt: while pinned to live edge, only the selected block may own ›.
        // Incremental path can leave a stale › on the previous last block.
        if (timelineFocus && focusable > 0 && selectedBlock >= 0 &&
            selectedBlock < static_cast<int>(blockRowIndex.size()) &&
            !transcriptView.lines.empty()) {
            int wantLine = -1;
            const int ri = blockRowIndex[static_cast<size_t>(selectedBlock)];
            if (ri >= 0 && ri < static_cast<int>(rootRowLineStart.size()))
                wantLine = rootRowLineStart[static_cast<size_t>(ri)];
            for (int i = 0; i < static_cast<int>(transcriptView.lines.size()); ++i) {
                auto& line = transcriptView.lines[static_cast<size_t>(i)];
                if (line.size() < 2) continue;
                const bool hasMarker = line.rfind("› ", 0) == 0;
                if (i == wantLine) {
                    if (!hasMarker && line.rfind("  ", 0) == 0)
                        line.replace(0, 2, "› ");
                } else if (hasMarker) {
                    line.replace(0, 2, "  ");
                }
            }
        }
    } else if (selectionNavPending && timelineFocus && focusable > 0) {
        // Do NOT clamp offset here with approximate spans — drawTranscript
        // snaps to the real › line after word-wrap (see selectionNavPending).
        // Calling ensureSelectionVisible with hard-wrap math scrolled past the
        // highlight on upward j/k.
    } else {
        transcriptView.clamp();
        selectionNavPending = false;
    }
    // Inspector is secondary chrome — skip while streaming to save a full
    // eventLog walk every frame. Rebuild on idle / turn boundaries.
    if (!running) rebuildInspector();
}

// Full projection (nested views, filter toggles, selection chrome, load).
inline void ShellModel::rebuildViewsFull() {
    const auto& rows = activeRows();
    transcriptView.lines.clear();
    blockRowIndex.clear();
    rootRowLineStart.clear();
    if (rows.empty()) {
        if (atRoot()) timelineState = PageState::Empty;
        projDirtyFrom = rows.size();
        return;
    }
    std::string scopeName = agentName;
    if (!atRoot()) {
        if (Agent* cur = currentAgent()) scopeName = cur->name();
        else if (!agentPath.empty()) scopeName = agentPath.back();
    }
    int focusIdx = 0;
    const bool record = atRoot();
    for (int ri = 0; ri < static_cast<int>(rows.size()); ++ri) {
        projectOneRow(rows[static_cast<size_t>(ri)], ri, focusIdx, scopeName, record);
    }
    if (atRoot() && !rows.empty()) timelineState = PageState::Populated;
    projDirtyFrom = rows.size();
}

// Streaming fast path: re-project only rootRows[projDirtyFrom..).
// Invariants for success:
//   - at root, not timeline-focused
//   - rootRowLineStart.size() == N where N is the previously projected
//     root row count (stable prefix length before this dirty wave)
//   - projDirtyFrom in (0, rootRows.size()]  (0 forces full)
inline bool ShellModel::tryRebuildViewsIncremental() {
    // Timeline focus used to force full rebuild every stream tick (› chrome).
    // projectOneRow already paints › from current selectedBlock on the dirty
    // tail; stable prefix keeps its prior chrome. Allow incremental either way.
    if (!atRoot()) return false;
    if (projDirtyFrom == 0) return false;
    if (projDirtyFrom > rootRows.size()) return false;
    // Map must describe exactly the stable prefix [0, projDirtyFrom).
    // After a previous full/inc pass, size == old rootRows.size() which may
    // be > projDirtyFrom (we dirtied a middle/last row). Accept:
    //   map.size() >= projDirtyFrom  (prefix valid)
    if (rootRowLineStart.size() < projDirtyFrom) return false;

    int cutLine = -1;
    if (projDirtyFrom == rootRowLineStart.size()) {
        // Pure append past previously projected rows.
        cutLine = static_cast<int>(transcriptView.lines.size());
    } else if (rootRowLineStart[projDirtyFrom] >= 0) {
        cutLine = rootRowLineStart[projDirtyFrom];
    } else {
        // Dirty row was filtered last time; find next projected start.
        cutLine = static_cast<int>(transcriptView.lines.size());
        for (size_t j = projDirtyFrom + 1; j < rootRowLineStart.size(); ++j) {
            if (rootRowLineStart[j] >= 0) {
                cutLine = rootRowLineStart[j];
                break;
            }
        }
    }
    if (cutLine < 0 || cutLine > static_cast<int>(transcriptView.lines.size()))
        return false;

    transcriptView.lines.resize(static_cast<size_t>(cutLine));

    // Keep block indices that point only at stable prefix rows.
    std::vector<int> newBlocks;
    newBlocks.reserve(blockRowIndex.size());
    for (int ri : blockRowIndex) {
        if (ri >= 0 && ri < static_cast<int>(projDirtyFrom)) newBlocks.push_back(ri);
    }
    blockRowIndex.swap(newBlocks);
    rootRowLineStart.resize(projDirtyFrom);

    std::string scopeName = agentName.empty() ? "CORTEX" : agentName;
    if (!agentModel.empty() || !agentProvider.empty()) {
        // scopeName for Response labels is just agentName; model meta added in projectOneRow
    }
    int focusIdx = static_cast<int>(blockRowIndex.size());
    for (size_t ri = projDirtyFrom; ri < rootRows.size(); ++ri) {
        projectOneRow(rootRows[ri], static_cast<int>(ri), focusIdx, scopeName, true);
    }
    if (!rootRows.empty()) timelineState = PageState::Populated;
    projDirtyFrom = rootRows.size();
    return true;
}

inline void ShellModel::rebuildViews() {
    if (batchingEvents) {
        viewRebuildPending = true;
        return;
    }
    viewRebuildPending = false;
    ++viewRebuildCount;

    // Cap eviction shifts indices — always full after that.
    // Nested drill uses nestedRows; keep full path.
    // forceFullProject_: trunc/fmt/selection-index jumps cannot use incremental
    // (stable prefix would keep old density or stale › chrome).
    bool didInc = false;
    const bool forceFull = forceFullProject_;
    forceFullProject_ = false;
    if (!forceFull && atRoot() && projDirtyFrom > 0 && projDirtyFrom <= rootRows.size() &&
        !rootRowLineStart.empty()) {
        didInc = tryRebuildViewsIncremental();
    }
    if (!didInc) {
        rebuildViewsFull();
    }
    ++transcriptVersion;  // wrap cache: dirty-tail rewrap via source snapshot
    finishRebuildScroll();
}

inline void ShellModel::ensureSelectionVisible() {
    // Map selected focusable block → DISPLAY line (wrapped).
    // transcriptView.offset is display-space (see ScrollViewState::content_h).
    // Never trust a stale wrap-cache here — recompute spans from source lines.
    if (selectedBlock < 0 || selectedBlock >= static_cast<int>(blockRowIndex.size()))
        return;
    const int ri = blockRowIndex[static_cast<size_t>(selectedBlock)];
    int sourceLine = -1;
    if (ri >= 0 && ri < static_cast<int>(rootRowLineStart.size()) &&
        rootRowLineStart[static_cast<size_t>(ri)] >= 0) {
        sourceLine = rootRowLineStart[static_cast<size_t>(ri)];
    }
    if (sourceLine < 0) return;

    const int wrapW = transcriptWrapCache.width > 0 ? transcriptWrapCache.width : 100;
    int displayLine = 0;
    int totalDisplay = 0;
    const auto& src = transcriptView.lines;
    for (int i = 0; i < static_cast<int>(src.size()); ++i) {
        int span = countSourceDisplaySpan(src[static_cast<size_t>(i)], wrapW);
        if (i < sourceLine) displayLine += span;
        totalDisplay += span;
    }
    transcriptView.content_h = totalDisplay;

    const int vh = std::max(1, transcriptView.viewport_h);
    const int top = transcriptView.offset;
    const int bot = top + vh - 1;
    // Keep a 1-line pad so the selection rail isn't glued to the edge.
    if (displayLine < top + 1) {
        transcriptView.stick_bottom = false;
        transcriptView.offset = std::max(0, displayLine - 1);
        transcriptView.clamp();
    } else if (displayLine > bot - 1) {
        transcriptView.stick_bottom = false;
        transcriptView.offset = std::max(0, displayLine - vh + 2);
        transcriptView.clamp();
    }
}


}  // namespace cortex::mk3::ui
