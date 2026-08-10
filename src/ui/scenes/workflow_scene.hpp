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
#include "src/ui/components/workflow_stage.hpp"
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
        // NOTE: the camera (zoom/pan/selection) for the active workflow is
        // restored by drawWorkflowDetail itself (path-changed branch) — for
        // BOTH the hub canvas and this page, and across restarts.
        const std::string n = model_->activeWorkflowName.empty()
                                  ? "workflow"
                                  : model_->activeWorkflowName;
        model_->dashboard.notice =
            "workflow · " + n + " · ↵ run · [] node · +/- zoom · r reload · x stop · m hub";
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

        if (event.code == KeyCode::Backspace) {
            model_->requestRoute(PendingRoute::Main);
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
        if (event.code == KeyCode::Enter) {
            tryRun();
            return true;
        }
        if (event.code == KeyCode::Character && (event.ch == 'r' || event.ch == 'R')) {
            // Reload the manifest from disk (drop engine cache).
            workflows::WorkflowEngine::instance().reload(model_->activeWorkflowManifestPath);
            dash.wfCanvasPath.clear();  // force re-frame + rebuild next draw
            dash.flashNotice("reloaded " + model_->activeWorkflowName);
            return true;
        }
        if (event.code == KeyCode::Character && (event.ch == 'x' || event.ch == 'X')) {
            model_->pendingStopWorkflow = true;
            model_->workflowRun.requestCancel();
            dash.flashNotice("stop requested");
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
            if (event.ch == '.' ) {
                dash.wfAnimFramePending = true;  // animates from CURRENT cam
                dash.flashNotice("frame");
                return true;
            }
            if (event.ch == '[' || event.ch == '{') {
                dash.wfFocusNode = dash.wfFocusNode + 1;
                dash.wfAnimNodePending = true;
                dash.flashNotice("node " + std::to_string(dash.wfFocusNode));
                return true;
            }
            if (event.ch == ']' || event.ch == '}') {
                dash.wfFocusNode = dash.wfFocusNode - 1;
                dash.wfAnimNodePending = true;
                dash.flashNotice("node " + std::to_string(dash.wfFocusNode));
                return true;
            }
            if (event.ch == '=' || event.ch == '+') {
                dash.wfZoomStep = 1.f;
                dash.flashNotice("zoom in");
                return true;
            }
            if (event.ch == '-' || event.ch == '_') {
                dash.wfZoomStep = -1.f;
                dash.flashNotice("zoom out");
                return true;
            }
        }
        return false;
    }

    void draw(inkcell::Surface& surface) const override {
        if (layout::render_min_size_notice(surface)) return;
        auto p = layout::page(surface);
        surface.clear(theme::base_bg());
        // No top header — the workflow page is canvas-first; the stage band
        // (name/status/meta) is the only chrome.

        catalog::ManifestEntry m;
        m.kind = "workflow";
        m.path = model_->activeWorkflowManifestPath;
        m.name = model_->activeWorkflowName;
        if (m.path.empty()) {
            surface.text({p.x, p.y + 4}, "no workflow selected", theme::amber());
            surface.text({p.x, p.y + 5}, "m · back to hub", theme::dim());
            return;
        }
        if (m.name.empty()) {
            auto& eng = workflows::WorkflowEngine::instance();
            auto& loaded = eng.load(m.path);
            if (loaded.isValid()) m.name = loaded.manifest().name;
            if (m.name.empty()) m.name = "workflow";
        }

        int y = p.y + 1;
        inkcell::Rect stage{p.x, y, p.w, std::max(6, p.bottom() - y)};
        components::drawWorkflowDetail(surface, stage, m, *model_, gfx::nowSeconds());
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

};

}  // namespace cortex::mk3::ui::scenes
