#pragma once
// ShellModel boot: prefs/theme/CWD policy → session identity → dashboard
// refresh → prompt history → unified session resume (ui_timeline or records).

#include <cstdlib>
#include <unistd.h>

#include "src/session/controller.hpp"
#include "src/session/manager.hpp"
#include "src/ui/chat/prompt_history.hpp"
#include "src/ui/model/inkcell_app_model.hpp"
#include "src/ui/model/ui_prefs.hpp"

namespace cortex::mk3::ui {

inline void initializeChatModel(const std::shared_ptr<ShellModel> &model,
                                const InkcellAppConfig &cfg) {
    // Disk prefs first; MK3_TUI_THEME env still wins when set.
    loadUiPrefs();
    if (const char *requestedTheme = std::getenv("MK3_TUI_THEME")) {
        std::string value = requestedTheme;
        if (value == "neon")
            theme::set(theme::Variant::Neon);
        else
            theme::set(theme::Variant::Graphite);
    }
    applyUiPrefsToModel(*model);
    // Launch-time CWD policy: rememberLastCwd=ON → chdir to persisted path;
    // OFF (default) → drop the persisted value, use launch dir as process CWD.
    applyLaunchCwd(*model);
    // Capture launch CWD before any chdir above (or after, if
    // rememberLastCwd was OFF). Used by the Settings CWD cycle.
    {
        char buf[1024] = {0};
        if (::getcwd(buf, sizeof(buf) - 1))
            model->launchCwd = buf;
    }
    model->activeSessionId = cfg.sessionId;
    // Single active id: seed process-wide SessionRef (kills dual-flush).
    // only --no-session suppresses disk; --ephemeral is exit-on-done
    // (orthogonal).
    session::activeSession().set(cfg.sessionId, cfg.noSession);
    // Wire the agent display identity so the chat transcript labels the
    // assistant's own turns with the real agent name + model/provider (not the
    // generic "CORTEX" sentinel) and subagent turns with the subagent name.
    model->agentName = cfg.agentName.empty() ? "cortex" : cfg.agentName;
    model->agentModel = cfg.model;
    model->agentProvider = cfg.provider;
    model->activeManifestPath = cfg.manifestPath;
    model->showThoughts = cfg.showThoughts;
    model->truncateBodies = cfg.truncateBodies;
    model->dashboard.manifestDir =
        !cfg.manifestDir.empty() ? cfg.manifestDir : cfg.manifestPath;
    model->dashboard.refreshAll(uiPrefShadow().globalSessions);
    if (cfg.startAtManifests) {
        model->dashboard.select(model::DashboardSection::Manifests);
        model->dashboard.refreshManifests();
    }
    model->promptHistory = chat::loadPromptHistory();
    model->promptHistoryIndex = static_cast<int>(model->promptHistory.size());
    // Unified resume: prefer ui_timeline, fall back to records (session audit
    // S0.3).
    if (!cfg.sessionId.empty()) {
        session::SessionManager sessions;
        if (sessions.exists(cfg.sessionId)) {
            // Restore parent + subagent runtime state BEFORE painting timeline
            // so ↳ drill has nested history (first-start works because prompt()
            // loads this lazily; resume never called prompt yet).
            if (model->rootAgent) {
                model->rootAgent->loadSession(cfg.sessionId);
                model->rootAgent->loadStateCheckpoint(cfg.sessionId);
            }
            model->loadSessionUi(sessions.load(cfg.sessionId));
            // Live Agent config always wins over session file identity fields.
            if (model->rootAgent) {
                const auto &c = model->rootAgent->config();
                if (!c.name.empty())
                    model->agentName = c.name;
                if (!c.model.empty())
                    model->agentModel = c.model;
                if (!c.provider.empty())
                    model->agentProvider = c.provider;
            }
        }
    }
}
} // namespace cortex::mk3::ui
