#pragma once
// App assembly: chat-only inkcell workbench.
// No main menu. No dashboards. This file wires AgentBridge + ShellModel +
// AgentScene; chat rendering lives in src/ui/chat and engine code stays in inkcell.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <mutex>
#include <thread>

#include "inkcell/app.hpp"
#include "src/core/manifest_loader.hpp"
#include "src/providers/factory.hpp"
#include "src/session/manager.hpp"
#include "src/session/controller.hpp"
#include "src/ui/bridge/agent_bridge.hpp"
#include "src/ui/chat/prompt_history.hpp"
#include "src/ui/model/inkcell_app_model.hpp"
#include "src/ui/app/inkcell_runtime.hpp"
#include "src/ui/model/inkcell_commands.hpp"
#include "src/ui/model/protocol_event_diff.hpp"
#include "src/ui/model/ui_prefs.hpp"
#include "src/ui/model/workflow_runner.hpp"
#include "src/ui/scenes/agent_scene.hpp"
#include "src/ui/scenes/main_scene.hpp"
#include "src/ui/scenes/tool_scene.hpp"

namespace cortex::mk3::ui {

inline void initializeChatModel(const std::shared_ptr<ShellModel>& model,
                               const InkcellAppConfig& cfg) {
    // Disk prefs first; MK3_TUI_THEME env still wins when set.
    loadUiPrefs();
    if (const char* requestedTheme = std::getenv("MK3_TUI_THEME")) {
        std::string value = requestedTheme;
        if (value == "neon") theme::set(theme::Variant::Neon);
        else theme::set(theme::Variant::Graphite);
    }
    applyUiPrefsToModel(*model);
    // Launch-time CWD policy: rememberLastCwd=ON → chdir to persisted path;
    // OFF (default) → drop the persisted value, use launch dir as process CWD.
    applyLaunchCwd(*model);
    // Capture launch CWD before any chdir above (or after, if
    // rememberLastCwd was OFF). Used by the Settings CWD cycle.
    {
        char buf[1024] = {0};
        if (::getcwd(buf, sizeof(buf) - 1)) model->launchCwd = buf;
    }
    model->activeSessionId = cfg.sessionId;
    // Single active id: seed process-wide SessionRef (kills dual-flush).
    // only --no-session suppresses disk; --ephemeral is exit-on-done (orthogonal).
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
    model->dashboard.refreshAll();
    model->promptHistory = chat::loadPromptHistory();
    model->promptHistoryIndex = static_cast<int>(model->promptHistory.size());
    // Unified resume: prefer ui_timeline, fall back to records (session audit S0.3).
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
                const auto& c = model->rootAgent->config();
                if (!c.name.empty()) model->agentName = c.name;
                if (!c.model.empty()) model->agentModel = c.model;
                if (!c.provider.empty()) model->agentProvider = c.provider;
            }
        }
    }
}

inline inkcell::App makeInkcellApp(const InkcellAppConfig& cfg, AgentBridge& bridge,
                                   std::shared_ptr<ShellModel> model, bool startAtDashboard) {
    // Dogfood inkcell CommandRegistry: chat + hub inventories (help overlay reads chat).
    // IMPORTANT: bind hub registry to a named temporary — range-for over
    // hubCommandRegistry().all() dangles (temporary CommandRegistry dies before loop body).
    inkcell::CommandRegistry commands = chatCommandRegistry();
    {
        inkcell::CommandRegistry hub = hubCommandRegistry();
        for (const auto& c : hub.all()) commands.add(c);
    }

    inkcell::App app;
    app.tick_ms(33)
        .commands(std::move(commands))
        .bind("q", "app.quit", "Quit")
        .bind("up", "scroll.up", "Up")
        .bind("down", "scroll.down", "Down")
        .bind("r", "shell.toggle_raw", "Toggle raw")
        .bind("t", "shell.toggle_thoughts", "Toggle thoughts")
        // Ctrl chords are handled in AgentScene::on_key so they work while typing:
        //   Ctrl-T thoughts · Ctrl-O truncate · Ctrl-R raw · Ctrl-X stop
        .bind("i", "shell.focus_composer", "Focus composer")
        .bind("m", "scene.main", "Dashboard")
        .bind("esc", "shell.focus_timeline", "Focus history")
        .route("scene.agent", "agent")
        .route("scene.main", "main")
        .route("scene.tool", "tool")
        .scene<scenes::MainScene>("main", cfg, bridge, model)
        .scene<scenes::AgentScene>("agent", cfg, bridge, model)
        .scene<scenes::ToolScene>("tool", cfg, bridge, model)
        .initial_scene(startAtDashboard ? "main" : "agent");
    return app;
}

