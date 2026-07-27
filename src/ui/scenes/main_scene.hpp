#pragma once
// Dashboard hub — tight IA, operable facets, real agent launch.
// Pill: Home · Sessions · Manifests · Settings
// ctrl-j = prev · ctrl-k = next (inverted). Enter on agent = hot-swap launch.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "base_scene.hpp"
#include "src/session/controller.hpp"
#include "src/session/manager.hpp"
#include "src/ui/assets/glyphs.hpp"
#include "src/ui/chat/chat_view.hpp"
#include "src/ui/chat/notification.hpp"
#include "src/ui/components/card_swipe.hpp"
#include "src/ui/components/chips.hpp"
#include "src/ui/components/chrome.hpp"
#include "src/ui/components/cmd_palette.hpp"
#include "src/ui/components/pill_nav.hpp"
#include "src/ui/components/workflow_canvas.hpp"
#include "src/ui/components/workflow_rail.hpp"
#include "src/ui/components/workflow_run.hpp"
#include "src/ui/gfx/blit.hpp"
#include "src/ui/gfx/field_raster.hpp"
#include "src/ui/gfx/shaders_dedsec.hpp"
#include "src/ui/layout/density.hpp"
#include "src/ui/layout/sbtui_layout.hpp"
#include "src/ui/model/dashboard_controller.hpp"
#include "src/ui/model/ui_prefs.hpp"
#include "src/ui/model/workflow_runner.hpp"
#include "src/workflows/workflow_engine.hpp"

namespace cortex::mk3::ui::scenes {

class MainScene final : public BaseScene {
   public:
    using BaseScene::BaseScene;
    std::string name() const override { return "Dashboard"; }

    void on_enter() override {
        BaseScene::on_enter();
        loadUiPrefs();  // theme + shader + chrome prefs from ui.json
        applyUiPrefsToModel(*model_);
        model_->dashboard.bumpNavActivity();  // pill visible on hub enter
        if (!cfg_.manifestDir.empty())
            model_->dashboard.manifestDir = cfg_.manifestDir;
        else if (!activeManifest().empty())
            model_->dashboard.manifestDir = activeManifest();
        else
            model_->dashboard.manifestDir.clear();
        model_->dashboard.refreshAll();
        model_->dashboard.focus = model::DashboardFocus::Content;
        model_->dashboard.searchMode = false;
        // Vet-fix: phantom-live UX guard. The hub used to inherit a stale
        // `model_->running = true` carry-over after a previous turn that
        // crashed before TurnDone got published. Reconcile against the
        // bridge's authoritative snapshot so the dashboard never shows
        // "● live" before the operator actually sent anything.
        {
            auto snap = bridge_.snapshot();
            if (!snap.running) {
                model_->running = false;
                if (model_->status.empty() || model_->status == "cancelling")
                    model_->status = "ready";
            }
        }
        bumpNotice();
        highlightActiveAgent();
    }

