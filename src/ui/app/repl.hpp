#pragma once
// Interactive inkcell entry points: shell/smoke/one-shot/repl runners. Each
// builds a ShellModel + inkcell App, then runs turn workers against the
// bridge. The REPL owns the hub hot-swap + workflow + submit dispatch.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "inkcell/app.hpp"
#include "src/core/agent.hpp"
#include "src/core/manifest_loader.hpp"
#include "src/session/controller.hpp"
#include "src/ui/app/agent_launcher.hpp"
#include "src/ui/app/agent_turn.hpp"
#include "src/ui/app/app_assembly.hpp"
#include "src/ui/app/inkcell_runtime.hpp"
#include "src/ui/app/model_boot.hpp"
#include "src/ui/bridge/agent_bridge.hpp"
#include "src/ui/chat/prompt_history.hpp"
#include "src/ui/model/inkcell_app_model.hpp"
#include "src/ui/model/workflow_runner.hpp"

namespace cortex::mk3::ui {

inline int runInkcellShell(const InkcellAppConfig &cfg, Agent &agent) {
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    model->setRootAgent(&agent);
    initializeChatModel(model, cfg);
    auto app = makeInkcellApp(cfg, bridge, model, true);
    installCoalescedTick(app, bridge, model);
    SessionFlushGuard flushGuard(agent, cfg.sessionId, cfg.noSession);
    if (snapshotMode()) {
        app.render_to(std::cout, "main", {120, 34});
        flushGuard.finish(model);
        return 0;
    }
    int rc = app.run("main");
    flushGuard.finish(model);
    (void)bridge;
    return rc;
}
inline int runInkcellSmoke(const InkcellAppConfig &cfg, Agent &agent) {
    return runInkcellShell(cfg, agent);
}
inline int runInkcellOneShot(const InkcellAppConfig &cfg, Agent &agent,
                             const std::string &prompt,
                             const std::string &sessionId, bool noSession) {
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    model->setRootAgent(&agent);
    initializeChatModel(model, cfg);
    TimelineRow userRow;
    userRow.kind = TimelineKind::User;
    userRow.title = "you";
    userRow.body = prompt;
    model->pushRow(std::move(userRow));
    agent.setAskToolHandler([&bridge](const Json::Value &params) {
        return bridge.requestAsk(params);
    });
    std::atomic<bool> done{false};
    std::thread worker([&]() {
        runAgentTurn(bridge, agent, prompt, sessionId, noSession, done);
    });
    SessionFlushGuard flushGuard(agent, sessionId, noSession);
    flushGuard.setSessionId(sessionId);

    auto app = makeInkcellApp(cfg, bridge, model, false);
    std::atomic<bool> quitPosted{false};
    installCoalescedTick(
        app, bridge, model,
        [&done, &quitPosted](inkcell::App &app, ShellModel &, AgentBridge &) {
            if (done.load(std::memory_order_acquire) &&
                !quitPosted.exchange(true)) {
                app.engine().post_action(inkcell::Action{"app.quit"});
            }
        });
    int rc = 0;
    if (snapshotMode()) {
        while (!done.load(std::memory_order_acquire)) {
            model->drain(bridge);
            if (model->askActive)
                bridge.cancelAsk();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        model->drain(bridge);
        app.render_to(std::cout, "agent", {120, 34});
    } else {
        rc = app.run("agent");
    }

    if (!done.load(std::memory_order_acquire))
        requestRunStop(RunStopKind::Operator);
    if (worker.joinable())
        worker.join();
    clearRunStop();
    flushGuard.finish(model);
    return rc;
}
inline int runInkcellRepl(const InkcellAppConfig &cfg, Agent &agent,
                          const std::string & /*sessionId*/, bool noSession) {
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    auto slot = std::make_shared<LiveAgentSlot>();
    slot->external = &agent;
    model->setRootAgent(slot->ptr());
    initializeChatModel(model, cfg);
    // Re-assert dev dumps for the live agent (CLI already may have set this).
    if (std::getenv("CORTEX_DEV_MODE") &&
        std::string(std::getenv("CORTEX_DEV_MODE")) != "0" &&
        std::string(std::getenv("CORTEX_DEV_MODE")) != "false") {
        agent.setDevMode(true);
    }
    agent.setAskToolHandler([&bridge](const Json::Value &params) {
        return bridge.requestAsk(params);
    });
    SessionFlushGuard flushGuard(agent, /*cfgSessionId*/ "", noSession);
    std::atomic<bool> workerBusy{false};
    std::atomic<bool> wfBusy{false};
    std::atomic<bool> quitPosted{false};
    std::thread worker;
    std::thread wfWorker;

    auto joinWorker = [&]() {
        if (worker.joinable())
            worker.join();
        workerBusy.store(false, std::memory_order_release);
    };
    auto joinWfWorker = [&]() {
        if (wfWorker.joinable())
            wfWorker.join();
        wfBusy.store(false, std::memory_order_release);
    };

    auto applyLiveIdentity = [&](const AgentConfig &acfg,
                                 const std::string &manifestPath) {
        model->agentName = acfg.name.empty() ? "cortex" : acfg.name;
        model->agentModel = acfg.model;
        model->agentProvider = acfg.provider;
        model->activeManifestPath = manifestPath;
        model->setRootAgent(slot->ptr());
        model->clearTranscript();
        model->dashboard.notice = "launched " + model->agentName;
        model->launchError.clear();
    };

    // Seed -p into the same submit path as a typed Enter.
    if (!cfg.initialPrompt.empty()) {
        model->pendingSubmit = cfg.initialPrompt;
        if (model->promptHistory.empty() ||
            model->promptHistory.back() != cfg.initialPrompt)
            model->promptHistory.push_back(cfg.initialPrompt);
        model->promptHistoryIndex =
            static_cast<int>(model->promptHistory.size());
        model->pushRow({TimelineKind::User, "you", cfg.initialPrompt, true});
    }

    // Seed prompt or resumed session go straight to agent; bare launch opens
    // dashboard.
    bool startAtDashboard = cfg.manifestPath.empty() &&
                            cfg.initialPrompt.empty() && cfg.sessionId.empty();
    auto app = makeInkcellApp(cfg, bridge, model, startAtDashboard);
    // Fullscreen child TUI (art) — suspend/resume alt-screen + force full frame.
    model->suspendTui = [&app]() { app.engine().suspend_for_child(); };
    model->resumeTui = [&app]() { app.engine().resume_after_child(); };
    const bool exitOnDone = cfg.ephemeral;
    // drain + pendingRoute in installCoalescedTick; hub/worker logic in extra
    // (runs first).
    installCoalescedTick(
        app, bridge, model,
        [slot, &workerBusy, &worker, &joinWorker, &wfBusy, &wfWorker,
         &joinWfWorker, &quitPosted, noSession, exitOnDone, applyLiveIdentity,
         &flushGuard](inkcell::App &app, ShellModel &model,
                      AgentBridge &bridge) {
            if (model.pendingStopWorkflow) {
                model.pendingStopWorkflow = false;
                model.workflowRun.requestCancel();
            }

            if (!model.pendingRunWorkflow.empty() &&
                !wfBusy.load(std::memory_order_acquire)) {
                std::string path = model.pendingRunWorkflow;
                model.pendingRunWorkflow.clear();
                joinWfWorker();
                wfBusy.store(true, std::memory_order_release);
                model.dashboard.notice = "workflow running…";
                wfWorker = std::thread([&model, &bridge, path, &wfBusy]() {
                    try {
                        auto result = model::runWorkflowOnHub(
                            path, model.workflowRun, &bridge);
                        if (result.success) {
                            model.dashboard.notice =
                                "workflow ok · " + result.workflowName + " · " +
                                std::to_string(
                                    static_cast<int>(result.elapsedMs)) +
                                "ms";
                        } else if (result.error == "cancelled" ||
                                   model.workflowRun.cancelRequested()) {
                            model.dashboard.notice = "workflow cancelled";
                        } else {
                            model.dashboard.notice =
                                "workflow fail · " + (result.error.empty()
                                                          ? result.workflowName
                                                          : result.error);
                        }
                    } catch (const std::exception &e) {
                        model.workflowRun.fail(e.what());
                        model.dashboard.notice =
                            std::string("workflow exception · ") + e.what();
                    }
                    wfBusy.store(false, std::memory_order_release);
                });
            }

            if (!model.pendingLaunchManifest.empty() &&
                !workerBusy.load(std::memory_order_acquire)) {
                std::string path = model.pendingLaunchManifest;
                model.pendingLaunchManifest.clear();
                // Kind-dispatch: only build an Agent for kind=agent. Other
                // kinds route to their dedicated scene (tool →
                // scenes::ToolScene) or show a notice until the matching scene
                // ships (relic/feed/etc). Workflow has its own path via
                // pendingRunWorkflow above.
                std::string kind = ManifestLoader::detectKind(path);
                if (kind != "agent") {
                    if (kind == "tool") {
                        ToolSchema ts = ManifestLoader::loadToolManifest(path);
                        model.activeToolManifestPath = path;
                        model.activeToolName = ts.name.empty()
                                                   ? std::filesystem::path(path)
                                                         .parent_path()
                                                         .filename()
                                                         .string()
                                                   : ts.name;
                        model.requestRoute(PendingRoute::Tool);
                    } else if (kind == "relic") {
                        model.activeRelicManifestPath = path;
                        model.activeRelicName = std::filesystem::path(path)
                                                    .parent_path()
                                                    .filename()
                                                    .string();
                        model.requestRoute(PendingRoute::Relic);
                    } else if (kind == "workflow") {
                        model.activeWorkflowManifestPath = path;
                        model.activeWorkflowName =
                            std::filesystem::path(path).stem().string();
                        model.requestRoute(PendingRoute::Workflow);
                    } else if (!kind.empty()) {
                        model.dashboard.flashNotice(
                            kind + " page · not implemented yet");
                    } else {
                        model.dashboard.flashNotice(
                            "unrecognized manifest kind");
                    }
                    return; // non-agent kinds handled above
                }
                if (!model.activeManifestPath.empty() &&
                    path == model.activeManifestPath) {
                    model.dashboard.notice =
                        "already live · " + model.agentName;
                    model.requestRoute(PendingRoute::Agent);
                } else if (model.running ||
                           workerBusy.load(std::memory_order_acquire)) {
                    // Live session keeps the worker. Do NOT joinWorker() here —
                    // that was killing bg turns when browsing manifests/settings
                    // and launching something else. Operator must stop first.
                    model.dashboard.flashNotice(
                        "live turn · " + model.agentName +
                        " — stop (x / Ctrl-C) before launching another agent");
                } else {
                    joinWorker();
                    std::string err;
                    auto next = buildAgentFromManifest(path, bridge, err);
                    if (!next) {
                        model.launchError = err.empty() ? "launch failed" : err;
                        model.dashboard.flashNotice("launch failed: " +
                                                    model.launchError);
                    } else {
                        AgentConfig loaded = next->config();
                        slot->owned = std::move(next);
                        applyLiveIdentity(loaded, path);
                        // Hub resume onto different agent: load session history
                        // + ui_timeline after rebuild (see pendingResumeSessionId).
                        if (!model.pendingResumeSessionId.empty()) {
                            const std::string sid = model.pendingResumeSessionId;
                            model.pendingResumeSessionId.clear();
                            try {
                                session::SessionManager sm;
                                if (sm.exists(sid)) {
                                    auto full = sm.load(sid);
                                    slot->get().loadSession(sid);
                                    slot->get().loadStateCheckpoint(sid);
                                    // Prefer session engine over agent.yml primary.
                                    if (!full.provider.empty() && !full.model.empty()) {
                                        auto p = providers::createProvider(
                                            full.provider, full.model);
                                        if (p) {
                                            p->setQuietLogs(true);
                                            slot->get().setProvider(
                                                p, full.provider, full.model);
                                            model.agentProvider = full.provider;
                                            model.agentModel = full.model;
                                        }
                                    }
                                    if (!full.agentName.empty())
                                        model.agentName = full.agentName;
                                    model.loadSessionUi(full);
                                    model.activeSessionId = sid;
                                    model.reannotateDrillable();
                                }
                            } catch (const std::exception& ex) {
                                model.dashboard.flashNotice(
                                    std::string("resume load failed: ") + ex.what());
                            }
                        }
                        flushGuard.rebind(slot->get(), model.activeSessionId);
                        model.requestRoute(PendingRoute::Agent);
                    }
                }
            }

            // /continue — empty prompt, silent history resume (no YOU row).
            if (model.pendingContinue &&
                !workerBusy.load(std::memory_order_acquire)) {
                model.pendingContinue = false;
                model.pendingSubmit.clear();  // continue wins over stale text
                workerBusy.store(true, std::memory_order_release);
                model.running = true;
                model.done = false;
                model.failed = false;
                model.status = "running";
                joinWorker();
                std::string sid = model.activeSessionId;
                worker = std::thread(
                    [slot, &bridge, sid, noSession, &workerBusy]() {
                        std::atomic<bool> done{false};
                        // Empty prompt → agent skips User: push when history lives.
                        runAgentTurn(bridge, slot->get(), std::string(), sid,
                                     noSession, done);
                        while (!done.load(std::memory_order_acquire))
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(5));
                        g_running = true;
                        workerBusy.store(false, std::memory_order_release);
                    });
            } else if (!model.pendingSubmit.empty() &&
                       !workerBusy.load(std::memory_order_acquire)) {
                std::string prompt = model.pendingSubmit;
                model.pendingSubmit.clear();
                chat::savePromptHistory(model.promptHistory);
                workerBusy.store(true, std::memory_order_release);
                model.running = true;
                model.done = false;
                model.failed = false;
                model.status = "running";
                joinWorker();
                std::string sid = model.activeSessionId;
                worker = std::thread(
                    [slot, &bridge, prompt, sid, noSession, &workerBusy]() {
                        std::atomic<bool> done{false};
                        runAgentTurn(bridge, slot->get(), prompt, sid,
                                     noSession, done);
                        while (!done.load(std::memory_order_acquire))
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(5));
                        g_running = true;
                        workerBusy.store(false, std::memory_order_release);
                    });
            }

            if (exitOnDone && model.done && !model.running &&
                !workerBusy.load(std::memory_order_acquire) &&
                !quitPosted.exchange(true)) {
                app.engine().post_action(inkcell::Action{"app.quit"});
            }
        });

    if (snapshotMode()) {
        app.render_to(std::cout, startAtDashboard ? "main" : "agent",
                      {120, 34});
        return 0;
    }

    int rc = app.run(startAtDashboard ? "main" : "agent");
    g_running = false;
    bridge.cancelAsk();
    model->workflowRun.requestCancel();
    joinWorker();
    joinWfWorker();
    chat::savePromptHistory(model->promptHistory);
    clearRunStop();
    // Vet-fix: snapshot UI timeline BEFORE agent history flush so the
    // on-disk session carries the exact live blocks (thoughts/actions/
    // results), not only User/Agent records. Then flush agent history_
    // for LLM continuity. Order matters: timeline first (UI truth),
    // history second (agent memory).
    model->persistUiTimeline();
    flushGuard.rebind(slot->get(), model->activeSessionId);
    flushGuard.finish(model);
    return rc;
}
} // namespace cortex::mk3::ui
