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
#include <thread>

#include "inkcell/app.hpp"
#include "src/core/manifest_loader.hpp"
#include "src/providers/factory.hpp"
#include "src/session/manager.hpp"
#include "src/ui/bridge/agent_bridge.hpp"
#include "src/ui/chat/prompt_history.hpp"
#include "src/ui/model/inkcell_app_model.hpp"
#include "src/ui/model/ui_prefs.hpp"
#include "src/ui/scenes/agent_scene.hpp"
#include "src/ui/scenes/main_scene.hpp"

namespace cortex::mk3::ui {

inline bool sameProtocolEvent(const ProtocolEvent& a, const ProtocolEvent& b) {
    if (a.kind != b.kind || a.text != b.text) return false;
    if (a.kind == ProtocolEventKind::ACTION) {
        return a.action.type == b.action.type && a.action.name == b.action.name &&
               a.action.id == b.action.id && a.action.body == b.action.body &&
               a.action.mode == b.action.mode && a.action.modifiers == b.action.modifiers;
    }
    if (a.kind == ProtocolEventKind::RESULT) {
        return a.result.id == b.result.id && a.result.ok == b.result.ok &&
               a.result.summary == b.result.summary && a.result.toolName == b.result.toolName &&
               a.result.exitCode == b.result.exitCode && a.result.elapsedMs == b.result.elapsedMs &&
               a.result.outputBytes == b.result.outputBytes;
    }
    return true;
}

inline void collectProtocolChanges(std::vector<UiEvent>& out,
                                   const std::vector<ProtocolEvent>& current,
                                   std::vector<ProtocolEvent>& previous) {
    size_t previousSize = previous.size();
    if (previous.size() < current.size()) previous.resize(current.size());
    for (size_t i = 0; i < current.size(); ++i) {
        if (i >= previousSize || !sameProtocolEvent(current[i], previous[i])) {
            out.push_back(UiEvent::protocolEvent(current[i], i));
            previous[i] = current[i];
        }
    }
    if (previous.size() > current.size()) previous.resize(current.size());
}

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
    model->activeSessionId = cfg.sessionId;
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
    if (!cfg.sessionId.empty()) {
        session::SessionManager sessions;
        if (sessions.exists(cfg.sessionId)) model->loadSessionRecords(sessions.load(cfg.sessionId).records);
    }
}

inline void installAppTick(inkcell::App& app, AgentBridge& bridge, const std::shared_ptr<ShellModel>& model) {
    // on_wake is a no-op: the engine already drains the eventfd (engine.hpp
    // ~line 300). We do NOT drain the UiEvent queue here — that would run
    // rebuildViews+redraw on EVERY token (often 50-100/sec), a wake-storm
    // that made the TUI feel unusably slow. Instead on_tick (30fps, below)
    // does the single drain+rebuild per frame, coalescing all tokens that
    // arrived in the last 33ms into one redraw. Input stays responsive
    // because select still wakes immediately on keypresses.
    app.engine().input_poll_ms(33).wake_fd(bridge.wakeFd()).on_wake([]() {});
    app.engine().on_tick([model, &bridge, &app](inkcell::Tick) {
        model->drain(bridge);
        if (model->pendingRoute == "agent") {
            model->pendingRoute.clear();
            app.engine().post_action(inkcell::Action{"scene.agent"});
        } else if (model->pendingRoute == "main") {
            model->pendingRoute.clear();
            app.engine().post_action(inkcell::Action{"scene.main"});
        } else if (model->pendingRoute == "quit") {
            model->pendingRoute.clear();
            app.engine().post_action(inkcell::Action{"app.quit"});
        }
    });
}

inline inkcell::App makeInkcellApp(const InkcellAppConfig& cfg, AgentBridge& bridge,
                                   std::shared_ptr<ShellModel> model, bool startAtDashboard) {
    inkcell::App app;
    app.tick_ms(33)
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
        .scene<scenes::MainScene>("main", cfg, bridge, model)
        .scene<scenes::AgentScene>("agent", cfg, bridge, model)
        .initial_scene(startAtDashboard ? "main" : "agent");
    return app;
}