inline void runAgentTurn(AgentBridge& bridge, Agent& agent, const std::string& prompt,
                         const std::string& sessionId, bool ephemeral, std::atomic<bool>& done) {
    try {
        bridge.publish(UiEvent::status("agent running"));
        // Vet-fix: pipe retry signals into a Notification event. Provider
        // suppresses stderr once quiet logs are on, so the chat chrome stays
        // clean during transient upstream flakes.
        {
            // Local provider fetch — captures by reference below; the ptr is
            // only used at install time.
            auto provider = agent.provider();
            if (provider) provider->setQuietLogs(true);
        }
        agent.setRetryHandler([&bridge, &agent, sessionId](const RetrySignal& rs) {
            std::string source = rs.kind == RetrySignal::Kind::Http ? "http" : "network";
            std::string detail = rs.kind == RetrySignal::Kind::Http
                                     ? std::string("HTTP ") + std::to_string(rs.httpStatus)
                                     : rs.curlError;
            std::string title = source + " retry · " + detail;
            std::string fmt =
                std::string("attempt ") + std::to_string(rs.attempt) + "/" +
                std::to_string(rs.maxAttempts) + " · " +
                std::to_string(rs.backoffMs / 1000) + "s";
            // Stable id per (provider, turn) so retries collapse to one badge.
            std::string id = std::string("retry:") + agent.name() + ":" + sessionId;
            bridge.publish(UiEvent::notification(source, "warn", title + " — " + fmt,
                                                  rs.attempt, rs.maxAttempts, id));
        });
        size_t rawSeen = 0;
        std::vector<ProtocolEvent> previousEvents;
        auto lastUiFlush = std::chrono::steady_clock::now() - std::chrono::milliseconds(16);
        auto onToken = [&](const std::string& token, bool finalChunk) {
            // Stream-as-fast-as-parse: never delay a closed (or provisional)
            // protocol event. Only coalesce pure byte heartbeats (~60fps).
            const auto& cur = agent.protocolEvents();
            bool protocolDirty = cur.size() != previousEvents.size();
            if (!protocolDirty) {
                for (size_t i = 0; i < cur.size(); ++i) {
                    if (i >= previousEvents.size() || !sameProtocolEvent(cur[i], previousEvents[i])) {
                        protocolDirty = true;
                        break;
                    }
                }
            }
            auto now = std::chrono::steady_clock::now();
            if (!finalChunk && !protocolDirty &&
                now - lastUiFlush < std::chrono::milliseconds(16))
                return;
            lastUiFlush = now;
            std::vector<UiEvent> batch;
            // Forwarded child tokens (non-empty) publish directly so the UI
            // stays alive during a synchronous sub-agent call — the child's
            // bytes don't appear in the parent's rawLlOutput, so the delta
            // read below wouldn't see them. For the parent's own stream,
            // 'token' is empty (content lands in rawLlOutput) and the delta
            // read handles it.
            if (!token.empty()) {
                batch.push_back(UiEvent::token(token));
            }
            const std::string& raw = agent.rawLlOutput();
            if (raw.size() > rawSeen) {
                batch.push_back(UiEvent::token(raw.substr(rawSeen)));
                rawSeen = raw.size();
            }
            collectProtocolChanges(batch, agent.protocolEvents(), previousEvents);
            if (!batch.empty())
                bridge.publishMany(std::move(batch));
        };
        std::string result = agent.prompt(prompt, onToken, sessionId, ephemeral);
        onToken("", true);
        UiEvent end;
        end.kind = UiEventKind::TurnDone;
        end.text = result;
        bridge.publish(std::move(end));
    } catch (const std::exception& e) {
        bridge.publish(UiEvent::error(e.what()));
        UiEvent end;
        end.kind = UiEventKind::TurnDone;
        end.text = std::string("error: ") + e.what();
        bridge.publish(std::move(end));
    }
    done.store(true, std::memory_order_release);
}