    bool on_key(const inkcell::KeyEvent& event) override;
    void draw(inkcell::Surface& surface) const override {
        if (layout::render_min_size_notice(surface, 80, 20)) return;
        surface.clear(theme::base_bg());

        const auto full = surface.bounds();
        // Horizontal breathing; bottom keeps 1px page padding. Nav pill floats
        // above the stage (does not own a permanent chrome strip).
        inkcell::Rect page{full.x + 2, full.y + 1, std::max(1, full.w - 4),
                           std::max(1, full.h - 2)};  // 1-cell bottom pad

        const auto tier = layout::densityOf(page.w);
        auto& dash = model_->dashboard;
        const float tsec = gfx::nowSeconds();
        const int tvar = gfx::themeVariantIndex();

        // ── Pill visibility (zen auto-hide + master enable) ───────────────
        const bool pillMaster = model_->navPillEnabled;
        const int hideMs = model_->navPillHideMs;
        bool wantPill = pillMaster;
        if (pillMaster && model_->zenMode && hideMs > 0) {
            if (dash.lastNavActivityMs <= 0) dash.lastNavActivityMs = model::DashboardState::nowMs();
            const int64_t idle = model::DashboardState::nowMs() - dash.lastNavActivityMs;
            wantPill = idle < hideMs || dash.focus == model::DashboardFocus::Dock ||
                       dash.navAnimating();
        } else if (pillMaster && model_->zenMode && hideMs <= 0) {
            wantPill = true;  // zen + never-hide
        }
        // Drive slide animation toward target.
        {
            float target = wantPill ? 1.f : 0.f;
            if (std::fabs(dash.pillRevealTo - target) > 0.01f) dash.requestPillReveal(target);
        }
        const float reveal = pillMaster ? dash.pillRevealT() : 0.f;

        gfx::drawFieldBg(surface, full, tvar, tsec);
        drawAppBar(surface, page, tier);

        // Stage fills the page under the app bar — pill floats over, does not
        // permanently steal rows (1px page pad remains as bg).
        const int pillBodyH = 3;
        int stageTop = page.y + 3;
        int stageBot = page.bottom();  // full page bottom (incl. pad zone)
        // When pill is fully up, keep content clear of the floating dock.
        const int reservedForPill = static_cast<int>(std::lround(reveal * (pillBodyH + 1)));
        int contentBot = stageBot - reservedForPill;
        int stageH = std::max(6, contentBot - stageTop);
        inkcell::Rect stage{page.x, stageTop, page.w, stageH};
        gfx::drawBorderlessPanel(surface, stage, theme::panel_bg());

        inkcell::Rect content{stage.x + 3, stage.y + 1, std::max(1, stage.w - 6),
                              std::max(1, stage.h - 2)};
        const int maxSlide = std::max(4, std::min(12, content.h / 3));
        int yOff = dash.pageSlideRows(maxSlide);
        if (yOff != 0) {
            inkcell::Rect slid = content;
            slid.y += yOff;
            slid.h = std::max(1, content.h - yOff);
            drawContent(surface, slid);
        } else {
            drawContent(surface, content);
        }

        // Hub notice → top-right pop toast
        if (!dash.notice.empty()) {
            inkcell::Rect toastArea{page.x, page.y + 3, page.w, std::min(8, page.h / 3)};
            chat::drawNoticeToast(surface, toastArea, dash.notice);
        }

        // Floating nav pill — draw last so it sits above the stage.
        if (pillMaster && reveal > 0.02f) {
            static const std::vector<components::PillItem> pills = {
                {"g", "Home"},
                {"s", "Sessions"},
                {"a", "Manifests"},
                {"?", "Settings"},
            };
            // Slide up from below: bottomY moves with reveal.
            const int restBottomY = page.bottom() - 1;  // seated
            const int hiddenBottomY = page.bottom() + pillBodyH;  // fully off
            const int pillBottomY =
                static_cast<int>(std::lround(hiddenBottomY + (restBottomY - hiddenBottomY) * reveal));

            // Key hints in the air above the pill when fully revealed.
            if (reveal > 0.85f && dash.notice.empty() && !dash.searchMode) {
                int hintY = pillBottomY - pillBodyH;
                if (hintY >= stageTop) {
                    inkcell::Rect hintRow{page.x, hintY, page.w, 1};
                    auto hints = hubChromeKeyHints(page.w >= 100 ? 7 : 5);
                    hints.theme(theme::activeInkcellTheme()).draw(surface, hintRow);
                }
            }

            components::drawPillDock(surface, page.x, page.w, pillBottomY, pills,
                                     dash.navigationIndex, dash.navPrevIndex, dash.navAnimT(),
                                     dash.focus == model::DashboardFocus::Dock);
        }

        components::drawCmdPalette(surface, full, model_->cmdPalette);
    }

   private:
    // Draw panels — definitions in hub_draw.hpp
    void drawAppBar(inkcell::Surface& surface, inkcell::Rect page, layout::DensityTier tier) const;
    void drawContent(inkcell::Surface& surface, inkcell::Rect frame) const;
    void sectionHead(inkcell::Surface& surface, inkcell::Rect frame, const std::string& title,
                     const std::string& subtitle) const;
    void metricTile(inkcell::Surface& surface, inkcell::Rect r, const std::string& label,
                    const std::string& value, inkcell::Style valueStyle) const;
    void drawHome(inkcell::Surface& surface, inkcell::Rect frame) const;
    void drawSessions(inkcell::Surface& surface, inkcell::Rect frame) const;
    void drawManifests(inkcell::Surface& surface, inkcell::Rect frame) const;
    void drawSettings(inkcell::Surface& surface, inkcell::Rect frame) const;
    void drawWorkflowStage(inkcell::Surface& surface, inkcell::Rect frame,
                           const catalog::ManifestEntry& m, float tsec) const;

