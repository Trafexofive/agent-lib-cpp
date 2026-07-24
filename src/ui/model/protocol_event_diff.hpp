#pragma once
// Protocol event baseline diff — pure functions, no TUI deps beyond UiEvent.
//
// Used by the worker→UI stream path to emit only dirty protocol rows.
// Extracted so the RETRY-rotation OOB regression can be unit-tested without
// standing up inkcell / Agent / bridge.

#include <cstddef>
#include <vector>

#include "src/core/types.hpp"
#include "src/ui/bridge/ui_event.hpp"

namespace cortex::mk3::ui {

inline bool sameProtocolEvent(const ProtocolEvent& a, const ProtocolEvent& b) {
    if (a.kind != b.kind || a.text != b.text) return false;
    if (a.kind == ProtocolEventKind::ACTION) {
        return a.action.type == b.action.type && a.action.name == b.action.name &&
               a.action.id == b.action.id && a.action.body == b.action.body &&
               a.action.mode == b.action.mode && a.action.modifiers == b.action.modifiers;
    }
    if (a.kind == ProtocolEventKind::RESULT) {
        return a.result.id == b.result.id && a.result.ok == b.result.ok &&
               a.result.summary == b.result.summary && a.result.toolName == b.result.toolName &&
               a.result.exitCode == b.result.exitCode && a.result.elapsedMs == b.result.elapsedMs &&
               a.result.outputBytes == b.result.outputBytes;
    }
    return true;
}

// Diff `current` against `previous`, appending UiEvents for dirty/new slots.
// Mutates `previous` to match `current` for the next call.
//
// Retry contract (agent clears protocolEvents_ then pushes RETRY):
//   1. Detect RETRY at current[0] before any resize.
//   2. Clear previous on rotation (stale attempt-N baseline).
//   3. Truncate previous if longer than current — never pad before the loop.
//   4. Assign-or-push_back so growth stays in-bounds.
//
// Writing previous[i] after previous.clear() with i < current.size() was a
// pure OOB vector write (tcache_thread_shutdown / unaligned tcache / SEGV
// on empty-response retry). Do not reintroduce pad-then-clear-then-index.
inline void collectProtocolChanges(std::vector<UiEvent>& out,
                                   const std::vector<ProtocolEvent>& current,
                                   std::vector<ProtocolEvent>& previous) {
    bool rotatedAtZero = !current.empty() && current[0].kind == ProtocolEventKind::RETRY &&
                         (previous.empty() || previous[0].kind != ProtocolEventKind::RETRY ||
                          !sameProtocolEvent(current[0], previous[0]));
    if (rotatedAtZero) previous.clear();

    if (previous.size() > current.size()) previous.resize(current.size());

    for (size_t i = 0; i < current.size(); ++i) {
        if (i >= previous.size() || !sameProtocolEvent(current[i], previous[i])) {
            out.push_back(UiEvent::protocolEvent(current[i], i));
            if (i < previous.size()) {
                previous[i] = current[i];
            } else {
                previous.push_back(current[i]);
            }
        }
    }
}

}  // namespace cortex::mk3::ui
