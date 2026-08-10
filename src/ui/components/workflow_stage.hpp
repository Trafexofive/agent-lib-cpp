#pragma once
// Full-page workflow stage — shared by the hub Workflows section and the
// dedicated WorkflowScene. Flat chrome (no box borders): panel_2 header band
// with name+version / status+elapsed, summary meta row, canvas with a
// RESERVED status row (nodes can overflow the unclipped viewport, so the
// status row is masked with panel_bg after the canvas), live event strip.

#include "src/ui/components/workflow_canvas.hpp"
#include "src/ui/components/workflow_rail.hpp"
#include "src/ui/components/workflow_run.hpp"
#include "src/ui/model/inkcell_app_model.hpp"
#include "src/ui/model/ui_prefs.hpp"
#include "src/ui/model/workflow_run_model.hpp"
#include "src/ui/model/workflow_runner.hpp"
#include "src/workflows/workflow_engine.hpp"

namespace cortex::mk3::ui::components {

// Draw the full workflow detail stage. Mirrors hub drawWorkflowStage exactly;
// both surfaces must render identically.
inline void drawWorkflowDetail(inkcell::Surface& surface, inkcell::Rect frame,
                               const catalog::ManifestEntry& m, const ShellModel& model,
                               float tsec) {
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
    auto graph = buildCanvasGraph(mf);

    auto run = model.workflowRun.snapshot();
    const bool liveHere =
        (run.live || model::runStatusActive(run.status) ||
         run.status == model::RunStatus::Succeeded ||
         run.status == model::RunStatus::Failed ||
         run.status == model::RunStatus::Cancelled) &&
        (!run.path.empty() ? run.path == m.path : run.name == m.name);
    if (liveHere) applyRunStatusToGraph(graph, run);

    // Header band (flat, panel_2): name+version left · status/elapsed right.
    fillRect(surface, {frame.x, frame.y, frame.w, 1}, theme::panel_2());
    std::string title = m.name.empty() ? mf.name : m.name;
    if (!mf.version.empty()) title += "  v" + mf.version;
    surface.text({frame.x + 2, frame.y}, inkcell::text::truncate(title, frame.w - 26),
                 theme::bright());
    std::string right;
    inkcell::Style rightSt = theme::dim();
    if (liveHere) {
        right = std::string(model::runStatusLabel(run.status)) + "  ·  " +
                formatRunElapsed(run.elapsedMs);
        rightSt = runStatusChipStyle(run.status);
    } else {
        model::WorkflowTopology topo;
        model::countTopo(mf.steps, topo);
        right = topologyLine(topo);
    }
    if (!right.empty()) {
        int rw = (int)inkcell::text::display_width(right);
        surface.text({std::max(frame.x + 2, frame.right() - rw - 2), frame.y},
                     inkcell::text::truncate(right, frame.w - 8), rightSt);
    }
    if (!mf.summary.empty())
        surface.text({frame.x + 2, frame.y + 1},
                     inkcell::text::truncate(mf.summary, frame.w - 4), theme::italic_dim());

    int bodyTop = frame.y + 3;
    int eventH = liveHere ? std::min(5, std::max(2, frame.h / 6)) : 0;
    int canvasH = std::max(4, frame.bottom() - bodyTop - eventH - 1);  // reserve status row
    inkcell::Rect canvas{frame.x, bodyTop, frame.w, canvasH};

    auto& dashMut = const_cast<model::DashboardState&>(model.dashboard);
    {
        int n = (int)graph.nodes.size();
        if (dashMut.wfFocusNode >= n) dashMut.wfFocusNode = 0;
        if (dashMut.wfFocusNode < 0) dashMut.wfFocusNode = n > 0 ? n - 1 : 0;
        // Autofollow: while a run is live, pin focus to the running node so the
        // canvas tracks progress without [ ] spam (operator can still override).
        auto live = model.workflowRun.snapshot();
        if (live.live && !graph.nodes.empty()) {
            int runIdx = -1;
            for (int i = 0; i < n; ++i) {
                if (graph.nodes[(size_t)i].status == model::StepStatus::Running) {
                    runIdx = i;
                    break;
                }
            }
            if (runIdx < 0 && live.currentIdx >= 0 && live.currentIdx < n)
                runIdx = live.currentIdx;
            if (runIdx >= 0 && runIdx != dashMut.wfFocusNode) {
                dashMut.wfFocusNode = runIdx;
                dashMut.wfAnimNodePending = true;  // smooth cam to node
            }
        }
    }
    components::CanvasCamera cam;
    cam.x = dashMut.wfCamX;
    cam.y = dashMut.wfCamY;
    cam.zoom = std::max(kCanvasMinZoom, std::min(kCanvasMaxZoom, dashMut.wfCamZoom));
    // Self-heal: an out-of-range camera (e.g. a legacy 1e8 sentinel or a
    // corrupted saved cam) would put the whole graph off-screen forever.
    // Treat it as "never framed" → the pathChanged branch re-fits next draw.
    if (std::fabs(cam.x) > 100000.f || std::fabs(cam.y) > 100000.f) {
        dashMut.wfCanvasPath.clear();
        cam.x = 0;
        cam.y = 0;
    }

    // Only snap-frame on a path change (no animation). Everything else (zoom,
    // '.' frame, '['/']' node focus, mid-flight animation) goes through the
    // pending-intent + animated camera resolver below.
    const bool pathChanged = dashMut.wfCanvasPath != m.path;
    // Resolve a target camera from pending intents (consumed here).
    auto resolveTarget = [&](components::CanvasCamera base) {
        components::CanvasCamera t = base;
        if (std::fabs(dashMut.wfZoomStep) > 0.01f) {
            zoomAround(t, canvas.w, canvas.h,
                       dashMut.wfZoomStep > 0 ? kCanvasZoomStep : 1.f / kCanvasZoomStep);
            dashMut.wfZoomStep = 0;
        }
        if (dashMut.wfAnimNodePending && !graph.nodes.empty()) {
            dashMut.wfAnimNodePending = false;
            const int fi = dashMut.wfFocusNode % (int)graph.nodes.size();
            t = base;
            components::cameraCenterNode(t, graph.nodes[(size_t)fi], canvas.w, canvas.h);
        }
        if (dashMut.wfAnimFramePending) {
            dashMut.wfAnimFramePending = false;
            t = base;
            // Anchor the dolly on the WORLD POINT under the viewport centre,
            // so the workflow content you are inspecting stays pinned and
            // visible while the rest of the graph sweeps in around it.
            components::cameraFitGraph(t, graph, canvas.w, canvas.h);
        }
        return t;
    };
    // Ease — smooth start/stop (quad, not cubic: flatter mid-slope means a
    // lower peak velocity per frame at 30fps → steadier, no stutter).
    auto easeInOut = [](float x) {
        return x < 0.5f ? 2.f * x * x : 1.f - std::pow(-2.f * x + 2.f, 2.f) / 2.f;
    };

    if (pathChanged) {
        // Fresh manifest: restore the camera this workflow was left at (from
        // the persisted table — works for BOTH the hub canvas and the page,
        // across restarts). Only fit the overview if it was never visited.
        if (const auto* saved = wfCamFind(m.path)) {
            cam.zoom = std::max(kCanvasMinZoom, std::min(kCanvasMaxZoom,
                                                         (float)saved->zoomX100 / 100.f));
            cam.x = (float)saved->x;
            cam.y = (float)saved->y;
            dashMut.wfFocusNode = saved->focus;
        } else {
            components::cameraFitGraph(cam, graph, canvas.w, canvas.h);
        }
        dashMut.wfCamX = cam.x;
        dashMut.wfCamY = cam.y;
        dashMut.wfCanvasPath = m.path;
        dashMut.wfAnimActive = false;
    } else if (dashMut.wfAnimActive) {
        // Continue the in-flight animation.
        float p = (tsec - dashMut.wfAnimT0) / (dashMut.wfAnimDur > 0.001f ? dashMut.wfAnimDur : 1.f);
        if (p >= 1.f) {
            cam.x = dashMut.wfAnimToX;
            cam.y = dashMut.wfAnimToY;
            cam.zoom = dashMut.wfAnimToZ;
            dashMut.wfAnimActive = false;
        } else {
            // Zoom stays on the target axis but is EASED EARLY relative to the
            // pan: the view widens to the target as fast as possible, so the
            // pan that follows always travels across a view wide enough to
            // hold the workflow. The one easing curve, applied at 1.65x speed
            // to the zoom axis, guarantees the graph never drops out of view
            // mid-flight and lands dead-centre.
            float e = easeInOut(p);
            float ez = easeInOut(std::min(1.f, p * 1.65f));
            cam.x = dashMut.wfAnimFromX + (dashMut.wfAnimToX - dashMut.wfAnimFromX) * e;
            cam.y = dashMut.wfAnimFromY + (dashMut.wfAnimToY - dashMut.wfAnimFromY) * e;
            cam.zoom = dashMut.wfAnimFromZ + (dashMut.wfAnimToZ - dashMut.wfAnimFromZ) * ez;
        }
    } else if (dashMut.wfZoomStep != 0.f || dashMut.wfAnimFramePending ||
               dashMut.wfAnimNodePending) {
        // Start an animated transition to the resolved target.
        components::CanvasCamera to = resolveTarget(cam);
        dashMut.wfAnimFromX = cam.x;
        dashMut.wfAnimFromY = cam.y;
        dashMut.wfAnimFromZ = cam.zoom;
        dashMut.wfAnimToX = to.x;
        dashMut.wfAnimToY = to.y;
        dashMut.wfAnimToZ = to.zoom;
        dashMut.wfAnimT0 = tsec;
        dashMut.wfAnimDur = 0.45f;
        dashMut.wfAnimActive = true;
        // First frame keeps the source camera; motion interpolates from here.
    }
    // Snap the camera to whole cells: every node/edge then translates in
    // lockstep (no per-node lround jitter → no shimmer during the tween).
    cam.x = std::round(cam.x);
    cam.y = std::round(cam.y);
    dashMut.wfCamX = cam.x;
    dashMut.wfCamY = cam.y;
    dashMut.wfCamZoom = cam.zoom;

    CanvasDrawOpts opt;
    opt.selected = dashMut.wfFocusNode;
    opt.tSec = tsec;
    opt.showChrome = true;
    if (liveHere && run.currentIdx >= 0 && run.currentIdx < (int)run.steps.size())
        opt.currentId = run.steps[(size_t)run.currentIdx].id;
    opt.statusLine = "";  // status row is reserved below the canvas
    drawWorkflowCanvas(surface, canvas, graph, cam, opt);

    // Live event strip under canvas (canvas bottom .. status row).
    if (liveHere && eventH > 0) {
        inkcell::Rect strip{frame.x, canvas.bottom(), frame.w,
                            frame.bottom() - canvas.bottom() - 1};
        if (strip.h >= 2) {
            hairline(surface, strip.x, strip.y, strip.w, theme::dim());
            int ey = strip.y + 1;
            int n = (int)run.events.size();
            int vis = std::max(1, strip.bottom() - ey);
            int start = n > vis ? n - vis : 0;
            for (int i = start; i < n && ey < strip.bottom(); ++i) {
                const auto& ev = run.events[(size_t)i];
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

    // Reserved status row (bottom) — masked so node overflow can't bleed in.
    int sy = frame.bottom() - 1;
    fillRect(surface, {frame.x, sy, frame.w, 1}, theme::panel_bg());
    std::string status = dashMut.wfCanvasFocus
                             ? "hjkl pan · [] node · . frame · +/- zoom · ↵ run · r reload · Esc stop"
                             : "tab canvas · ↵ run";
    if (liveHere && !run.lastError.empty() &&
        (run.status == model::RunStatus::Failed ||
         run.status == model::RunStatus::Cancelled))
        status = run.lastError;
    surface.text({frame.x + 1, sy}, inkcell::text::truncate(status, frame.w - 2),
                 theme::italic_accent());

    // Remember this workflow's camera so reopening the page restores it.
    ui::wfCamRemember(m.path, cam.zoom, cam.x, cam.y, dashMut.wfFocusNode);
}

}  // namespace cortex::mk3::ui::components
