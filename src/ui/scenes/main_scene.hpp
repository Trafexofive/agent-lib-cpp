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
                    model_->pendingRoute = "agent";
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
                    model_->pendingRoute = "quit";
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
    void runPaletteAction(const std::string& id) {
        auto& dash = model_->dashboard;
        if (id == "nav.home") dash.select(model::DashboardSection::Home);
        else if (id == "nav.sessions") dash.select(model::DashboardSection::Sessions);
        else if (id == "nav.manifests") {
            dash.select(model::DashboardSection::Manifests);
            dash.refreshManifests();
        } else if (id == "nav.help" || id == "nav.settings")
            dash.select(model::DashboardSection::Settings);
        else if (id == "nav.chat") model_->pendingRoute = "agent";
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
        } else if (id == "sys.quit") model_->pendingRoute = "quit";
        bumpNotice();
    }
    std::string activeName() const {
        return !model_->agentName.empty() ? model_->agentName : nonempty(cfg_.agentName, "builtin");
    }
    std::string activeManifest() const {
        return !model_->activeManifestPath.empty() ? model_->activeManifestPath : cfg_.manifestPath;
    }

    void drawAppBar(inkcell::Surface& surface, inkcell::Rect page, layout::DensityTier tier) const {
        auto bar = theme::panel_2();
        // Two rows only — no hairline under bar; wallpaper/ripple owns the gap.
        surface.fill({page.x, page.y, page.w, 2}, " ", bar);

        // Brand: CORTEX bold + MK3 cyan — contiguous "CORTEX MK3" for scan/tests
        auto brand = theme::bright().with_bg(bar.bg);
        auto mk = theme::cyan().with_bg(bar.bg);
        surface.text({page.x, page.y}, "CORTEX ", brand);
        surface.text({page.x + 7, page.y}, "MK3", mk);
        auto sec = theme::italic_accent().with_bg(bar.bg);
        surface.text({page.x + 10, page.y},
                     "  ·  " + std::string(model::dashboardSectionName(model_->dashboard.section)),
                     sec);

        std::string right = activeName() + "  ·  " + nonempty(cfg_.provider, model_->agentProvider) +
                            "/" + nonempty(cfg_.model, model_->agentModel) + "  ·  " +
                            layout::densityName(tier);
        // Prefer live identity
        right = activeName() + "  ·  " + nonempty(model_->agentProvider, nonempty(cfg_.provider, "?")) +
                "/" + nonempty(model_->agentModel, nonempty(cfg_.model, "?")) + "  ·  " +
                layout::densityName(tier);
        int rw = inkcell::text::display_width(right);
        surface.text({std::max(page.x, page.right() - rw), page.y}, right,
                     theme::dim().with_bg(bar.bg));

        std::string sub;
        if (!model_->launchError.empty()) {
            sub = "error  " + model_->launchError;
        } else if (model_->dashboard.searchMode || !model_->dashboard.searchQuery.empty()) {
            sub = "/" + model_->dashboard.searchQuery +
                  (model_->dashboard.searchMode ? "█" : "") + "   esc clears";
        } else if (!model_->dashboard.notice.empty()) {
            sub = model_->dashboard.notice;
        } else {
            sub = "session " + suffix(model_->activeSessionId) + "   registry " +
                  std::to_string(model_->dashboard.manifests.size()) + "   " +
                  (model_->running ? "● live" : "○ idle");
        }
        surface.text({page.x, page.y + 1}, inkcell::text::truncate(sub, page.w),
                     (!model_->launchError.empty() ? theme::red() : theme::dim()).with_bg(bar.bg));
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

    void drawContent(inkcell::Surface& surface, inkcell::Rect frame) const {
        switch (model_->dashboard.section) {
            case model::DashboardSection::Home: drawHome(surface, frame); break;
            case model::DashboardSection::Sessions: drawSessions(surface, frame); break;
            case model::DashboardSection::Manifests: drawManifests(surface, frame); break;
            case model::DashboardSection::Settings: drawSettings(surface, frame); break;
        }
    }

    void sectionHead(inkcell::Surface& surface, inkcell::Rect frame, const std::string& title,
                     const std::string& subtitle) const {
        // No ─ rules — title + italic subtitle only; field/theme is the chrome.
        surface.text({frame.x, frame.y}, title, theme::bright());
        if (!subtitle.empty())
            surface.text({frame.x, frame.y + 1}, inkcell::text::truncate(subtitle, frame.w),
                         theme::italic_dim());
    }

    void metricTile(inkcell::Surface& surface, inkcell::Rect r, const std::string& label,
                    const std::string& value, inkcell::Style valueSt) const {
        // Borderless tile — no ─ box chrome
        surface.fill(r, " ", theme::panel_2());
        surface.text({r.x + 2, r.y + 1}, inkcell::text::truncate(label, r.w - 4), theme::dim());
        surface.text({r.x + 2, r.y + 2}, inkcell::text::truncate(value, r.w - 4), valueSt);
    }

    void drawHome(inkcell::Surface& surface, inkcell::Rect frame) const {
        // ── Hero identity ────────────────────────────────────────────
        const std::string name = activeName();
        const std::string engine =
            nonempty(model_->agentProvider, nonempty(cfg_.provider, "?")) + "/" +
            nonempty(model_->agentModel, nonempty(cfg_.model, "?"));
        const bool live = model_->running;
        const char* pulse = live ? "● LIVE" : "○ READY";

        surface.text({frame.x, frame.y}, inkcell::text::truncate(name, frame.w / 2), theme::bright());
        surface.text({frame.x + inkcell::text::display_width(name) + 2, frame.y},
                     pulse, live ? theme::green() : theme::muted());
        if (frame.h > 1)
            surface.text({frame.x, frame.y + 1},
                         inkcell::text::truncate(engine, frame.w), theme::italic_dim());

        int y = frame.y + 3;

        // ── KPI strip (real counts only) ─────────────────────────────
        int agents = 0, tools = 0, feeds = 0, other = 0;
        for (const auto& m : model_->dashboard.manifests) {
            if (m.kind == "agent") ++agents;
            else if (m.kind == "tool") ++tools;
            else if (m.kind == "feed") ++feeds;
            else ++other;
        }
        int sessN = static_cast<int>(model_->dashboard.sessions.size());
        int toolN = model_->rootAgent ? static_cast<int>(model_->rootAgent->toolNames().size()) : 0;
        int subN = model_->rootAgent ? static_cast<int>(model_->rootAgent->subAgentNames().size()) : 0;

        struct Kpi { const char* lab; std::string val; inkcell::Style st; };
        std::vector<Kpi> kpis = {
            {"REGISTRY", std::to_string(static_cast<int>(model_->dashboard.manifests.size())), theme::cyan()},
            {"AGENTS", std::to_string(agents), theme::bright()},
            {"SESSIONS", std::to_string(sessN), theme::text()},
            {"TOOLS", std::to_string(toolN), theme::text()},
            {"SUBS", std::to_string(subN), theme::muted()},
        };
        int cols = std::min(static_cast<int>(kpis.size()), std::max(3, frame.w / 18));
        int gap = 1;
        int tileW = std::max(12, (frame.w - gap * (cols - 1)) / cols);
        int tileH = 3;
        if (y + tileH < frame.bottom()) {
            for (int i = 0; i < cols; ++i) {
                inkcell::Rect t{frame.x + i * (tileW + gap), y, tileW, tileH};
                surface.fill(t, " ", theme::panel_2());
                // top accent freckle
                surface.text({t.x + 1, t.y}, "▀",
                             (i == 0 ? theme::cyan() : theme::dim()).with_bg(theme::panel_2().bg));
                surface.text({t.x + 2, t.y}, inkcell::text::truncate(kpis[static_cast<size_t>(i)].lab, tileW - 3),
                             theme::dim().with_bg(theme::panel_2().bg));
                auto vs = kpis[static_cast<size_t>(i)].st;
                vs.bg = theme::panel_2().bg;
                vs.bold = true;
                surface.text({t.x + 2, t.y + 1},
                             inkcell::text::truncate(kpis[static_cast<size_t>(i)].val, tileW - 3), vs);
            }
            y += tileH + 2;
        }

        // ── Two-column: runtime | loadout ────────────────────────────
        int colW = frame.w >= 70 ? (frame.w - 3) / 2 : frame.w;
        int leftX = frame.x;
        int rightX = frame.w >= 70 ? frame.x + colW + 3 : frame.x;
        int yL = y, yR = y;

        auto head = [&](int x, int& yy, const char* t, inkcell::Style st) {
            if (yy >= frame.bottom()) return;
            surface.text({x, yy++}, t, st);
        };
        auto row = [&](int x, int& yy, int w, const char* k, const std::string& v) {
            if (yy >= frame.bottom()) return;
            components::fieldLine(surface, x, yy++, w, k, v);
        };

        head(leftX, yL, "RUNTIME", theme::cyan_soft());
        row(leftX, yL, colW, "manifest",
            activeManifest().empty() ? "—" : basename(activeManifest()));
        row(leftX, yL, colW, "session",
            model_->activeSessionId.empty() ? "—" : suffix(model_->activeSessionId));
        row(leftX, yL, colW, "harness", basename(cfg_.harnessPath));
        row(leftX, yL, colW, "system", basename(cfg_.systemPromptPath));
        row(leftX, yL, colW, "persona", basename(cfg_.personaPath));
        row(leftX, yL, colW, "turn", live ? "running" : "idle");

        if (frame.w >= 70) {
            head(rightX, yR, "LOADOUT", theme::amber_soft());
            auto joinN = [&](const std::vector<std::string>& names, int maxN) {
                std::string j;
                int n = 0;
                for (const auto& nm : names) {
                    if (n >= maxN) {
                        j += " · +";
                        j += std::to_string(static_cast<int>(names.size()) - maxN);
                        break;
                    }
                    if (!j.empty()) j += " · ";
                    j += nm;
                    ++n;
                }
                return j.empty() ? std::string("—") : j;
            };
            if (model_->rootAgent) {
                row(rightX, yR, colW, "tools", joinN(model_->rootAgent->toolNames(), 6));
                row(rightX, yR, colW, "agents", joinN(model_->rootAgent->subAgentNames(), 5));
                row(rightX, yR, colW, "feeds", joinN(model_->rootAgent->feedNames(), 4));
            } else {
                row(rightX, yR, colW, "tools", "—");
                row(rightX, yR, colW, "agents", "—");
                row(rightX, yR, colW, "feeds", "—");
            }
            row(rightX, yR, colW, "registry", std::to_string(agents) + "a · " +
                                                  std::to_string(tools) + "t · " +
                                                  std::to_string(feeds) + "f");
            row(rightX, yR, colW, "field",
                gfx::fieldEnabled() ? std::string(gfx::activeFieldName()) : std::string("off"));
        }

        y = std::max(yL, yR) + 1;

        // ── Recent sessions (actionable) ─────────────────────────────
        if (y + 2 < frame.bottom() && !model_->dashboard.sessions.empty()) {
            surface.text({frame.x, y++}, "RECENT", theme::violet());
            int shown = 0;
            for (const auto& s : model_->dashboard.sessions) {
                if (shown >= 4 || y >= frame.bottom() - 1) break;
                bool cur = !model_->activeSessionId.empty() && s.id == model_->activeSessionId;
                std::string line = std::string(cur ? "▸ " : "  ") +
                                   (s.agentName.empty() ? s.id : s.agentName);
                line += "  ·  " + std::to_string(s.turnCount) + "t";
                if (!s.updated.empty()) line += "  ·  " + s.updated;
                surface.text({frame.x, y++}, inkcell::text::truncate(line, frame.w),
                             cur ? theme::bright() : theme::muted());
                ++shown;
            }
        }

        if (!model_->launchError.empty() && y < frame.bottom()) {
            surface.text({frame.x, y},
                         inkcell::text::truncate("⚠  " + model_->launchError, frame.w),
                         theme::red());
        }

        // Footer action — one line, no key encyclopedia
        if (frame.bottom() - 1 > y)
            surface.text({frame.x, frame.bottom() - 1},
                         "enter open chat  ·  a registry  ·  s sessions",
                         theme::italic_dim());
    }

    void drawSessions(inkcell::Surface& surface, inkcell::Rect frame) const {
        // Vet-fix smarter sessions page: LIVE chip on the active session,
        // column-rich row, active-delete guard refused via notice (not a
        // hint-only silent failure), empty-state with shortcut, scoped
        // header KPI. Same AAA grammar as Home.
        const auto& dash = model_->dashboard;

        // Hero + KPI label
        surface.text({frame.x, frame.y}, "SESSIONS", theme::bright());
        const int totalSess = static_cast<int>(dash.sessions.size());
        const int activeFlag = !model_->activeSessionId.empty() ? 1 : 0;
        std::string stat =
            " " + std::to_string(totalSess) +
            " on disk" + (activeFlag ? "  ·  1 active" : "");
        surface.text({frame.x + 9, frame.y}, inkcell::text::truncate(stat, frame.w - 9),
                     theme::italic_dim());
        if (frame.h > 1) {
            int liveTurn = model_->running ? 1 : 0;
            std::string meta = std::string("enter resume · n new · d delete · e export · x kill-live · / search") +
                                (liveTurn ? std::string("  ·  ● live") : std::string("  ·  ○ idle"));
            surface.text({frame.x, frame.y + 1},
                         inkcell::text::truncate(meta, frame.w), theme::italic_dim());
        }

        int y = frame.y + 3;
        if (dash.sessions.empty()) {
            inkcell::Style tip = theme::muted();
            surface.text({frame.x, y++}, "Nothing yet.", tip);
            surface.text({frame.x, y++}, "Press  n  to create and open chat.", theme::text());
            surface.text({frame.x, y}, "Press  e  on a later session to export as .json.", theme::dim());
            return;
        }

        // Visible column headers
        if (frame.w >= 64) {
            int rowW = frame.w;
            int idsX = frame.x;
            int liveX = frame.x + 4;
            int agentX = frame.x + 12;
            int turnsX = std::max(agentX + 18, frame.x + rowW - 28);
            int updatedX = std::max(turnsX + 6, frame.x + rowW - 14);
            auto head = theme::dim();
            surface.text({idsX, y}, "ID", head);
            surface.text({liveX, y}, "STATE", head);
            surface.text({agentX, y}, "AGENT", head);
            surface.text({turnsX, y}, "TURNS", head);
            surface.text({updatedX, y}, "UPDATED", head);
            ++y;
        }

        int visible = std::max(1, frame.bottom() - y - 1);  // reserve row for footer
        int start = std::max(0, std::min(dash.sessionIndex - visible / 2,
                                         static_cast<int>(dash.sessions.size()) - visible));

        for (int i = start; i < static_cast<int>(dash.sessions.size()) && y < frame.bottom(); ++i) {
            const auto& s = dash.sessions[static_cast<size_t>(i)];
            bool selected = i == dash.sessionIndex;
            bool active = !model_->activeSessionId.empty() &&
                          s.id == model_->activeSessionId;

            auto rowBg = selected ? theme::panel_3()
                          : active   ? theme::panel_2()
                                     : theme::panel_bg();
            surface.fill({frame.x, y, frame.w, 1}, " ", rowBg);
            if (selected)
                surface.text({frame.x, y}, "▌", theme::cyan().with_bg(rowBg.bg));

            // Primary label: title (first prompt / rename) else id suffix.
            std::string idTxt = suffix(s.id);
            std::string label = !s.title.empty() ? s.title : idTxt;
            int labelW = std::max(2, inkcell::text::display_width(label));
            auto idSt = active ? theme::bright() : theme::muted();
            idSt.bg = rowBg.bg;
            surface.text({frame.x + 1, y},
                         inkcell::text::truncate(label, std::max(2, frame.w - 3)),
                         active ? idSt.strong() : idSt);

            if (frame.w >= 64) {
                // STATE + resume quality badge
                auto state = active ? theme::green() : theme::dim();
                state.bg = rowBg.bg;
                state.bold = active;
                std::string badge = active ? "● LIVE" : (s.hasUiTimeline ? "◉ UI" : "◯");
                surface.text({frame.x + labelW + 2, y}, badge, state);

                // AGENT column
                std::string agentTxt = nonempty(s.agentName, "?");
                surface.text({frame.x + labelW + 10, y},
                             inkcell::text::truncate(agentTxt, std::max(8, frame.w - labelW - 32)),
                             (theme::text()).with_bg(rowBg.bg));

                // TURNS
                auto turnSt = theme::text().with_bg(rowBg.bg);
                turnSt.bold = (s.turnCount > 0);
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%zut",
                              static_cast<size_t>(s.turnCount));
                surface.text({frame.x + frame.w - 22, y},
                             inkcell::text::truncate(buf, 6), turnSt);

                // UPDATED — age-style short suffix (last 4 of iso or -)
                std::string whenTxt = s.updated.empty()
                                          ? std::string("—")
                                          : s.updated.substr(std::max<size_t>(0, s.updated.size() - 16));
                whenTxt += "Z";
                auto updSt = theme::italic_dim();
                updSt.bg = rowBg.bg;
                surface.text({frame.x + frame.w - 14, y},
                             inkcell::text::truncate(whenTxt, 14), updSt);
            } else {
                std::string oneLine = active ? "● " : (s.hasUiTimeline ? "◉ " : "  ");
                oneLine += label + "  " + nonempty(s.agentName, "?") + "  " +
                           std::to_string(s.turnCount) + "r  ";
                if (!s.updated.empty()) oneLine += s.updated;
                auto st = ((selected ? theme::bright() : theme::text())).with_bg(rowBg.bg);
                if (active) st.bold = true;
                surface.text({frame.x + 1, y},
                             inkcell::text::truncate(oneLine, frame.w - 2), st);
            }
            ++y;
        }

        // Footer row: error context message + J/K hint
        if (y < frame.bottom()) {
            std::string hint =
                "j/k · ↵ resume · n new · f fork · t title · d del · e export · x kill";
            surface.text({frame.x, y}, inkcell::text::truncate(hint, frame.w),
                         theme::italic_dim());
        }
    }

    void drawManifests(inkcell::Surface& surface, inkcell::Rect frame) const {
        const auto& dash = model_->dashboard;
        auto L = layout::manifestLayoutFor(frame.w);

        sectionHead(surface, frame, "Manifests",
                    "↵ run/launch · 1-9 kind · f facet · z canvas · tab focus · / search");

        int y = frame.y + 4;

        // Expanded infinite canvas owns the whole stage.
        if (dash.wfCanvasExpanded) {
            const auto* sel = dash.selectedManifest();
            if (sel && sel->kind == "workflow") {
                drawWorkflowStage(surface, {frame.x, y, frame.w, frame.bottom() - y}, *sel,
                                  gfx::nowSeconds());
                return;
            }
        }

        // Operable kind chips with indices: [1 agent 12] ...
        std::map<std::string, int> kindCounts;
        // Count from unfiltered would need cache; approximate from current view + labels
        auto allForCount = catalog::discoverManifests(dash.manifestDir);
        for (const auto& m : allForCount) kindCounts[m.kind]++;

        std::vector<components::Chip> kindChips;
        kindChips.push_back({"0", "0:all", static_cast<int>(allForCount.size()),
                             dash.manifestFilter.empty()});
        const auto& facets = model::DashboardState::kindFacets();
        for (size_t i = 1; i < facets.size() && i <= 9; ++i) {
            const std::string& k = facets[i];
            int c = kindCounts.count(k) ? kindCounts[k] : 0;
            kindChips.push_back({std::to_string(i), std::to_string(i) + ":" + k, c,
                                 dash.manifestFilter == k});
        }
        int after = y;
        components::drawChipStrip(surface, {frame.x, y, frame.w, 2}, kindChips, &after);
        y = after;

        // Category strip (secondary) — only when useful
        std::map<std::string, int> catCounts;
        for (const auto& m : dash.manifests) catCounts[m.category]++;
        if (catCounts.size() > 1) {
            std::vector<components::Chip> catChips;
            for (const auto& kv : catCounts)
                catChips.push_back({kv.first, kv.first, kv.second, dash.tagFilter == kv.first});
            components::drawChipStrip(surface, {frame.x, y, frame.w, 2}, catChips, &after);
            y = after + 1;
        } else {
            ++y;
        }

        if (dash.manifests.empty()) {
            bool filtered = !dash.manifestFilter.empty() || !dash.tagFilter.empty() ||
                            !dash.searchQuery.empty();
            surface.text({frame.x, y++},
                         filtered ? "No matches — Esc clears filters." : assets::MANIFESTS_EMPTY,
                         theme::amber());
            return;
        }

        // Grouped list
        struct Row {
            bool header = false;
            std::string text;
            int idx = -1;
        };
        std::vector<Row> rows;
        std::string last;
        for (int i = 0; i < static_cast<int>(dash.manifests.size()); ++i) {
            const auto& m = dash.manifests[static_cast<size_t>(i)];
            if (m.category != last) {
                last = m.category;
                rows.push_back({true,
                                "▸ " + m.category + "  (" + std::to_string(catCounts[m.category]) + ")",
                                -1});
            }
            rows.push_back({false, "", i});
        }
        int selRow = 0;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            if (!rows[static_cast<size_t>(i)].header &&
                rows[static_cast<size_t>(i)].idx == dash.manifestIndex) {
                selRow = i;
                break;
            }

        int listW = L.listW;
        int listBottom = frame.bottom();
        // On narrow layouts reserve a floating swipe card strip at the bottom.
        const bool floatingCard = !L.showDetail && frame.h >= 14;
        if (floatingCard) listBottom = frame.bottom() - std::min(12, frame.h / 3);

        int visible = std::max(1, listBottom - y);
        int start = std::max(0, std::min(selRow - visible / 3,
                                         std::max(0, static_cast<int>(rows.size()) - visible)));
        const std::string am = activeManifest();
        const std::string an = activeName();

        const float cardT = dash.cardAnimT();
        const bool swiping = dash.cardAnimating();
        const int nudge = swiping ? components::listNudgeX(dash.cardAnimDir, cardT) : 0;

        for (int ri = start; ri < static_cast<int>(rows.size()) && y < listBottom; ++ri) {
            const auto& row = rows[static_cast<size_t>(ri)];
            if (row.header) {
                surface.text({frame.x, y++}, inkcell::text::truncate(row.text, listW),
                             theme::amber_soft());
                continue;
            }
            const auto& m = dash.manifests[static_cast<size_t>(row.idx)];
            bool selected = row.idx == dash.manifestIndex;
            bool wasPrev = swiping && row.idx == dash.cardPrevIndex;
            bool active =
                m.kind == "agent" && ((!an.empty() && m.name == an) || (!am.empty() && m.path == am));

            int rowX = frame.x + (selected ? nudge : (wasPrev ? -nudge / 2 : 0));
            int rowW = std::max(8, listW - (rowX - frame.x));
            components::drawCardRow(surface, {rowX, y, rowW, 1}, selected, active);
            components::kindChip(surface, rowX + 2, y, m.kind, selected);

            std::string name = std::string(active ? "● " : "  ") + m.name;
            if (m.launchable && m.kind == "agent") name += selected ? "  ↵ launch" : "";
            if (m.kind == "workflow") {
                name += selected ? "  ↵ run" : "  ▷";
                auto live = model_->workflowRun.snapshot();
                if (live.live && (live.path == m.path || live.name == m.name))
                    name += "  ●";
            }
            int nameCol = rowX + 8;
            int nameBudget = rowW - 10;
            if (L.showTagColumn && L.tagColMax > 0) {
                nameBudget = std::max(10, rowW - 10 - L.tagColMax);
                components::drawTagChips(surface, nameCol + nameBudget + 1, y, L.tagColMax, m.tags,
                                         4);
            }
            auto nameSt = selected ? theme::bright() : theme::text();
            if (wasPrev) nameSt = theme::italic_dim();
            surface.text({nameCol, y}, inkcell::text::truncate(name, nameBudget), nameSt);
            ++y;
        }

        // ── Detail / floating card with curved swipe ─────────────────
        inkcell::Rect det;
        if (L.showDetail) {
            det = {frame.x + L.detailX, frame.y + 4, L.detailW, frame.h - 5};
        } else if (floatingCard) {
            int ch = frame.bottom() - listBottom;
            det = {frame.x, listBottom, frame.w, ch};
        } else {
            return;
        }

        // Card well — fill only for non-workflow; canvas wants field void.
        const auto* selPeek = dash.selectedManifest();
        const bool wfDetail = selPeek && selPeek->kind == "workflow";
        if (!wfDetail) surface.fill(det, " ", theme::panel_bg());

        auto paintManifestBody = [&](inkcell::Surface& s, inkcell::Rect inner, float alpha,
                                     const catalog::ManifestEntry& m) {
            // Workflows get infinite canvas + rail — not the generic field card.
            if (m.kind == "workflow" && alpha >= 0.55f) {
                drawWorkflowStage(s, inner, m, gfx::nowSeconds());
                return;
            }

            bool ghost = alpha < 0.55f;
            auto titleSt = ghost ? theme::muted() : theme::bright();
            auto kindSt = ghost ? theme::dim() : theme::kindAccent(m.kind, true);
            auto bodySt = ghost ? theme::italic_dim() : theme::text();
            int dy = inner.y;
            int ix = inner.x;
            int iw = inner.w;
            if (dy >= inner.bottom()) return;

            // Title row: name + version chip
            std::string title = m.name;
            if (!m.version.empty()) title += "  v" + m.version;
            s.text({ix, dy++}, inkcell::text::truncate(title, iw), titleSt);
            if (dy >= inner.bottom()) return;

            // Kind · category · flags
            std::string meta = std::string(assets::kindLabel(m.kind)) + " · " + m.category;
            if (m.nested) meta += " · nested";
            if (m.builtin) meta += " · builtin";
            if (m.launchable) meta += " · launchable";
            s.text({ix, dy++}, inkcell::text::truncate(meta, iw), kindSt);

            if (!m.summary.empty() && dy < inner.bottom() - 10) {
                for (const auto& line : chat::wrapWordsLossless(m.summary, iw)) {
                    if (dy >= inner.bottom() - 10) break;
                    s.text({ix, dy++}, line, bodySt);
                }
            }
            if (dy < inner.bottom()) ++dy;

            if (dy < inner.bottom())
                components::fieldLine(s, ix, dy++, iw, "kind", m.kind);
            if (dy < inner.bottom() && !m.version.empty())
                components::fieldLine(s, ix, dy++, iw, "version", m.version);
            if (dy < inner.bottom())
                components::fieldLine(s, ix, dy++, iw, "category", m.category);
            if (dy < inner.bottom() && (!m.provider.empty() || !m.model.empty()))
                components::fieldLine(s, ix, dy++, iw, "engine",
                                      nonempty(m.provider, "?") + "/" + nonempty(m.model, "?"));
            if (dy < inner.bottom())
                components::fieldLine(s, ix, dy++, iw, "source", nonempty(m.source, "—"));
            if (dy < inner.bottom())
                components::fieldLine(s, ix, dy++, iw, "rel",
                                      m.relPath.empty() ? "—" : m.relPath);
            if (dy < inner.bottom())
                components::fieldLine(s, ix, dy++, iw, "path",
                                      m.path.empty() ? "—" : m.path);
            if (dy < inner.bottom()) {
                std::string flags;
                if (m.launchable) flags += "launch ";
                if (m.nested) flags += "nested ";
                if (m.builtin) flags += "builtin ";
                if (flags.empty()) flags = "—";
                components::fieldLine(s, ix, dy++, iw, "flags", flags);
            }

            if (dy < inner.bottom()) {
                ++dy;
                if (dy < inner.bottom()) {
                    auto tagHead = ghost ? theme::dim() : theme::violet_soft();
                    s.text({ix, dy++}, "TAGS", tagHead);
                }
                std::string all;
                for (const auto& t : m.tags) {
                    if (!all.empty()) all += "  ";
                    all += "#" + t;
                }
                auto tagSt = ghost ? theme::italic_dim() : theme::italic();
                tagSt.fg = theme::violet_soft().fg;
                for (const auto& line : chat::wrapWordsLossless(all.empty() ? "—" : all, iw)) {
                    if (dy >= inner.bottom() - 2) break;
                    s.text({ix, dy++}, line, tagSt);
                }
            }
            if (m.launchable && m.kind == "agent" && dy < inner.bottom()) {
                bool isLive =
                    (!am.empty() && m.path == am) || (!an.empty() && m.name == an);
                if (dy < inner.bottom()) ++dy;
                if (dy < inner.bottom())
                    s.text({ix, dy++},
                           isLive ? "LIVE · enter opens chat" : "ENTER LAUNCHES",
                           ghost ? theme::green_soft() : theme::green());
            }
        };

        const auto* sel = dash.selectedManifest();
        if (!sel) return;

        const int cardW = det.w;
        const int cardH = det.h;
        // Rest pose fills the well
        auto restPose = components::CardPose{det.x, det.y, 1.f};

        if (swiping && dash.cardPrevIndex >= 0 &&
            dash.cardPrevIndex < static_cast<int>(dash.manifests.size())) {
            const auto& prev = dash.manifests[static_cast<size_t>(dash.cardPrevIndex)];
            auto outP = components::outgoingPose(det, dash.cardAnimDir, cardT);
            auto inP = components::incomingPose(det, dash.cardAnimDir, cardT);

            // Outgoing first (under), then incoming on top
            components::drawSwipedCard(
                surface, det, outP, cardW, cardH, 0.8f,
                [&](inkcell::Surface& s, inkcell::Rect inner, float a) {
                    paintManifestBody(s, inner, a, prev);
                });
            components::drawSwipedCard(
                surface, det, inP, cardW, cardH, 0.15f,
                [&](inkcell::Surface& s, inkcell::Rect inner, float a) {
                    paintManifestBody(s, inner, a, *sel);
                });
        } else {
            components::drawSwipedCard(
                surface, det, restPose, cardW, cardH, 0.15f,
                [&](inkcell::Surface& s, inkcell::Rect inner, float a) {
                    paintManifestBody(s, inner, a, *sel);
                });
        }
    }

    // AAA options: j/k focus · h/l or enter cycle · no redundant dumps
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

    void drawSettings(inkcell::Surface& surface, inkcell::Rect frame) const {
        // Title plate
        surface.text({frame.x, frame.y}, "SETTINGS", theme::bright());
        surface.text({frame.x + 10, frame.y}, "OPTIONS",
                     theme::italic_dim());

        int y = frame.y + 2;
        // Live preview strip — what the field actually is right now
        int previewH = std::min(5, std::max(3, frame.h / 6));
        if (y + previewH + 8 < frame.bottom()) {
            inkcell::Rect prev{frame.x, y, frame.w, previewH};
            if (gfx::fieldEnabled()) {
                gfx::drawFieldBg(surface, prev, gfx::themeVariantIndex(), gfx::nowSeconds());
            } else {
                surface.fill(prev, " ", theme::base_bg());
            }
            // Overlay label on preview
            std::string tag = gfx::fieldEnabled()
                                  ? std::string("FIELD  ·  ") + gfx::activeFieldName()
                                  : std::string("FIELD  ·  OFF");
            auto tagSt = theme::bright();
            tagSt.bg = inkcell::Color::rgb(0, 0, 0);
            surface.text({prev.x + 2, prev.y + previewH / 2},
                         inkcell::text::truncate(tag, prev.w - 4), tagSt);
            y = prev.bottom() + 1;
        }

        // Section labels
        auto section = [&](const char* name) {
            if (y >= frame.bottom()) return;
            surface.text({frame.x, y++}, name, theme::violet());
        };

        // Game-style option row:  [▸] LABEL ……… ◂ VALUE ▸   bind
        auto option = [&](int idx, const char* label, const std::string& value, const char* bind,
                          bool carousel) {
            if (y >= frame.bottom()) return;
            bool foc = (dashFocus() == idx);
            auto rowBg = foc ? theme::panel_3() : theme::panel_2();
            surface.fill({frame.x, y, frame.w, 1}, " ", rowBg);
            if (foc)
                surface.text({frame.x, y}, "▌",
                             theme::cyan().with_bg(rowBg.bg));

            auto labSt = (foc ? theme::bright() : theme::muted()).with_bg(rowBg.bg);
            surface.text({frame.x + 2, y}, inkcell::text::truncate(label, 16), labSt);

            // Value — centered carousel or toggle glyph
            std::string val = value;
            if (carousel) val = "◂  " + value + "  ▸";
            int vw = inkcell::text::display_width(val);
            int vx = frame.x + std::max(20, (frame.w - vw) / 2);
            auto valSt = (foc ? theme::bright() : theme::text()).with_bg(rowBg.bg);
            if (foc) valSt.bold = true;
            surface.text({vx, y}, inkcell::text::truncate(val, frame.w - 24), valSt);

            // Bind chip right
            auto bindSt = theme::italic_accent().with_bg(rowBg.bg);
            int bw = inkcell::text::display_width(bind);
            surface.text({frame.right() - bw - 1, y}, bind, bindSt);
            ++y;
        };

        section("DISPLAY");
        option(0, "THEME", upperCopy(theme::name()), "T / ←→", true);
        option(1, "FIELD", gfx::fieldEnabled() ? "ON" : "OFF", "B", false);
        option(2, "SHADER",
               gfx::fieldEnabled() ? upperCopy(gfx::activeFieldName()) : std::string("—"),
               "S / ←→", true);

        if (y < frame.bottom()) ++y;
        section("CHAT");
        option(3, "THOUGHTS", model_->showThoughts ? "ON" : "OFF", "^T", false);
        option(4, "TRUNCATE", model_->truncateBodies ? "ON" : "OFF", "^O", false);
        option(5, "RAW STREAM", model_->showRaw ? "ON" : "OFF", "^R", false);
        option(6, "CHAT FIELD",
               model_->chatFieldEnabled
                   ? (gfx::fieldEnabled() ? std::string("ON  · ")
                                              + gfx::activeFieldName()
                                            : std::string("ON  ·  hub off"))
                   : "OFF",
               "B / S", true);

        // Single footer — path only, no key encyclopedia
        if (y + 1 < frame.bottom()) {
            y = frame.bottom() - 1;
            surface.text({frame.x, y},
                         inkcell::text::truncate(
                             std::string("j/k select  ·  h/l or enter cycle  ·  ") + uiPrefsPath(),
                             frame.w),
                         theme::italic_dim());
        }
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
            model_->pendingRoute = "agent";
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
    void drawWorkflowStage(inkcell::Surface& surface, inkcell::Rect frame,
                           const catalog::ManifestEntry& m, float tsec) const {
        if (frame.w < 12 || frame.h < 6) return;

        auto& engine = workflows::WorkflowEngine::instance();
        auto& loaded = engine.load(m.path);
        if (!loaded.isValid()) {
            surface.text({frame.x, frame.y}, "failed to load workflow", theme::red());
            surface.text({frame.x, frame.y + 1},
                         inkcell::text::truncate(m.path, frame.w), theme::dim());
            return;
        }
        const auto& mf = loaded.manifest();
        auto graph = components::buildCanvasGraph(mf);

        auto run = model_->workflowRun.snapshot();
        const bool liveHere =
            (run.live || model::runStatusActive(run.status) ||
             run.status == model::RunStatus::Succeeded ||
             run.status == model::RunStatus::Failed ||
             run.status == model::RunStatus::Cancelled) &&
            (!run.path.empty() ? run.path == m.path : run.name == m.name);
        if (liveHere) components::applyRunStatusToGraph(graph, run);

        // Header strip (2 rows)
        std::string title = m.name;
        if (!m.version.empty()) title += "  v" + m.version;
        surface.text({frame.x, frame.y}, inkcell::text::truncate(title, frame.w), theme::bright());

        model::WorkflowTopology topo;
        model::countTopo(mf.steps, topo);
        std::string meta = components::topologyLine(topo);
        if (!m.summary.empty()) meta += "  ·  " + m.summary;
        if (liveHere) {
            meta = std::string(model::runStatusLabel(run.status)) + "  ·  " +
                   components::formatRunElapsed(run.elapsedMs) + "  ·  " + meta;
        }
        surface.text({frame.x, frame.y + 1}, inkcell::text::truncate(meta, frame.w),
                     liveHere ? components::runStatusChipStyle(run.status) : theme::italic_dim());

        int bodyTop = frame.y + 3;
        int eventH = liveHere ? std::min(5, std::max(2, frame.h / 6)) : 0;
        int canvasH = std::max(4, frame.bottom() - bodyTop - eventH - (liveHere ? 0 : 1));
        inkcell::Rect canvas{frame.x, bodyTop, frame.w, canvasH};

        // Camera — settle path changes / center sentinel
        auto& dashMut = const_cast<model::DashboardState&>(model_->dashboard);
        components::CanvasCamera cam;
        cam.x = dashMut.wfCamX;
        cam.y = dashMut.wfCamY;
        bool needFrame = (dashMut.wfCanvasPath != m.path) || cam.x > 1e8f;
        if (needFrame) {
            if (!graph.nodes.empty() && dashMut.wfFocusNode >= 0 &&
                dashMut.wfFocusNode < static_cast<int>(graph.nodes.size()) && cam.x > 1e8f) {
                components::cameraCenterNode(
                    cam, graph.nodes[static_cast<size_t>(dashMut.wfFocusNode)], canvas.w,
                    canvas.h);
            } else {
                components::cameraFrameGraph(cam, graph, canvas.w, canvas.h);
            }
            dashMut.wfCamX = cam.x;
            dashMut.wfCamY = cam.y;
            dashMut.wfCanvasPath = m.path;
            if (dashMut.wfFocusNode >= static_cast<int>(graph.nodes.size()))
                dashMut.wfFocusNode = 0;
        }
        if (dashMut.wfFocusNode < 0) dashMut.wfFocusNode = 0;
        if (!graph.nodes.empty()) {
            int n = static_cast<int>(graph.nodes.size());
            dashMut.wfFocusNode = dashMut.wfFocusNode % n;
            if (dashMut.wfFocusNode < 0) dashMut.wfFocusNode += n;
        }

        components::CanvasDrawOpts opt;
        opt.selected = dashMut.wfFocusNode;
        opt.tSec = tsec;
        opt.showChrome = true;
        if (liveHere && run.currentIdx >= 0 &&
            run.currentIdx < static_cast<int>(run.steps.size()))
            opt.currentId = run.steps[static_cast<size_t>(run.currentIdx)].id;
        opt.statusLine = dashMut.wfCanvasFocus
                             ? "hjkl pan · [] node · . center · z expand · ↵ run · Esc stop"
                             : "tab canvas · z expand · ↵ run";
        if (liveHere && !run.lastError.empty() &&
            (run.status == model::RunStatus::Failed ||
             run.status == model::RunStatus::Cancelled))
            opt.statusLine = run.lastError;

        components::drawWorkflowCanvas(surface, canvas, graph, cam, opt);

        // Live event strip under canvas
        if (liveHere && eventH > 0) {
            inkcell::Rect strip{frame.x, canvas.bottom(), frame.w, frame.bottom() - canvas.bottom()};
            if (strip.h >= 2) {
                components::hairline(surface, strip.x, strip.y, strip.w, theme::dim());
                int ey = strip.y + 1;
                int n = static_cast<int>(run.events.size());
                int vis = std::max(1, strip.bottom() - ey);
                int start = n > vis ? n - vis : 0;
                for (int i = start; i < n && ey < strip.bottom(); ++i) {
                    const auto& ev = run.events[static_cast<size_t>(i)];
                    auto st = theme::dim();
                    if (ev.kind.find("fail") != std::string::npos)
                        st = theme::red();
                    else if (ev.kind == "step.ok" || ev.kind == "done")
                        st = theme::green_soft();
                    else if (ev.kind == "step.enter" || ev.kind == "hitl")
                        st = theme::cyan();
                    else if (ev.kind == "checkpoint" || ev.kind == "emit")
                        st = theme::amber_soft();
                    std::string line = ev.kind;
                    if (!ev.text.empty()) line += "  " + ev.text;
                    surface.text({strip.x, ey++},
                                 inkcell::text::truncate(line, strip.w), st);
                }
            }
        }
    }

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
        model_->pendingRoute = "agent";
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
        model_->pendingRoute = "agent";
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
