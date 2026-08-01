#pragma once
// Workflow scene — full-page canvas + run for one workflow.yml.
// Enter from Manifests or hub Workflows section deep-link.
// FOR NOW: thin wrapper around the same stage used by hub drawWorkflows.

#include <string>

#include "base_scene.hpp"
#include "src/core/agent_catalog.hpp"
#include "src/ui/components/cmd_palette.hpp"
#include "src/ui/components/workflow_canvas.hpp"
#include "src/ui/components/workflow_rail.hpp"
#include "src/ui/components/workflow_run.hpp"
#include "src/ui/gfx/shaders_dedsec.hpp"
#include "src/ui/model/ui_prefs.hpp"
#include "src/ui/model/workflow_run_model.hpp"
#include "src/ui/model/workflow_runner.hpp"
#include "src/workflows/workflow_engine.hpp"

namespace cortex::mk3::ui::scenes {

class WorkflowScene final : public BaseScene {
   public:
    using BaseScene::BaseScene;
    std::string name() const override { return "Workflow"; }

    void on_enter() override {
        BaseScene::on_enter();
        model_->composer.focused = false;
        model_->timelineFocus = false;
        model_->dashboard.wfCanvasFocus = true;
        model_->status = "ready";
        const std::string n = model_->activeWorkflowName.empty()
                                  ? "workflow"
                                  : model_->activeWorkflowName;
        model_->dashboard.notice =
            "workflow · " + n + " · ↵ run · z expand · x stop · m hub";
    }

    bool on_key(const inkcell::KeyEvent& event) override {
        using inkcell::KeyCode;
        auto& dash = model_->dashboard;

        if (model_->cmdPalette.open && !model_->cmdPalette.closing) {
            std::string action;
            if (components::handleCmdPaletteKey(model_->cmdPalette, event, &action)) {
                if (action == "nav.main") model_->requestRoute(PendingRoute::Main);
                else if (action == "sys.quit") model_->requestRoute(PendingRoute::Quit);
                else if (action == "act.wf_run") tryRun();
                else if (action == "act.wf_stop") {
                    model_->pendingStopWorkflow = true;
                    model_->workflowRun.requestCancel();
                    dash.flashNotice("stopping…");
                }
                return true;
            }
        }
        if (event.code == KeyCode::Character && event.ctrl() &&
            (event.ch == 'p' || event.ch == 'P')) {
            model_->cmdPalette.toggle(components::hubCommands());
            return true;
        }

        if (event.code == KeyCode::Escape) {
            if (model_->workflowRun.isLive() || model_->workflowRun.isActive()) {
                model_->pendingStopWorkflow = true;
                model_->workflowRun.requestCancel();
                dash.flashNotice("stopping workflow…");
                return true;
            }
            model_->requestRoute(PendingRoute::Main);
            return true;
        }
        if (event.code == KeyCode::Character &&
            (event.ch == 'm' || event.ch == 'M' || event.ch == 'a' || event.ch == 'A')) {
            model_->requestRoute(PendingRoute::Main);
            return true;
        }
        if (event.code == KeyCode::Enter ||
            (event.code == KeyCode::Character && (event.ch == 'r' || event.ch == 'R'))) {
            tryRun();
            return true;
        }
        if (event.code == KeyCode::Character && (event.ch == 'x' || event.ch == 'X')) {
            model_->pendingStopWorkflow = true;
            model_->workflowRun.requestCancel();
            dash.flashNotice("stop requested");
            return true;
        }
        if (event.code == KeyCode::Character && (event.ch == 'z' || event.ch == 'Z')) {
            dash.wfCanvasExpanded = !dash.wfCanvasExpanded;
            dash.flashNotice(dash.wfCanvasExpanded ? "canvas expanded" : "canvas normal");
            return true;
        }
        // Pan
        if (event.code == KeyCode::Character && !event.ctrl()) {
            if (event.ch == 'h' || event.ch == 'H') {
                dash.wfCamX -= 4.f;
                return true;
            }
            if (event.ch == 'l' || event.ch == 'L') {
                dash.wfCamX += 4.f;
                return true;
            }
            if (event.ch == 'j' || event.ch == 'J') {
                dash.wfCamY += 2.f;
                return true;
            }
            if (event.ch == 'k' || event.ch == 'K') {
                dash.wfCamY -= 2.f;
                return true;
            }
            if (event.ch == '.') {
                dash.wfCamX = 1e9f;  // reframe sentinel
                dash.flashNotice("center");
                return true;
            }
        }
        return false;
    }