inline int runInkcellShell(const InkcellAppConfig& cfg, Agent& agent) {
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

inline int runInkcellSmoke(const InkcellAppConfig& cfg, Agent& agent) { return runInkcellShell(cfg, agent); }

// --ephemeral + -p: run one turn and exit when finished.
// sessionId / noSession control persistence; exit is unconditional here.
inline int runInkcellOneShot(const InkcellAppConfig& cfg, Agent& agent, const std::string& prompt,
                             const std::string& sessionId, bool noSession) {
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    model->setRootAgent(&agent);
    initializeChatModel(model, cfg);
    TimelineRow userRow;
    userRow.kind = TimelineKind::User;
    userRow.title = "you";
    userRow.body = prompt;
    model->pushRow(std::move(userRow));
    agent.setAskToolHandler([&bridge](const Json::Value& params) { return bridge.requestAsk(params); });
    std::atomic<bool> done{false};
    std::thread worker([&]() { runAgentTurn(bridge, agent, prompt, sessionId, noSession, done); });
    SessionFlushGuard flushGuard(agent, sessionId, noSession);
    flushGuard.setSessionId(sessionId);

    auto app = makeInkcellApp(cfg, bridge, model, false);
    std::atomic<bool> quitPosted{false};
    installCoalescedTick(app, bridge, model,
        [&done, &quitPosted](inkcell::App& app, ShellModel&, AgentBridge&) {
            if (done.load(std::memory_order_acquire) && !quitPosted.exchange(true)) {
                app.engine().post_action(inkcell::Action{"app.quit"});
            }
        });
    int rc = 0;
    if (snapshotMode()) {
        while (!done.load(std::memory_order_acquire)) {
            model->drain(bridge);
            if (model->askActive) bridge.cancelAsk();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        model->drain(bridge);
        app.render_to(std::cout, "agent", {120, 34});
    } else {
        rc = app.run("agent");
    }

    if (!done.load(std::memory_order_acquire)) g_running = false;
    if (worker.joinable()) worker.join();
    g_running = true;
    flushGuard.finish(model);
    return rc;
}

// Live agent slot — starts as external ref from main(); hub launch may replace
// with an owned Agent built from a selected manifest (hot-swap).
struct LiveAgentSlot {
    Agent* external = nullptr;
    std::unique_ptr<Agent> owned;
    Agent& get() { return owned ? *owned : *external; }
    Agent* ptr() { return owned ? owned.get() : external; }
};

// Build a fully wired Agent from agent.yml. Returns nullptr + err on failure.
inline std::unique_ptr<Agent> buildAgentFromManifest(const std::string& manifestPath,
                                                     AgentBridge& bridge, std::string& err) {
    try {
        auto acfg = ManifestLoader::loadAgentConfig(manifestPath);
        ManifestLoader::loadEnv(manifestPath, acfg);
        if (acfg.name.empty()) {
            err = "manifest has no name: " + manifestPath;
            return nullptr;
        }
        auto provider = providers::createProvider(acfg.provider, acfg.model);
        if (!provider) {
            err = "provider unavailable: " + acfg.provider + "/" + acfg.model;
            return nullptr;
        }
        auto agent = std::make_unique<Agent>(acfg, provider);
        ManifestLoader::loadFeeds(manifestPath, *agent);
        ManifestLoader::loadRelics(manifestPath, *agent);
        ManifestLoader::loadTools(manifestPath, *agent);
        ManifestLoader::loadSubAgents(manifestPath, *agent, acfg.provider);
        ManifestLoader::loadWorkflows(manifestPath);
        agent->setAskToolHandler([&bridge](const Json::Value& params) {
            return bridge.requestAsk(params);
        });
        return agent;
    } catch (const std::exception& e) {
        err = e.what();
        return nullptr;
    }
}

// Interactive REPL. noSession → agent.prompt won't persist.
// cfg.ephemeral → quit after a completed agent turn.
// cfg.initialPrompt → auto-submit a seed prompt (-p without --ephemeral).
// Hub Enter on a launchable agent sets model->pendingLaunchManifest → hot-swap.
inline int runInkcellRepl(const InkcellAppConfig& cfg, Agent& agent, const std::string& /*sessionId*/, bool noSession) {
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    auto slot = std::make_shared<LiveAgentSlot>();
    slot->external = &agent;
    model->setRootAgent(slot->ptr());
    initializeChatModel(model, cfg);
    agent.setAskToolHandler([&bridge](const Json::Value& params) { return bridge.requestAsk(params); });
    SessionFlushGuard flushGuard(agent, /*cfgSessionId*/ "", noSession);
    std::atomic<bool> workerBusy{false};
    std::atomic<bool> wfBusy{false};
    std::atomic<bool> quitPosted{false};
    std::thread worker;
    std::thread wfWorker;

    auto joinWorker = [&]() {
        if (worker.joinable()) worker.join();
        workerBusy.store(false, std::memory_order_release);
    };
    auto joinWfWorker = [&]() {
        if (wfWorker.joinable()) wfWorker.join();
        wfBusy.store(false, std::memory_order_release);
    };

    auto applyLiveIdentity = [&](const AgentConfig& acfg, const std::string& manifestPath) {
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
        if (model->promptHistory.empty() || model->promptHistory.back() != cfg.initialPrompt)
            model->promptHistory.push_back(cfg.initialPrompt);
        model->promptHistoryIndex = static_cast<int>(model->promptHistory.size());
        model->pushRow({TimelineKind::User, "you", cfg.initialPrompt, true});
    }

    // With a seed prompt go straight to agent; bare launch still opens dashboard.
    bool startAtDashboard = cfg.manifestPath.empty() && cfg.initialPrompt.empty();
    auto app = makeInkcellApp(cfg, bridge, model, startAtDashboard);
    const bool exitOnDone = cfg.ephemeral;
    // drain + pendingRoute in installCoalescedTick; hub/worker logic in extra (runs first).
    installCoalescedTick(
        app, bridge, model,
        [slot, &workerBusy, &worker, &joinWorker, &wfBusy, &wfWorker, &joinWfWorker, &quitPosted,
         noSession, exitOnDone, applyLiveIdentity, &flushGuard](inkcell::App& app, ShellModel& model,
                                                               AgentBridge& bridge) {
        if (model.pendingStopWorkflow) {
            model.pendingStopWorkflow = false;
            model.workflowRun.requestCancel();
        }

        if (!model.pendingRunWorkflow.empty() && !wfBusy.load(std::memory_order_acquire)) {
            std::string path = model.pendingRunWorkflow;
            model.pendingRunWorkflow.clear();
            joinWfWorker();
            wfBusy.store(true, std::memory_order_release);
            model.dashboard.notice = "workflow running…";
            wfWorker = std::thread([&model, &bridge, path, &wfBusy]() {
                try {
                    auto result = model::runWorkflowOnHub(path, model.workflowRun, &bridge);
                    if (result.success) {
                        model.dashboard.notice =
                            "workflow ok · " + result.workflowName + " · " +
                            std::to_string(static_cast<int>(result.elapsedMs)) + "ms";
                    } else if (result.error == "cancelled" || model.workflowRun.cancelRequested()) {
                        model.dashboard.notice = "workflow cancelled";
                    } else {
                        model.dashboard.notice =
                            "workflow fail · " +
                            (result.error.empty() ? result.workflowName : result.error);
                    }
                } catch (const std::exception& e) {
                    model.workflowRun.fail(e.what());
                    model.dashboard.notice = std::string("workflow exception · ") + e.what();
                }
                wfBusy.store(false, std::memory_order_release);
            });
        }

        if (!model.pendingLaunchManifest.empty() && !workerBusy.load(std::memory_order_acquire)) {
            std::string path = model.pendingLaunchManifest;
            model.pendingLaunchManifest.clear();
            // Kind-dispatch: only build an Agent for kind=agent. Other kinds
            // route to their dedicated scene (tool → scenes::ToolScene) or
            // show a notice until the matching scene ships (relic/feed/etc).
            // Workflow has its own path via pendingRunWorkflow above.
            std::string kind = ManifestLoader::detectKind(path);
            if (kind != "agent") {
                if (kind == "tool") {
                    // Set the active tool state on ShellModel so ToolScene
                    // on_enter can find it, then route.
                    ToolSchema ts = ManifestLoader::loadToolManifest(path);
                    model.activeToolManifestPath = path;
                    model.activeToolName = ts.name.empty() ? std::filesystem::path(path).parent_path().filename().string() : ts.name;
                    model.requestRoute(PendingRoute::Tool);
                } else if (kind == "workflow") {
                    // Should not normally happen (workflow uses pendingRunWorkflow)
                    // but harmless: route to main with a notice.
                    model.dashboard.flashNotice("workflow · use the canvas to run");
                } else if (!kind.empty()) {
                    model.dashboard.flashNotice(kind + " page · not implemented yet");
                } else {
                    model.dashboard.flashNotice("unrecognized manifest kind");
                }
                // Skip the agent build path — handled above per-kind.
                return;  // tail of the lambda for this tick
            }
            if (!model.activeManifestPath.empty() && path == model.activeManifestPath) {
                model.dashboard.notice = "already live · " + model.agentName;
                model.requestRoute(PendingRoute::Agent);
            } else {
                joinWorker();
                std::string err;
                auto next = buildAgentFromManifest(path, bridge, err);
                if (!next) {
                    model.launchError = err.empty() ? "launch failed" : err;
                    model.dashboard.flashNotice("launch failed: " + model.launchError);
                } else {
                    AgentConfig loaded = next->config();
                    slot->owned = std::move(next);
                    applyLiveIdentity(loaded, path);
                    flushGuard.rebind(slot->get(), model.activeSessionId);
                    model.requestRoute(PendingRoute::Agent);
                }
            }
        }

        if (!model.pendingSubmit.empty() && !workerBusy.load(std::memory_order_acquire)) {
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
            worker = std::thread([slot, &bridge, prompt, sid, noSession, &workerBusy]() {
                std::atomic<bool> done{false};
                runAgentTurn(bridge, slot->get(), prompt, sid, noSession, done);
                while (!done.load(std::memory_order_acquire))
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                g_running = true;
                workerBusy.store(false, std::memory_order_release);
            });
        }

        if (exitOnDone && model.done && !model.running &&
            !workerBusy.load(std::memory_order_acquire) && !quitPosted.exchange(true)) {
            app.engine().post_action(inkcell::Action{"app.quit"});
        }
    });

    if (snapshotMode()) {
        app.render_to(std::cout, startAtDashboard ? "main" : "agent", {120, 34});
        return 0;
    }

    int rc = app.run(startAtDashboard ? "main" : "agent");
    g_running = false;
    bridge.cancelAsk();
    model->workflowRun.requestCancel();
    joinWorker();
    joinWfWorker();
    chat::savePromptHistory(model->promptHistory);
    g_running = true;
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

}  // namespace cortex::mk3::ui
