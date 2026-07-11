#pragma once
// UI context/status model. Pure state, no rendering or terminal dependency.

#include <string>

namespace cortex::mk3::ui::model {

enum class AppView {
    Welcome,
    AgentHistory,
    EventDetail,
    CommandPalette,
    ManifestSurface,
    SessionBrowser,
    ProviderPicker,
    AskModal,
    ConfirmModal,
    HelpOverlay,
    ResizeNotice,
};

enum class FocusPane {
    None,
    Composer,
    History,
    Detail,
    Palette,
    Modal,
    List,
};

enum class RunLifecycle {
    Idle,
    WaitingProvider,
    ParsingProtocol,
    StreamingResponse,
    RunningTools,
    WaitingHumanInput,
    Cancelling,
    Cancelled,
    Complete,
    Error,
};

struct ProviderContext {
    std::string provider;
    std::string model;
    bool configured = false;
    bool authAvailable = false;
    bool rateLimited = false;
    std::string statusMessage;
};

struct SessionContext {
    std::string id;
    std::string name;
    bool loaded = false;
    bool staleReplay = false;
    int turnCount = 0;
};

struct ManifestContext {
    std::string path;
    std::string agentName;
    bool loaded = false;
    bool valid = true;
    std::string validationError;
};

struct RunContext {
    RunLifecycle lifecycle = RunLifecycle::Idle;
    int pendingOps = 0;
    int actionCount = 0;
    int resultCount = 0;
    bool cancelable = false;
};

struct UiContext {
    AppView view = AppView::AgentHistory;
    FocusPane focus = FocusPane::Composer;
    ProviderContext provider;
    SessionContext session;
    ManifestContext manifest;
    RunContext run;
    bool hasSelection = false;
    bool selectedDrillable = false;
    bool selectedHasDetail = false;
    bool selectedError = false;
    bool atRootAgent = true;
    bool composerHasText = false;
};

inline const char* appViewName(AppView view) {
    switch (view) {
        case AppView::Welcome: return "welcome";
        case AppView::AgentHistory: return "agent_history";
        case AppView::EventDetail: return "event_detail";
        case AppView::CommandPalette: return "command_palette";
        case AppView::ManifestSurface: return "manifest_surface";
        case AppView::SessionBrowser: return "session_browser";
        case AppView::ProviderPicker: return "provider_picker";
        case AppView::AskModal: return "ask_modal";
        case AppView::ConfirmModal: return "confirm_modal";
        case AppView::HelpOverlay: return "help_overlay";
        case AppView::ResizeNotice: return "resize_notice";
    }
    return "agent_history";
}

inline const char* runLifecycleName(RunLifecycle lifecycle) {
    switch (lifecycle) {
        case RunLifecycle::Idle: return "idle";
        case RunLifecycle::WaitingProvider: return "waiting provider";
        case RunLifecycle::ParsingProtocol: return "parsing protocol";
        case RunLifecycle::StreamingResponse: return "streaming response";
        case RunLifecycle::RunningTools: return "running tools";
        case RunLifecycle::WaitingHumanInput: return "waiting human input";
        case RunLifecycle::Cancelling: return "cancelling";
        case RunLifecycle::Cancelled: return "cancelled";
        case RunLifecycle::Complete: return "complete";
        case RunLifecycle::Error: return "error";
    }
    return "idle";
}

inline bool runActive(RunLifecycle lifecycle) {
    return lifecycle == RunLifecycle::WaitingProvider || lifecycle == RunLifecycle::ParsingProtocol ||
           lifecycle == RunLifecycle::StreamingResponse || lifecycle == RunLifecycle::RunningTools ||
           lifecycle == RunLifecycle::WaitingHumanInput || lifecycle == RunLifecycle::Cancelling;
}

inline std::string providerLabel(const ProviderContext& ctx) {
    if (ctx.provider.empty() && ctx.model.empty()) return "provider: unset";
    if (ctx.model.empty()) return ctx.provider;
    return ctx.provider + "/" + ctx.model;
}

inline std::string globalStatusLine(const UiContext& ctx) {
    std::string out = providerLabel(ctx.provider);
    out += " · ";
    out += runLifecycleName(ctx.run.lifecycle);
    if (ctx.run.pendingOps > 0) out += " · pending " + std::to_string(ctx.run.pendingOps);
    if (!ctx.session.id.empty()) {
        std::string suffix = ctx.session.id.size() > 8 ? ctx.session.id.substr(ctx.session.id.size() - 8) : ctx.session.id;
        out += " · session:…" + suffix;
    }
    if (ctx.session.staleReplay) out += " · stale replay";
    return out;
}

}  // namespace cortex::mk3::ui::model