    void draw(inkcell::Surface& surface) const override {
        if (layout::render_min_size_notice(surface)) return;
        auto p = layout::page(surface);
        surface.clear(theme::base_bg());
        views::topbar(surface, cfg_, *model_, name());

        catalog::ManifestEntry m;
        m.kind = "workflow";
        m.path = model_->activeWorkflowManifestPath;
        m.name = model_->activeWorkflowName;
        if (m.path.empty()) {
            surface.text({p.x, p.y + 4}, "no workflow selected", theme::amber());
            surface.text({p.x, p.y + 5}, "m · back to hub", theme::dim());
            return;
        }
        // Load summary if missing
        if (m.name.empty()) {
            auto& eng = workflows::WorkflowEngine::instance();
            auto& loaded = eng.load(m.path);
            if (loaded.isValid()) m.name = loaded.manifest().name;
            if (m.name.empty()) m.name = "workflow";
        }

        int y = p.y + 3;
        inkcell::Rect stage{p.x, y, p.w, std::max(6, p.bottom() - y)};
        drawStage(surface, stage, m);
    }

   private:
    void tryRun() {
        auto& dash = model_->dashboard;
        const std::string& path = model_->activeWorkflowManifestPath;
        if (path.empty()) {
            dash.flashNotice("no workflow path");
            return;
        }
        if (!model::workflowRunnablePath(path, model_->activeWorkflowName)) {
            dash.flashNotice("workflow spec · not runnable");
            return;
        }
        if (model_->workflowRun.isLive()) {
            dash.flashNotice("already running · Esc/x stop");
            return;
        }
        model_->pendingRunWorkflow = path;
        dash.wfCanvasFocus = true;
        dash.flashNotice("running " + model_->activeWorkflowName + "…");
    }

    void drawStage(inkcell::Surface& surface, inkcell::Rect frame,
                   const catalog::ManifestEntry& m) const {
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

        std::string title = m.name.empty() ? mf.name : m.name;
        if (!mf.version.empty()) title += "  v" + mf.version;
        surface.text({frame.x, frame.y}, inkcell::text::truncate(title, frame.w),
                     theme::bright());

        model::WorkflowTopology topo;
        model::countTopo(mf.steps, topo);
        std::string meta = components::topologyLine(topo);
        if (!mf.summary.empty()) meta += "  ·  " + mf.summary;
        if (liveHere) {
            meta = std::string(model::runStatusLabel(run.status)) + "  ·  " +
                   components::formatRunElapsed(run.elapsedMs) + "  ·  " + meta;
        }
        surface.text({frame.x, frame.y + 1}, inkcell::text::truncate(meta, frame.w),
                     liveHere ? components::runStatusChipStyle(run.status)
                              : theme::italic_dim());

        int bodyTop = frame.y + 3;
        int eventH = liveHere ? std::min(5, std::max(2, frame.h / 6)) : 0;
        int canvasH = std::max(4, frame.bottom() - bodyTop - eventH);
        inkcell::Rect canvas{frame.x, bodyTop, frame.w, canvasH};

        auto& dashMut = const_cast<model::DashboardState&>(model_->dashboard);
        components::CanvasCamera cam;
        cam.x = dashMut.wfCamX;
        cam.y = dashMut.wfCamY;
        bool needFrame = (dashMut.wfCanvasPath != m.path) || cam.x > 1e8f;
        if (needFrame) {
            components::cameraFrameGraph(cam, graph, canvas.w, canvas.h);
            dashMut.wfCamX = cam.x;
            dashMut.wfCamY = cam.y;
            dashMut.wfCanvasPath = m.path;
        }

        components::CanvasDrawOpts opt;
        opt.selected = dashMut.wfFocusNode;
        opt.tSec = gfx::nowSeconds();
        opt.showChrome = true;
        if (liveHere && run.currentIdx >= 0 &&
            run.currentIdx < static_cast<int>(run.steps.size()))
            opt.currentId = run.steps[static_cast<size_t>(run.currentIdx)].id;
        opt.statusLine = "hjkl pan · . center · ↵ run · x stop · m hub";
        if (liveHere && !run.lastError.empty()) opt.statusLine = run.lastError;
        components::drawWorkflowCanvas(surface, canvas, graph, cam, opt);

        if (liveHere && eventH > 0) {
            inkcell::Rect strip{frame.x, canvas.bottom(), frame.w,
                                frame.bottom() - canvas.bottom()};
            if (strip.h >= 2) {
                int ey = strip.y + 1;
                int n = static_cast<int>(run.events.size());
                int vis = std::max(1, strip.bottom() - ey);
                int start = std::max(0, n - vis);
                for (int i = start; i < n && ey < strip.bottom(); ++i) {
                    const auto& ev = run.events[static_cast<size_t>(i)];
                    std::string line = ev.kind;
                    if (!ev.text.empty()) line += "  " + ev.text;
                    surface.text({strip.x, ey++},
                                 inkcell::text::truncate(line, strip.w),
                                 theme::dim());
                }
            }
        }
    }
};

}  // namespace cortex::mk3::ui::scenes
