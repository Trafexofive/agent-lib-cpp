#pragma once
// =============================================================================
// InkcellRuntime — single place for wake/tick coalesce + session flush RAII (F5)
// Product owns domain; this owns the inkcell Engine glue Cortex kept re-copying.
// =============================================================================

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "inkcell/app.hpp"
#include "src/core/agent.hpp"
#include "src/session/controller.hpp"
#include "src/ui/bridge/agent_bridge.hpp"
#include "src/ui/gfx/field_raster.hpp"
#include "src/ui/model/inkcell_app_model.hpp"

namespace cortex::mk3::ui {

// ── Session flush (explicit + atexit safety net) ─────────────────────────────

inline void flushAgentSession(Agent& agent, const std::string& sessionId, bool ephemeral) {
    // Drain async ui_timeline first when we have an id — even if save is skipped.
    if (!sessionId.empty() && !ephemeral && !session::activeSession().isEphemeral())
        session::AsyncUiTimelineWriter::instance().flush();
    if (ephemeral || sessionId.empty()) return;
    if (session::activeSession().isEphemeral()) return;
    agent.saveSession(sessionId);
    session::AsyncUiTimelineWriter::instance().flush();
}

namespace flush {

struct State {
    std::mutex mtx;
    Agent* agent = nullptr;
    std::string sessionId;
    std::string cfgSessionId;
    bool active = false;
};

inline State& state() {
    static State s;
    return s;
}

inline void activate(Agent& agent, const std::string& cfgSessionId) {
    std::lock_guard<std::mutex> g(state().mtx);
    state().agent = &agent;
    state().cfgSessionId = cfgSessionId;
    state().active = true;
}

inline void setActiveSession(const std::string& sessionId) {
    std::lock_guard<std::mutex> g(state().mtx);
    state().sessionId = sessionId;
}

inline void disarm() {
    std::lock_guard<std::mutex> g(state().mtx);
    state().active = false;
}

inline void runOnce() {
    Agent* a = nullptr;
    bool wasActive = false;
    {
        std::lock_guard<std::mutex> g(state().mtx);
        wasActive = state().active;
        a = state().agent;
        state().active = false;
    }
    if (!wasActive || a == nullptr) return;
    std::string sid = session::activeSession().get();
    if (sid.empty()) {
        std::lock_guard<std::mutex> g(state().mtx);
        sid = state().sessionId.empty() ? state().cfgSessionId : state().sessionId;
    }
    flushAgentSession(*a, sid, session::activeSession().isEphemeral());
    session::AsyncUiTimelineWriter::instance().flush();
}

inline void installAtexit() {
    static std::once_flag once;
    std::call_once(once, []() { std::atexit(&runOnce); });
}

}  // namespace flush

// RAII: activate flush on construct, flush+disarm on destroy if still armed.
// Use around every app.run() so SIGINT atexit path and normal return both work.
class SessionFlushGuard {
   public:
    SessionFlushGuard(Agent& agent, std::string cfgSessionId, bool noSession)
        : agent_(&agent), cfgSessionId_(std::move(cfgSessionId)), noSession_(noSession) {
        flush::installAtexit();
        flush::activate(*agent_, cfgSessionId_);
    }

    SessionFlushGuard(const SessionFlushGuard&) = delete;
    SessionFlushGuard& operator=(const SessionFlushGuard&) = delete;

    ~SessionFlushGuard() {
        if (!armed_) return;
        finish(/*model=*/nullptr);
    }

    void setSessionId(const std::string& sid) { flush::setActiveSession(sid); }

    // Normal exit: sync model id, flush, disarm (atexit becomes no-op).
    void finish(const std::shared_ptr<ShellModel>& model) {
        if (!armed_) return;
        armed_ = false;
        if (model && !model->activeSessionId.empty())
            session::activeSession().set(model->activeSessionId, noSession_);
        std::string sid = session::activeSession().get();
        if (sid.empty() && model)
            sid = model->activeSessionId.empty() ? cfgSessionId_ : model->activeSessionId;
        if (sid.empty()) sid = cfgSessionId_;
        flushAgentSession(*agent_, sid, noSession_ || session::activeSession().isEphemeral());
        if (model) model->persistUiTimelineFlush();
        flush::setActiveSession(sid);
        flush::disarm();
    }

    // Hot-swap: point atexit at a new live agent without finishing.
    void rebind(Agent& agent, const std::string& sessionId) {
        agent_ = &agent;
        flush::activate(agent, sessionId);
        if (!sessionId.empty()) flush::setActiveSession(sessionId);
    }

   private:
    Agent* agent_ = nullptr;
    std::string cfgSessionId_;
    bool noSession_ = false;
    bool armed_ = true;
};

// ── Engine tick / wake (COOKBOOK: coalesce on tick, not on wake) ─────────────

// Extra tick work after drain + pendingRoute handling (one-shot exit, REPL workers).
using TickHook = std::function<void(inkcell::App&, ShellModel&, AgentBridge&)>;

// Wire wake_fd + 33ms tick: drain bridge once per frame; route pendingRoute.
// Do not drain on wake — that is the wake-storm anti-pattern.
inline void installCoalescedTick(inkcell::App& app, AgentBridge& bridge,
                                 const std::shared_ptr<ShellModel>& model,
                                 TickHook extra = {}) {
    app.engine().input_poll_ms(33).wake_fd(bridge.wakeFd()).on_wake([]() {});
    // Idle-skip is legal only if live chrome marks. Field / palette / notice
    // used to freeze after the first present (one key → one frame → still).
    app.engine().skip_idle_draw(true);
    app.engine().on_tick([model, &bridge, &app, extra = std::move(extra)](inkcell::Tick t) {
        model->drain(bridge);
        bool live = model->running || model->routeTicks > 0;
        if (model->cmdPalette.active()) live = true;
        if (model->dashboard.noticeExpireAtMs > 0) live = true;
        if (model->dashboard.navAnimating()) live = true;
        if (gfx::fieldEnabled()) {
            // Wallpaper tax: 10 Hz, not 30. Keys still present immediately.
            static uint64_t lastFieldMark = 0;
            if (t.now_ms - lastFieldMark >= 100) {
                lastFieldMark = t.now_ms;
                live = true;
            }
        }
        if (live) {
            if (t.clock) t.clock->mark();
            else app.engine().clock().mark();
        }
        // Extra first so hub launch/submit can set pendingRoute same frame.
        if (extra) extra(app, *model, bridge);
        const PendingRoute route = model->pendingRoute;
        if (route == PendingRoute::None) return;
        model->clearRoute();
        if (pendingRoutePersistBefore(route)) model->persistUiTimeline();
        const char* action = pendingRouteAction(route);
        if (action && action[0]) app.engine().post_action(inkcell::Action{action});
    });
}

}  // namespace cortex::mk3::ui
