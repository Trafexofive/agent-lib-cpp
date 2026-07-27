#pragma once
// Timeline → transcriptView projection (F4). Out-of-line ShellModel methods.
// Included at the bottom of inkcell_app_model.hpp after ShellModel is complete.

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace cortex::mk3::ui {

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
            if (type == "AGENT" && !row.actionName.empty()) {
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
        auto bodyLines = splitDisplayLines(row.body);
        int shown = 0;
        int total = 0;
        for (const auto& line : bodyLines) {
            if (line.empty() && row.body.empty()) continue;
            ++total;
        }
        // Thoughts are operator-secondary; hard-cap tighter so a runaway
        // stream cannot paint 50×wide lines every frame and stall input.
        const int bodyCap =
            (row.kind == TimelineKind::Thought) ? 12 : kMaxBodyLines;
        for (const auto& line : bodyLines) {
            if (line.empty() && row.body.empty()) continue;
            if (truncateBodies && shown >= bodyCap) break;
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

    if (running && atRoot() && !timelineFocus) {
        transcriptView.stick_bottom = true;
        transcriptView.scroll_to_end();
    } else if (timelineFocus && focusable > 0) {
        transcriptView.stick_bottom = false;
        ensureSelectionVisible();
    } else if (transcriptView.stick_bottom) {
        transcriptView.scroll_to_end();
    }
    rebuildInspector();
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
    if (!atRoot() || timelineFocus) return false;
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
    bool didInc = false;
    if (atRoot() && !timelineFocus && projDirtyFrom > 0 &&
        projDirtyFrom <= rootRows.size() && !rootRowLineStart.empty()) {
        didInc = tryRebuildViewsIncremental();
    }
    if (!didInc) {
        rebuildViewsFull();
    }
    ++transcriptVersion;  // wrap cache: dirty-tail rewrap via source snapshot
    finishRebuildScroll();
}

inline void ShellModel::ensureSelectionVisible() {
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


}  // namespace cortex::mk3::ui