    void runPaletteAction(const std::string& id) {
        auto& dash = model_->dashboard;
        if (id == "nav.home") dash.select(model::DashboardSection::Home);
        else if (id == "nav.sessions") dash.select(model::DashboardSection::Sessions);
        else if (id == "nav.manifests") {
            dash.select(model::DashboardSection::Manifests);
            dash.refreshManifests();
        } else if (id == "nav.help" || id == "nav.settings")
            dash.select(model::DashboardSection::Settings);
        else if (id == "nav.chat") model_->requestRoute(PendingRoute::Agent);
        else if (id == "act.refresh") {
            dash.refreshAll();
            dash.notice = "refreshed";
        } else if (id == "act.theme") {
            theme::toggle();
            persistUiPrefs(*model_);
        } else if (id == "act.shader") {
            gfx::cycleField(1);
            persistUiPrefs(*model_);
        } else if (id == "act.shader_off") {
            gfx::setFieldEnabled(false);
            persistUiPrefs(*model_);
        } else if (id == "act.shader_on") {
            gfx::setFieldEnabled(true);
            persistUiPrefs(*model_);
        } else if (id == "act.launch" || id == "act.wf_run") activate();
        else if (id == "act.wf_stop") {
            model_->pendingStopWorkflow = true;
            model_->workflowRun.requestCancel();
            dash.notice = "stopping workflow…";
        } else if (id == "act.wf_resume") {
            resumeLastWorkflow();
        } else if (id == "nav.wf_facet") {
            dash.select(model::DashboardSection::Manifests);
            dash.manifestFilter = "workflow";
            dash.refreshManifests();
            dash.notice = "facet · workflow";
        } else if (id == "act.wf_canvas") {
            if (workflowSelectionActive()) {
                dash.wfCanvasExpanded = !dash.wfCanvasExpanded;
                dash.wfCanvasFocus = true;
                dash.notice = dash.wfCanvasExpanded ? "infinite canvas" : "canvas docked";
            } else {
                dash.notice = "select a workflow first";
            }
        } else if (id == "sys.quit") model_->requestRoute(PendingRoute::Quit);
        bumpNotice();
    }
    std::string activeName() const {
        return !model_->agentName.empty() ? model_->agentName : nonempty(cfg_.agentName, "builtin");
    }
    std::string activeManifest() const {
        return !model_->activeManifestPath.empty() ? model_->activeManifestPath : cfg_.manifestPath;
    }

    void highlightActiveAgent() {
        const std::string am = activeManifest();
        const std::string an = activeName();
        if (am.empty() && an.empty()) return;
        for (int i = 0; i < static_cast<int>(model_->dashboard.manifests.size()); ++i) {
            const auto& m = model_->dashboard.manifests[static_cast<size_t>(i)];
            if (m.kind != "agent") continue;
            if ((!an.empty() && m.name == an) || (!am.empty() && m.path == am)) {
                model_->dashboard.manifestIndex = i;
                break;
            }
        }
    }

    void bumpNotice() {
        auto& dash = model_->dashboard;
        if (dash.searchMode) {
            dash.notice = "search: " + dash.searchQuery;
            return;
        }
        if (dash.section == model::DashboardSection::Manifests) {
            dash.notice = std::to_string(dash.manifests.size()) + " shown";
            if (!dash.manifestFilter.empty()) dash.notice += " · " + dash.manifestFilter;
            if (!dash.tagFilter.empty()) dash.notice += " · #" + dash.tagFilter;
            if (!dash.searchQuery.empty()) dash.notice += " · /" + dash.searchQuery;
        } else if (dash.section == model::DashboardSection::Sessions) {
            dash.notice = std::to_string(dash.sessions.size()) + " sessions";
        } else {
            dash.notice.clear();  // Home/Help — app bar pulse is enough
        }
    }