inline void runAgentTurn(AgentBridge& bridge, Agent& agent, const std::string& prompt,
                         const std::string& sessionId, bool ephemeral, std::atomic<bool>& done) {
    try {
        bridge.publish(UiEvent::status("agent running"));
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
    installAppTick(app, bridge, model);
    if (snapshotMode()) {
        app.render_to(std::cout, "main", {120, 34});
        return 0;
    }
    return app.run("main");
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

    auto app = makeInkcellApp(cfg, bridge, model, false);
    std::atomic<bool> quitPosted{false};
    app.engine().input_poll_ms(33).wake_fd(bridge.wakeFd()).on_wake([]() {});
    app.engine().on_tick([model, &bridge, &app, &done, &quitPosted](inkcell::Tick) {
        model->drain(bridge);
        if (model->pendingRoute == "quit") {
            model->pendingRoute.clear();
            app.engine().post_action(inkcell::Action{"app.quit"});
        }
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
    std::atomic<bool> workerBusy{false};
    std::atomic<bool> quitPosted{false};
    std::thread worker;

    auto joinWorker = [&]() {
        if (worker.joinable()) worker.join();
        workerBusy.store(false, std::memory_order_release);
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
    app.engine().input_poll_ms(33).wake_fd(bridge.wakeFd()).on_wake([]() {});
    const bool exitOnDone = cfg.ephemeral;
    app.engine().on_tick([model, slot, &bridge, &app, &workerBusy, &worker, &joinWorker, &quitPosted,
                          noSession, exitOnDone, applyLiveIdentity](inkcell::Tick) {
        model->drain(bridge);

        // Hub launch: hot-swap agent from selected manifest, then open chat.
        if (!model->pendingLaunchManifest.empty() &&
            !workerBusy.load(std::memory_order_acquire)) {
            std::string path = model->pendingLaunchManifest;
            model->pendingLaunchManifest.clear();
            // Same manifest already live → just open chat.
            if (!model->activeManifestPath.empty() && path == model->activeManifestPath) {
                model->dashboard.notice = "already live · " + model->agentName;
                model->pendingRoute = "agent";
            } else {
                joinWorker();
                std::string err;
                auto next = buildAgentFromManifest(path, bridge, err);
                if (!next) {
                    model->launchError = err.empty() ? "launch failed" : err;
                    model->dashboard.notice = "launch failed: " + model->launchError;
                } else {
                    AgentConfig loaded = next->config();
                    slot->owned = std::move(next);
                    applyLiveIdentity(loaded, path);
                    model->pendingRoute = "agent";
                }
            }
        }

        if (model->pendingRoute == "agent") {
            model->pendingRoute.clear();
            app.engine().post_action(inkcell::Action{"scene.agent"});
        } else if (model->pendingRoute == "main") {
            model->pendingRoute.clear();
            app.engine().post_action(inkcell::Action{"scene.main"});
        } else if (model->pendingRoute == "quit") {
            model->pendingRoute.clear();
            app.engine().post_action(inkcell::Action{"app.quit"});
        }
        if (!model->pendingSubmit.empty() && !workerBusy.load(std::memory_order_acquire)) {
            std::string prompt = model->pendingSubmit;
            model->pendingSubmit.clear();
            chat::savePromptHistory(model->promptHistory);
            workerBusy.store(true, std::memory_order_release);
            model->running = true;
            model->done = false;
            model->failed = false;
            model->status = "running";
            joinWorker();
            worker = std::thread([slot, &bridge, model, prompt, noSession, &workerBusy]() {
                std::atomic<bool> done{false};
                runAgentTurn(bridge, slot->get(), prompt, model->activeSessionId, noSession, done);
                while (!done.load(std::memory_order_acquire))
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                g_running = true;
                workerBusy.store(false, std::memory_order_release);
            });
        }
        // --ephemeral: leave once a turn has finished and the worker is idle.
        if (exitOnDone && model->done && !model->running &&
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
    joinWorker();
    chat::savePromptHistory(model->promptHistory);
    g_running = true;
    return rc;
}

}  // namespace cortex::mk3::ui
