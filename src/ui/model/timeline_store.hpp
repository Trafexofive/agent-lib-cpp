#pragma once
// =============================================================================
// TimelineStore — pure row storage + protocol index map (foundation F3).
// No Surface, no Agent*, no rebuildViews. Callers apply projection after.
// =============================================================================

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <vector>

#include "src/ui/model/timeline_codec.hpp"
#include "src/ui/text/sanitize.hpp"

namespace cortex::mk3::ui {

struct TimelineStore {
    std::deque<TimelineRow> rootRows;
    std::deque<TimelineRow> nestedRows;
    std::vector<int> activeProtocolRows;
    std::set<std::string> pendingActionIds;
    std::set<std::string> completedResultIds;

    int tokenBytes = 0;
    int actionCount = 0;
    int resultCount = 0;
    int pendingOps = 0;
    PageState timelineState = PageState::Empty;

    // Incremental projection bookkeeping (owned with rows so caps stay consistent).
    size_t projDirtyFrom = 0;
    std::vector<int> rootRowLineStart;

    static constexpr int kRootRowCap = 600;
    static constexpr std::size_t kBodyCap = 8 * 1024;

    void clear() {
        rootRows.clear();
        nestedRows.clear();
        activeProtocolRows.clear();
        pendingActionIds.clear();
        completedResultIds.clear();
        tokenBytes = 0;
        actionCount = 0;
        resultCount = 0;
        pendingOps = 0;
        timelineState = PageState::Empty;
        markProjFull();
    }

    void clearProtocolEpoch() {
        activeProtocolRows.clear();
        pendingActionIds.clear();
        completedResultIds.clear();
        pendingOps = 0;
    }

    void markProjDirty(size_t rootIndex) {
        if (rootIndex < projDirtyFrom) projDirtyFrom = rootIndex;
    }

    void markProjFull() {
        projDirtyFrom = 0;
        rootRowLineStart.clear();
    }

    // Cap + sanitize body; append to rootRows. Does not project.
    // Returns true if a row was appended.
    bool appendRoot(TimelineRow row) {
        clampAndSanitize(row);
        rootRows.push_back(std::move(row));
        timelineState = PageState::Populated;
        markProjDirty(rootRows.size() - 1);
        return true;
    }

    void upsertProtocol(size_t index, TimelineRow row) {
        // Same body policy as appendRoot (cap + symbol-dump collapse).
        clampAndSanitize(row);
        if (activeProtocolRows.size() <= index) activeProtocolRows.resize(index + 1, -1);
        int& mapped = activeProtocolRows[index];
        if (mapped >= 0 && mapped < static_cast<int>(rootRows.size())) {
            rootRows[static_cast<size_t>(mapped)] = std::move(row);
            markProjDirty(static_cast<size_t>(mapped));
        } else {
            mapped = static_cast<int>(rootRows.size());
            rootRows.push_back(std::move(row));
            markProjDirty(static_cast<size_t>(mapped));
        }
        timelineState = PageState::Populated;
    }

    void setStreamProgress(int bytes) {
        tokenBytes = bytes;
        if (!rootRows.empty() && rootRows.back().kind == TimelineKind::Stream) {
            rootRows.back().title = "stream";
            rootRows.back().body = std::to_string(bytes) + " bytes received";
            markProjDirty(rootRows.size() - 1);
        } else {
            rootRows.push_back(
                {TimelineKind::Stream, "stream", std::to_string(bytes) + " bytes received", true});
            timelineState = PageState::Populated;
            markProjDirty(rootRows.size() - 1);
        }
    }

    // Drop oldest Thought/Stream rows until under cap. Returns rows dropped.
    // Shifts activeProtocolRows; marks full projection.
    size_t enforceRowCap(int& selectedBlock) {
        if (static_cast<int>(rootRows.size()) < kRootRowCap) return 0;
        int excess = static_cast<int>(rootRows.size()) - kRootRowCap;
        size_t drop = 0;
        while (static_cast<int>(drop) < excess) {
            const auto& r = rootRows[drop];
            if (r.kind == TimelineKind::Thought || r.kind == TimelineKind::Stream) {
                ++drop;
            } else {
                break;
            }
        }
        if (drop == 0) return 0;
        for (size_t i = 0; i < drop; ++i) rootRows.pop_front();
        selectedBlock = std::max(0, selectedBlock - static_cast<int>(drop));
        for (auto& idx : activeProtocolRows) {
            if (idx >= 0) idx -= static_cast<int>(drop);
        }
        timelineState = PageState::Populated;
        markProjFull();
        return drop;
    }

    template <typename RowRange>
    static int countFocusable(const RowRange& rows, bool showThoughtsFlag = true) {
        int n = 0;
        for (const auto& row : rows) {
            if (row.kind == TimelineKind::Thought && !showThoughtsFlag) continue;
            if (row.kind == TimelineKind::Stream) continue;
            ++n;
        }
        return n;
    }

    static void clampAndSanitize(TimelineRow& row) {
        if (row.body.size() > kBodyCap) {
            std::string head = row.body.substr(0, kBodyCap - 80);
            std::string tail;
            if (row.body.size() > kBodyCap + 256) {
                tail = row.body.substr(row.body.size() - 256, 256);
            } else {
                tail = row.body.substr(kBodyCap - 80);
            }
            std::string body;
            body.reserve(kBodyCap);
            body.append(head);
            body.append("\n\n  … [truncated for chat — row was ");
            body.append(std::to_string(row.body.size()));
            body.append(" bytes] …\n\n");
            body.append(tail);
            row.body = std::move(body);
        }
        if (!row.body.empty()) row.body = sanitizeForDisplay(row.body);
    }
};

}  // namespace cortex::mk3::ui