    static std::string suffix(const std::string& value) {
        if (value.empty()) return "none";
        return value.size() > 14 ? value.substr(value.size() - 14) : value;
    }
    static std::string basename(const std::string& path) {
        if (path.empty()) return "none";
        size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    void nudgeSetting(int dir) {
        auto& dash = model_->dashboard;
        switch (dash.settingsFocus) {
            case 0:  // theme
                theme::toggle();
                break;
            case 1:  // field on/off
                gfx::toggleFieldEnabled();
                break;
            case 2:  // shader carousel
                if (!gfx::fieldEnabled()) gfx::setFieldEnabled(true);
                gfx::cycleField(dir >= 0 ? 1 : -1);
                break;
            case 3:  // thoughts
                model_->showThoughts = !model_->showThoughts;
                model_->rebuildViews();
                break;
            case 4:  // truncate
                model_->truncateBodies = !model_->truncateBodies;
                model_->rebuildViews();
                break;
            case 5:  // raw
                model_->showRaw = !model_->showRaw;
                model_->rebuildViews();
                break;
            case 6:  // chat field bg
                // Vet-fix: independent from hub field on/off; chat-side underlay
                // persists via ui_prefs alongside theme + chat toggles.
                model_->chatFieldEnabled = !model_->chatFieldEnabled;
                dash.notice = model_->chatFieldEnabled
                                  ? std::string("chat · field bg on — ") +
                                        gfx::activeFieldName()
                                  : std::string("chat · field bg off (solid theme)");
                break;
            case 7:  // zen mode
                model_->zenMode = !model_->zenMode;
                dash.bumpNavActivity();  // show pill when enabling zen
                dash.notice = model_->zenMode
                                  ? "zen · pill auto-hides after idle nav"
                                  : "zen off · pill always up";
                break;
            case 8:  // nav pill master
                model_->navPillEnabled = !model_->navPillEnabled;
                if (model_->navPillEnabled) dash.bumpNavActivity();
                dash.notice = model_->navPillEnabled ? "nav pill ON" : "nav pill OFF";
                break;
            case 9: {  // pill hide delay carousel
                // 2s · 3s · 5s · 8s · never(0)
                static const int kDelays[] = {2000, 3000, 5000, 8000, 0};
                int idx = 1;  // default 3s
                for (int i = 0; i < 5; ++i)
                    if (kDelays[i] == model_->navPillHideMs) {
                        idx = i;
                        break;
                    }
                idx = (idx + (dir >= 0 ? 1 : 4)) % 5;
                model_->navPillHideMs = kDelays[idx];
                dash.bumpNavActivity();
                dash.notice = model_->navPillHideMs <= 0
                                  ? "pill hide · never"
                                  : ("pill hide · " +
                                     std::to_string(model_->navPillHideMs / 1000) + "s");
                break;
            }
            default:
                break;
        }
        persistUiPrefs(*model_);
        // Keep notice for toast paint this frame; clear on next action via hub_keys.
    }

    int dashFocus() const { return model_->dashboard.settingsFocus; }

    static std::string upperCopy(const std::string& s) {
        std::string o = s;
        for (char& c : o) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return o;
    }

    // Session/workflow ops — definitions in hub_session_ops.hpp
    bool workflowSelectionActive() const;
    void activate();
    void queueWorkflowRun(const catalog::ManifestEntry& m);
    void resumeLastWorkflow();
    void resumeSelectedSession();
    void createSession();
    void killLiveSession();
    void forkSelectedSession();
    void retitleSelectedSession();
    void exportSelectedSession();
    void deleteSelectedSession();

    void handle(const inkcell::Action& action) override {
        if (action.is("scroll.up")) model_->dashboard.moveNavigation(-1);
        else if (action.is("scroll.down")) model_->dashboard.moveNavigation(1);
    }
};

}  // namespace cortex::mk3::ui::scenes

#include "src/ui/scenes/hub_draw.hpp"
#include "src/ui/scenes/hub_session_ops.hpp"
#include "src/ui/scenes/hub_keys.hpp"
