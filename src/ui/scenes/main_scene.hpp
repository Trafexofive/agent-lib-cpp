#pragma once
// Dashboard hub — tight IA, operable facets, real agent launch.
// Pill: Home · Sessions · Manifests · Settings
// ctrl-j = prev · ctrl-k = next (inverted). Enter on agent = hot-swap launch.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "base_scene.hpp"
#include "src/session/controller.hpp"
#include "src/session/manager.hpp"
#include "src/ui/assets/glyphs.hpp"
#include "src/ui/chat/chat_view.hpp"
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
        loadUiPrefs();  // theme + shader from ~/.config/cortex-mk3/ui.json
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

    bool on_key(const inkcell::KeyEvent& event) override {
        using inkcell::KeyCode;
        auto& dash = model_->dashboard;

        // Palette has priority when open / closing input sink
        if (model_->cmdPalette.open && !model_->cmdPalette.closing) {
            std::string action;
            if (components::handleCmdPaletteKey(model_->cmdPalette, event, &action)) {
                if (!action.empty()) runPaletteAction(action);
                return true;
            }
        }

        // Ctrl-P opens palette
        if (event.code == KeyCode::Character && event.ctrl() &&
            (event.ch == 'p' || event.ch == 'P')) {
            model_->cmdPalette.toggle(components::hubCommands());
            return true;
        }

        // ── Search mode ──────────────────────────────────────────────
        if (dash.searchMode) {
            if (event.code == KeyCode::Escape) {
                dash.searchMode = false;
                dash.searchQuery.clear();
                dash.refreshManifests();
                bumpNotice();
                return true;
            }
            if (event.code == KeyCode::Enter) {
                dash.searchMode = false;
                bumpNotice();
                return true;
            }
            if (event.code == KeyCode::Backspace) {
                if (!dash.searchQuery.empty()) dash.searchQuery.pop_back();
                dash.refreshManifests();
                bumpNotice();
                return true;
            }
            if (event.code == KeyCode::Character && !event.ctrl() && event.ch >= 32) {
                dash.searchQuery.push_back(static_cast<char>(event.ch));
                dash.refreshManifests();
                bumpNotice();
                return true;
            }
            return true;
        }

        // Tab toggles dock vs content focus
        if (event.code == KeyCode::Tab) {
            if (dash.section == model::DashboardSection::Manifests && workflowSelectionActive()) {
                dash.wfCanvasFocus = !dash.wfCanvasFocus;
                dash.focus = model::DashboardFocus::Content;
                dash.notice = dash.wfCanvasFocus ? "canvas focus · hjkl pan · [] node"
                                                 : "list focus";
                return true;
            }
            dash.toggleFocus();
            return true;
        }

        // ── ctrl-j = prev · ctrl-k = next ────────────────────────────
        if (event.ctrl() && event.code == KeyCode::Character) {
            if (event.ch == 'j' || event.ch == 'J') {
                dash.moveNavigation(-1);
                bumpNotice();
                return true;
            }
            if (event.ch == 'k' || event.ch == 'K') {
                dash.moveNavigation(1);
                bumpNotice();
                return true;
            }
        }

        // When dock focused, plain j/k also cycle (no chord needed)
        if (dash.focus == model::DashboardFocus::Dock && !event.ctrl() &&
            event.code == KeyCode::Character) {
            if (event.ch == 'j' || event.ch == 'J') {
                dash.moveNavigation(-1);
                bumpNotice();
                return true;
            }
            if (event.ch == 'k' || event.ch == 'K') {
                dash.moveNavigation(1);
                bumpNotice();
                return true;
            }
        }

        if (event.code == KeyCode::Escape) {
            if (model_->workflowRun.isLive() || model_->workflowRun.isActive()) {
                model_->pendingStopWorkflow = true;
                model_->workflowRun.requestCancel();
                dash.notice = "stopping workflow…";
                return true;
            }
            if (dash.wfCanvasExpanded) {
                dash.wfCanvasExpanded = false;
                dash.notice = "canvas docked";
                return true;
            }
            if (dash.wfCanvasFocus) {
                dash.wfCanvasFocus = false;
                dash.notice = "list focus";
                return true;
            }
            if (dash.focus == model::DashboardFocus::Dock) {
                dash.focus = model::DashboardFocus::Content;
                return true;
            }
            if (!dash.searchQuery.empty()) {
                dash.searchQuery.clear();
                dash.refreshManifests();
                dash.notice = "search cleared";
                return true;
            }
            if (!dash.tagFilter.empty()) {
                dash.tagFilter.clear();
                dash.refreshManifests();
                dash.notice = "tag cleared";
                return true;
            }
            if (!dash.manifestFilter.empty()) {
                dash.manifestFilter.clear();
                dash.refreshManifests();
                dash.notice = "kind cleared";
                return true;
            }
            return true;
        }

        // Content list nav / settings option nav
        if (dash.focus == model::DashboardFocus::Content && !event.ctrl()) {
            const bool up = event.code == KeyCode::ArrowUp ||
                            (event.code == KeyCode::Character && (event.ch == 'k' || event.ch == 'K'));
            const bool down = event.code == KeyCode::ArrowDown ||
                              (event.code == KeyCode::Character && (event.ch == 'j' || event.ch == 'J'));
            const bool left = event.code == KeyCode::ArrowLeft ||
                              (event.code == KeyCode::Character && (event.ch == 'h' || event.ch == 'H'));
            const bool right = event.code == KeyCode::ArrowRight ||
                               (event.code == KeyCode::Character && (event.ch == 'l' || event.ch == 'L'));

            // Infinite canvas owns hjkl when focused (workflow selected).
            if (dash.section == model::DashboardSection::Manifests && dash.wfCanvasFocus &&
                workflowSelectionActive()) {
                const float pan = event.shift() ? 8.f : 3.f;
                if (left) {
                    dash.wfCamX -= pan;
                    return true;
                }
                if (right) {
                    dash.wfCamX += pan;
                    return true;
                }
                if (up) {
                    dash.wfCamY -= pan;
                    return true;
                }
                if (down) {
                    dash.wfCamY += pan;
                    return true;
                }
                if (event.code == KeyCode::Character && (event.ch == '[' || event.ch == 'p')) {
                    dash.wfFocusNode = std::max(0, dash.wfFocusNode - 1);
                    return true;
                }
                if (event.code == KeyCode::Character && (event.ch == ']' || event.ch == 'n')) {
                    ++dash.wfFocusNode;
                    return true;
                }
            }

            if (dash.section == model::DashboardSection::Settings) {
                if (up) {
                    dash.settingsFocus =
                        (dash.settingsFocus + model::DashboardState::settingsOptionCount - 1) %
                        model::DashboardState::settingsOptionCount;
                    return true;
                }
                if (down) {
                    dash.settingsFocus =
                        (dash.settingsFocus + 1) % model::DashboardState::settingsOptionCount;
                    return true;
                }
                if (left) {
                    nudgeSetting(-1);
                    return true;
                }
                if (right || event.code == KeyCode::Enter) {
                    nudgeSetting(+1);
                    return true;
                }
            } else {
                if (up) {
                    if (dash.section == model::DashboardSection::Sessions) dash.moveSession(-1);
                    else if (dash.section == model::DashboardSection::Manifests)
                        dash.moveManifest(-1);
                    return true;
                }
                if (down) {
                    if (dash.section == model::DashboardSection::Sessions) dash.moveSession(1);
                    else if (dash.section == model::DashboardSection::Manifests)
                        dash.moveManifest(1);
                    return true;
                }
            }
        }

        if (event.code == KeyCode::Enter && dash.section != model::DashboardSection::Settings) {
            activate();
            return true;
        }

        if (event.code == KeyCode::Character && !event.ctrl()) {
            // Leader-leader: space space → palette (not in search)
            if (event.ch == ' ') {
                if (model_->cmdPalette.noteSpace()) {
                    model_->cmdPalette.show(components::hubCommands());
                    return true;
                }
                // single space swallowed on hub (no type-ahead surface)
                return true;
            }
            model_->cmdPalette.clearLeader();

            // Settings hot-numbers: 0 field off · 1-7 pick shader (no on-screen list)
            if (dash.section == model::DashboardSection::Settings && event.ch >= '0' &&
                event.ch <= '9') {
                int d = event.ch - '0';
                if (d == 0) {
                    gfx::setFieldEnabled(false);
                    dash.settingsFocus = 1;
                } else if (d >= 1 && d <= gfx::fieldCount()) {
                    gfx::setFieldEnabled(true);
                    gfx::setFieldIndex(d - 1);
                    dash.settingsFocus = 2;
                }
                persistUiPrefs(*model_);
                return true;
            }
            // Kind digits on Manifests — operable facets
            if (dash.section == model::DashboardSection::Manifests && event.ch >= '0' &&
                event.ch <= '9') {
                if (dash.applyKindDigit(event.ch - '0')) {
                    bumpNotice();
                    return true;
                }
            }
            switch (event.ch) {
                case '/':
                    if (dash.section == model::DashboardSection::Manifests) {
                        dash.searchMode = true;
                        dash.focus = model::DashboardFocus::Content;
                        dash.notice = "search: ";
                        return true;
                    }
                    break;
                case 'c':
                case 'C':
                    // Chat route always — canvas center is '.' (no clash).
                    model_->requestRoute(PendingRoute::Agent);
                    return true;
                case '.':
                    if (dash.section == model::DashboardSection::Manifests &&
                        workflowSelectionActive()) {
                        dash.wfCamX = 1e9f;  // sentinel → reframe on next draw
                        dash.notice = "center";
                        return true;
                    }
                    break;
                case 'o':
                case 'O':
                case 'g':
                case 'G':
                    dash.select(model::DashboardSection::Home);
                    bumpNotice();
                    return true;
                case 's':
                case 'S':
                    // On Settings: S cycles shader. Elsewhere: Sessions jump.
                    if (dash.section == model::DashboardSection::Settings) {
                        dash.settingsFocus = 2;
                        if (!gfx::fieldEnabled()) gfx::setFieldEnabled(true);
                        else gfx::cycleField(1);
                        persistUiPrefs(*model_);
                        return true;
                    }
                    dash.select(model::DashboardSection::Sessions);
                    bumpNotice();
                    return true;
                case 'a':
                case 'A':
                case 'm':
                case 'M':
                    dash.select(model::DashboardSection::Manifests);
                    dash.refreshManifests();
                    bumpNotice();
                    return true;
                case 'f':
                case 'F':
                    if (dash.section == model::DashboardSection::Sessions) {
                        forkSelectedSession();
                        return true;
                    }
                    if (dash.section == model::DashboardSection::Manifests) {
                        if (event.ch == 'F' && workflowSelectionActive()) {
                            dash.wfCanvasExpanded = !dash.wfCanvasExpanded;
                            dash.wfCanvasFocus = dash.wfCanvasExpanded;
                            dash.notice = dash.wfCanvasExpanded ? "canvas expanded"
                                                               : "canvas docked";
                            return true;
                        }
                        dash.cycleManifestFilter();
                        bumpNotice();
                    }
                    return true;
                case 't':
                    if (dash.section == model::DashboardSection::Sessions) {
                        retitleSelectedSession();
                        return true;
                    }
                    if (dash.section == model::DashboardSection::Manifests) {
                        dash.cycleTagFilter();
                        bumpNotice();
                    }
                    return true;
                case '?':
                    dash.select(model::DashboardSection::Settings);
                    bumpNotice();
                    return true;
                case 'b':
                case 'B':  // toggle field on/off (global)
                    gfx::toggleFieldEnabled();
                    if (dash.section == model::DashboardSection::Settings) dash.settingsFocus = 1;
                    persistUiPrefs(*model_);
                    return true;
                case 'n':
                case 'N':
                    if (dash.section == model::DashboardSection::Sessions ||
                        dash.section == model::DashboardSection::Home)
                        createSession();
                    return true;
                case 'd':
                case 'D':
                    if (dash.section == model::DashboardSection::Sessions) deleteSelectedSession();
                    return true;
                case 'e':
                case 'E':
                    if (dash.section == model::DashboardSection::Sessions)
                        exportSelectedSession();
                    return true;
                case 'k':  // alias — lowercase k for quick kill on Sessions
                case 'K':
                    if (dash.section == model::DashboardSection::Sessions) killLiveSession();
                    return true;
                case 'y':
                case 'Y':
                    if (!dash.yankBuffer.empty()) {
                        dash.notice = "yank: " + dash.yankBuffer;
                    }
                    return true;
                case 'r':
                    if (dash.section == model::DashboardSection::Manifests) {
                        resumeLastWorkflow();
                        return true;
                    }
                    break;
                case 'x':
                    if (dash.section == model::DashboardSection::Sessions) {
                        // 'x' shortcut — kill live session, mirroring
                        // Ctrl-X semantics. Doesn't conflict with workflow
                        // cancel because we're on the Sessions page.
                        killLiveSession();
                        return true;
                    }
                    if (model_->workflowRun.isLive() || model_->workflowRun.isActive()) {
                        model_->pendingStopWorkflow = true;
                        model_->workflowRun.requestCancel();
                        dash.notice = "stopping workflow…";
                        return true;
                    }
                    break;
                case 'X':
                    if (dash.section == model::DashboardSection::Sessions) killLiveSession();
                    return true;
                    break;
                case 'z':
                case 'Z':
                    if (dash.section == model::DashboardSection::Manifests &&
                        workflowSelectionActive()) {
                        dash.wfCanvasExpanded = !dash.wfCanvasExpanded;
                        dash.wfCanvasFocus = true;
                        dash.notice =
                            dash.wfCanvasExpanded ? "infinite canvas" : "canvas docked";
                        return true;
                    }
                    break;
                case 'R':
                    dash.refreshAll();
                    dash.notice = "refreshed";
                    return true;
                case 'T':
                    theme::toggle();
                    if (dash.section == model::DashboardSection::Settings) dash.settingsFocus = 0;
                    persistUiPrefs(*model_);
                    return true;
                case 'q':
                case 'Q':
                    model_->requestRoute(PendingRoute::Quit);
                    return true;
            }
        }
        return false;
    }

    void draw(inkcell::Surface& surface) const override {
        if (layout::render_min_size_notice(surface, 80, 20)) return;
        surface.clear(theme::base_bg());

        const auto full = surface.bounds();
        // Horizontal breathing only — bottom is owned by the 3-row textured pill.
        inkcell::Rect page{full.x + 2, full.y + 1, std::max(1, full.w - 4),
                           std::max(1, full.h - 1)};

        const auto tier = layout::densityOf(page.w);
        const auto& dash = model_->dashboard;
        const float tsec = gfx::nowSeconds();
        const int tvar = gfx::themeVariantIndex();

        gfx::drawFieldBg(surface, full, tvar, tsec);
        drawAppBar(surface, page, tier);

        // Textured 3-row pill (restored). bottomY = last body row.
        // Shadow paints one row under body when available — pin body so
        // shadow lands on full.bottom()-1 (no black dead strip).
        const int pillBodyH = 3;
        const int airAbovePill = 1;
        const int pillBottomY = full.bottom() - 2;  // body ends here; shadow on last row
        int stageTop = page.y + 3;
        int stageBot = pillBottomY - (pillBodyH - 1) - airAbovePill;
        int stageH = std::max(6, stageBot - stageTop);
        inkcell::Rect stage{page.x, stageTop, page.w, stageH};
        gfx::drawBorderlessPanel(surface, stage, theme::panel_bg());

        // Content has real padding — not cramped
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

        static const std::vector<components::PillItem> pills = {
            {"g", "Home"},
            {"s", "Sessions"},
            {"a", "Manifests"},
            {"?", "Settings"},
        };
        components::drawPillDock(surface, page.x, page.w, pillBottomY, pills,
                                 dash.navigationIndex, dash.navPrevIndex, dash.navAnimT(),
                                 dash.focus == model::DashboardFocus::Dock);

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
                persistUiPrefs(*model_);
                dash.notice.clear();
                break;
            default:
                break;
        }
        persistUiPrefs(*model_);
        dash.notice.clear();
    }

    int dashFocus() const { return model_->dashboard.settingsFocus; }

    static std::string upperCopy(const std::string& s) {
        std::string o = s;
        for (char& c : o) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return o;
    }

    void activate() {
        auto& dash = model_->dashboard;
        if (dash.section == model::DashboardSection::Sessions) {
            resumeSelectedSession();
            return;
        }
        if (dash.section == model::DashboardSection::Home) {
            model_->requestRoute(PendingRoute::Agent);
            return;
        }
        if (dash.section == model::DashboardSection::Manifests) {
            const auto* m = dash.selectedManifest();
            if (!m) {
                dash.notice = "no selection";
                return;
            }
            if (m->kind == "agent" && m->launchable) {
                // Real launch — REPL tick hot-swaps Agent then opens chat.
                dash.yankBuffer = "cortex-mk3 -m " + m->path + " --tui experimental";
                model_->pendingLaunchManifest = m->path;
                dash.notice = "launching " + m->name + "…";
                model_->launchError.clear();
                return;
            }
            if (m->kind == "workflow") {
                queueWorkflowRun(*m);
                return;
            }
            dash.notice = m->kind + " · " + m->category + " · inspect only";
            return;
        }
    }

    bool workflowSelectionActive() const {
        const auto* m = model_->dashboard.selectedManifest();
        return m && m->kind == "workflow" &&
               model::workflowRunnablePath(m->path, m->name);
    }

    // Infinite canvas stage: header + graph void + optional live strip.
    void queueWorkflowRun(const catalog::ManifestEntry& m) {
        auto& dash = model_->dashboard;
        if (!model::workflowRunnablePath(m.path, m.name)) {
            dash.notice = "workflow spec · not runnable";
            return;
        }
        if (model_->workflowRun.isLive()) {
            dash.notice = "workflow already running · Esc/x stop";
            return;
        }
        dash.yankBuffer = "workflow run " + m.path;
        model_->pendingRunWorkflow = m.path;
        dash.wfCanvasFocus = true;
        dash.notice = "running " + m.name + "…";
    }

    void resumeLastWorkflow() {
        auto& dash = model_->dashboard;
        auto snap = model_->workflowRun.snapshot();
        std::string path = snap.path;
        if (path.empty()) {
            const auto* m = dash.selectedManifest();
            if (m && m->kind == "workflow") path = m->path;
        }
        if (path.empty()) {
            dash.notice = "no workflow to resume";
            return;
        }
        if (model_->workflowRun.isLive()) {
            dash.notice = "already running";
            return;
        }
        model_->pendingRunWorkflow = path;
        dash.wfCanvasFocus = true;
        dash.notice = "re-running " + (snap.name.empty() ? path : snap.name) + "…";
    }

    void resumeSelectedSession() {
        if (!model_->rootAgent) {
            model_->dashboard.notice = "agent runtime unavailable";
            return;
        }
        session::SessionManager sessions;
        auto result = model::resumeDashboardSession(
            model_->dashboard, sessions,
            [&](const std::string& id) { model_->rootAgent->loadSession(id); });
        if (!result.ok) return;
        // loadSession may have repaired records:[] from the state
        // checkpoint into agent.history_ + re-saved the session file.
        // Prefer re-reading the session so the chat UI gets the same
        // rows the agent will use for the next turn.
        std::vector<SessionRecord> records = std::move(result.records);
        if (records.empty() && sessions.exists(result.sessionId)) {
            try {
                records = sessions.load(result.sessionId).records;
            } catch (...) {
            }
        }
        // Prefer full session (ui_timeline) so resume matches live chat.
        Session full;
        try {
            if (sessions.exists(result.sessionId))
                full = sessions.load(result.sessionId);
        } catch (...) {
        }
        if (full.records.empty() && !records.empty()) full.records = records;
        if (full.records.empty() && model_->rootAgent) {
            for (const auto& h : model_->rootAgent->history()) {
                SessionRecord rec;
                if (h.rfind("User: ", 0) == 0) {
                    rec.role = SessionRecord::USER;
                    rec.content = h.substr(6);
                } else if (h.rfind("Agent: ", 0) == 0) {
                    rec.role = SessionRecord::AGENT;
                    rec.content = h.substr(7);
                } else if (h.rfind("System: ", 0) == 0) {
                    rec.role = SessionRecord::SYSTEM;
                    rec.content = h.substr(8);
                } else {
                    continue;
                }
                full.records.push_back(std::move(rec));
            }
        }
        model_->loadSessionUi(full);
        model_->activeSessionId = result.sessionId;
        model_->requestRoute(PendingRoute::Agent);
    }

    void createSession() {
        if (!model_->rootAgent) {
            model_->dashboard.notice = "agent runtime unavailable";
            return;
        }
        session::SessionManager sessions;
        auto result = model::createDashboardSession(
            model_->dashboard, sessions, model_->rootAgent->name(),
            nonempty(model_->agentModel, cfg_.model),
            nonempty(model_->agentProvider, cfg_.provider),
            [&] { model_->rootAgent->clearHistory(); });
        if (!result.ok) return;
        model_->clearTranscript();
        model_->activeSessionId = result.sessionId;
        model_->requestRoute(PendingRoute::Agent);
    }

    void killLiveSession() {
        // Operator-locked requirement: a way to end a live session in-place
        // from the Sessions page (not just refuse-to-delete). We:
        //   1) flip g_running so the worker exits its current iteration;
        //   2) explicitly tell the model the live run is cancelled;
        //   3) drop the persistence file (which the Sessions list can now
        //      re-read on next refresh — it's truly gone);
        //   4) clear the in-memory transcript so a stale chat footer
        //      doesn't haunt the operator.
        const auto* sel = model_->dashboard.selectedSession();
        if (!sel) {
            model_->dashboard.notice = "no session selected";
            return;
        }
        std::string id = sel->id;
        if (model_->activeSessionId.empty()) {
            // Selected the row but nothing live → just delete it.
            deleteSelectedSession();
            return;
        }
        if (model_->activeSessionId != id) {
            model_->dashboard.notice =
                "live is " + suffix(model_->activeSessionId) + " — select it to kill";
            return;
        }
        // 1. signal worker to stop. Next iteration check will exit before
        // a new prompt() round, and the bridge publishes TurnDone.
        g_running = false;
        // 2. clear live flags synchronously so the hub reflects the state
        // immediately (the worker may take a moment to publish TurnDone).
        model_->running = false;
        model_->status = "stopped";
        try {
            // Drain any coalesced ui_timeline write BEFORE remove — otherwise
            // AsyncUiTimelineWriter can resurrect the file after delete.
            session::AsyncUiTimelineWriter::instance().flush();
            session::SessionManager sessions;
            sessions.remove(id);
            // 3. clear in-memory session record of the dead session.
            model_->activeSessionId.clear();
            session::activeSession().clear();
            model_->pendingSubmit.clear();
            if (model_->rootAgent) model_->rootAgent->clearHistory();
            model_->clearTranscript();
            model_->dashboard.refreshSessions(sessions);
            model_->dashboard.notice = "killed live session " + suffix(id);
        } catch (const std::exception& e) {
            model_->dashboard.notice = std::string("kill failed: ") + e.what();
        }
    }

    void forkSelectedSession() {
        const auto* sel = model_->dashboard.selectedSession();
        if (!sel) {
            model_->dashboard.notice = "no session selected";
            return;
        }
        try {
            session::SessionManager sessions;
            if (!sessions.exists(sel->id)) {
                model_->dashboard.notice = "session missing on disk";
                return;
            }
            std::string newId = session::mintSessionId();
            std::string title = sel->title.empty() ? ("fork of " + suffix(sel->id))
                                                  : (sel->title + " (fork)");
            Session fork = session::forkSession(sessions, sel->id, newId, title);
            model_->dashboard.refreshSessions(sessions);
            // Select the new fork.
            for (int i = 0; i < static_cast<int>(model_->dashboard.sessions.size()); ++i) {
                if (model_->dashboard.sessions[static_cast<size_t>(i)].id == fork.id) {
                    model_->dashboard.sessionIndex = i;
                    break;
                }
            }
            model_->dashboard.notice =
                "forked " + suffix(sel->id) + " → " + suffix(fork.id) +
                (fork.uiTimelineJson.empty() ? " (records only)" : " (ui timeline)");
        } catch (const std::exception& e) {
            model_->dashboard.notice = std::string("fork failed: ") + e.what();
        }
    }

    void retitleSelectedSession() {
        const auto* sel = model_->dashboard.selectedSession();
        if (!sel) {
            model_->dashboard.notice = "no session selected";
            return;
        }
        try {
            session::SessionManager sessions;
            Session s = sessions.load(sel->id);
            if (s.id.empty()) {
                model_->dashboard.notice = "session missing on disk";
                return;
            }
            // Prefer first user record as title; if already titled, clear (toggle).
            std::string next;
            if (sel->title.empty() || sel->title == suffix(sel->id)) {
                for (const auto& rec : s.records) {
                    if (rec.role == SessionRecord::USER && !rec.content.empty()) {
                        next = rec.content;
                        auto nl = next.find('\n');
                        if (nl != std::string::npos) next = next.substr(0, nl);
                        if (next.size() > 48) next = next.substr(0, 45) + "...";
                        break;
                    }
                }
                if (next.empty()) next = "session " + suffix(sel->id);
            } else {
                next.clear();  // clear custom title → fall back to prompt/id
            }
            if (!session::setSessionTitle(sessions, sel->id, next)) {
                model_->dashboard.notice = "title update failed";
                return;
            }
            model_->dashboard.refreshSessions(sessions);
            model_->dashboard.notice =
                next.empty() ? ("cleared title on " + suffix(sel->id))
                             : ("titled " + suffix(sel->id) + " → " + next);
        } catch (const std::exception& e) {
            model_->dashboard.notice = std::string("title failed: ") + e.what();
        }
    }

    void exportSelectedSession() {
        // Vet-fix smarter export: portable .json via SessionManager::exportToFile
        // rather than dumping raw. Path is well-known: /tmp/mk3-session-<id>.json.
        // Falls back to a notice if anything stalls so the operator can recover.
        const auto* sel = model_->dashboard.selectedSession();
        if (!sel) {
            model_->dashboard.notice = "no session selected";
            return;
        }
        try {
            session::SessionManager sessions;
            std::string path = "/tmp/mk3-session-" + sel->id + ".json";
            if (sessions.exportToFile(sel->id, path)) {
                model_->dashboard.notice = "exported → " + path;
            } else {
                model_->dashboard.notice = "export failed (empty/loadable?): " + sel->id;
            }
        } catch (const std::exception& e) {
            model_->dashboard.notice = std::string("export error: ") + e.what();
        }
    }

    void deleteSelectedSession() {
        const auto* sel = model_->dashboard.selectedSession();
        if (!sel) {
            model_->dashboard.notice = "no session selected";
            return;
        }
        std::string id = sel->id;
        // Vet-fix: refuse to delete the active session — that would
        // silently strand the live agent. Operator must end/clear first.
        if (model_->activeSessionId == id) {
            model_->dashboard.notice =
                "active session — /clear or resume another first (esc to clear)";
            return;
        }
        try {
            session::SessionManager sessions;
            sessions.remove(id);
            // Clamp selector if we just shrank the list.
            model_->dashboard.sessionIndex =
                std::max(0, std::min(model_->dashboard.sessionIndex,
                                     static_cast<int>(model_->dashboard.sessions.size()) - 1));
            model_->dashboard.refreshSessions(sessions);
            model_->dashboard.notice = "deleted " + id;
        } catch (const std::exception& e) {
            model_->dashboard.notice = std::string("delete failed: ") + e.what();
        }
    }

    void handle(const inkcell::Action& action) override {
        if (action.is("scroll.up")) model_->dashboard.moveNavigation(-1);
        else if (action.is("scroll.down")) model_->dashboard.moveNavigation(1);
    }
};

}  // namespace cortex::mk3::ui::scenes

#include "src/ui/scenes/hub_draw.hpp"
