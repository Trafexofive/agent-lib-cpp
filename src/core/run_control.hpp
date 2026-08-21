#pragma once
// Per-agent run stop + process-wide hard kill.
//
// SIGINT/SIGTERM → g_hardKill (whole process).
// TUI Ctrl-X / slash stop → Agent::runControl_.requestStop (that agent +
// children the TUI walks). Stream stall recovery → clearSoft() on THIS
// agent's RunControl only — never store g_hardKill=false.

#include <atomic>
#include <cstdint>

namespace cortex::mk3 {

enum class RunStopKind : uint8_t {
    None = 0,
    Operator = 1,          // Ctrl-C/X, slash stop, TUI stopAgentLoop
    ExternalSignal = 2,    // SIGTERM / external timeout / kill
    StreamAbort = 3,       // provider stall/callback abort (retryable)
};

inline std::atomic<bool> g_hardKill{false};
inline std::atomic<uint8_t> g_hardKillKind{
    static_cast<uint8_t>(RunStopKind::None)};

struct RunControl {
    std::atomic<bool> running{true};
    std::atomic<uint8_t> kind{static_cast<uint8_t>(RunStopKind::None)};

    bool isRunning() const {
        if (g_hardKill.load(std::memory_order_acquire))
            return false;
        return running.load(std::memory_order_acquire);
    }
    RunStopKind stopKind() const {
        if (g_hardKill.load(std::memory_order_acquire)) {
            return static_cast<RunStopKind>(
                g_hardKillKind.load(std::memory_order_acquire));
        }
        return static_cast<RunStopKind>(kind.load(std::memory_order_acquire));
    }
    void requestStop(RunStopKind k) {
        kind.store(static_cast<uint8_t>(k), std::memory_order_release);
        running.store(false, std::memory_order_release);
    }
    // Soft recover after stream stall/abort — THIS agent only.
    void clearSoft() {
        kind.store(static_cast<uint8_t>(RunStopKind::None),
                   std::memory_order_release);
        running.store(true, std::memory_order_release);
    }
    void armTurn() {
        if (g_hardKill.load(std::memory_order_acquire))
            return;
        const auto k = static_cast<RunStopKind>(kind.load(std::memory_order_acquire));
        // Parent walk-stop must survive child prompt() TLS arm.
        if (k == RunStopKind::Operator || k == RunStopKind::ExternalSignal)
            return;
        clearSoft();
    }
};

inline thread_local RunControl* tlsRunControl = nullptr;

inline bool runIsActive() {
    if (g_hardKill.load(std::memory_order_acquire))
        return false;
    if (tlsRunControl)
        return tlsRunControl->isRunning();
    return true;
}

inline RunStopKind currentRunStopKind() {
    if (g_hardKill.load(std::memory_order_acquire)) {
        return static_cast<RunStopKind>(
            g_hardKillKind.load(std::memory_order_acquire));
    }
    if (tlsRunControl)
        return tlsRunControl->stopKind();
    return RunStopKind::None;
}

inline void requestHardKill(RunStopKind kind = RunStopKind::Operator) {
    g_hardKillKind.store(static_cast<uint8_t>(kind), std::memory_order_release);
    g_hardKill.store(true, std::memory_order_release);
    if (tlsRunControl)
        tlsRunControl->requestStop(kind);
}

inline void clearHardKill() {
    g_hardKillKind.store(static_cast<uint8_t>(RunStopKind::None),
                         std::memory_order_release);
    g_hardKill.store(false, std::memory_order_release);
}

// SIGINT / process bootstrap. TUI per-agent stop must NOT use this.
inline void requestRunStop(RunStopKind kind) { requestHardKill(kind); }

inline void clearRunStop() {
    clearHardKill();
    if (tlsRunControl)
        tlsRunControl->clearSoft();
}

struct TlsRunGuard {
    RunControl* prev;
    explicit TlsRunGuard(RunControl* rc) : prev(tlsRunControl) {
        tlsRunControl = rc;
        if (rc)
            rc->armTurn();
    }
    ~TlsRunGuard() { tlsRunControl = prev; }
    TlsRunGuard(const TlsRunGuard&) = delete;
    TlsRunGuard& operator=(const TlsRunGuard&) = delete;
};

// Compatibility for `if (g_running)` / `g_running = false` / `.load/.store`.
// Assignment true NEVER clears g_hardKill. Soft-clear is TLS-only.
struct RunningView {
    operator bool() const { return runIsActive(); }
    bool load(std::memory_order = std::memory_order_seq_cst) const {
        return runIsActive();
    }
    void store(bool v, std::memory_order = std::memory_order_seq_cst) const {
        if (v) {
            if (tlsRunControl)
                tlsRunControl->clearSoft();
            return;
        }
        if (tlsRunControl)
            tlsRunControl->requestStop(RunStopKind::Operator);
        else
            requestHardKill(RunStopKind::Operator);
    }
    RunningView& operator=(bool v) {
        store(v);
        return *this;
    }
};

inline RunningView g_running{};

}  // namespace cortex::mk3
