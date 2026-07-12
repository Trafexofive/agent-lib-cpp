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
#include "src/session/manager.hpp"
#include "src/ui/bridge/agent_bridge.hpp"
#include "src/ui/chat/prompt_history.hpp"
#include "src/ui/model/inkcell_app_model.hpp"
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

inline void publishProtocolChanges(AgentBridge& bridge, const std::vector<ProtocolEvent>& current,
                                   std::vector<ProtocolEvent>& previous) {
    for (size_t i = 0; i < current.size(); ++i) {
        if (i >= previous.size() || !sameProtocolEvent(current[i], previous[i]))
            bridge.publish(UiEvent::protocolEvent(current[i], i));
    }
    previous = current;
}

inline void initializeChatModel(const std::shared_ptr<ShellModel>& model,
                               const InkcellAppConfig& cfg) {
    if (const char* requestedTheme = std::getenv("MK3_TUI_THEME")) {
        std::string value = requestedTheme;
        if (value == "neon") theme::set(theme::Variant::Neon);
        else theme::set(theme::Variant::Graphite);
    } else {
        theme::set(theme::Variant::Graphite);
    }
    model->activeSessionId = cfg.sessionId;
    model->dashboard.refreshSessions();
    model->promptHistory = chat::loadPromptHistory();
    model->promptHistoryIndex = static_cast<int>(model->promptHistory.size());
    if (!cfg.sessionId.empty()) {
        session::SessionManager sessions;
        if (sessions.exists(cfg.sessionId)) model->loadSessionRecords(sessions.load(cfg.sessionId).records);
    }
}

inline void installAppTick(inkcell::App& app, AgentBridge& bridge, const std::shared_ptr<ShellModel>& model) {
    app.engine().input_poll_ms(33).wake_fd(bridge.wakeFd()).on_wake([model, &bridge]() { model->drain(bridge); });
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
        auto onToken = [&](const std::string&, bool) {
            const std::string& raw = agent.rawLlOutput();
            if (raw.size() > rawSeen) {
                bridge.publish(UiEvent::token(raw.substr(rawSeen)));
                rawSeen = raw.size();
            }
            publishProtocolChanges(bridge, agent.protocolEvents(), previousEvents);
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

inline int runInkcellOneShot(const InkcellAppConfig& cfg, Agent& agent, const std::string& prompt,
                             const std::string& sessionId, bool ephemeral) {
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
    std::thread worker([&]() { runAgentTurn(bridge, agent, prompt, sessionId, ephemeral, done); });

    auto app = makeInkcellApp(cfg, bridge, model, false);
    installAppTick(app, bridge, model);
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

inline int runInkcellRepl(const InkcellAppConfig& cfg, Agent& agent, const std::string& /*sessionId*/, bool ephemeral) {
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    model->setRootAgent(&agent);
    initializeChatModel(model, cfg);
    agent.setAskToolHandler([&bridge](const Json::Value& params) { return bridge.requestAsk(params); });
    std::atomic<bool> workerBusy{false};
    std::thread worker;

    auto joinWorker = [&]() {
        if (worker.joinable()) worker.join();
        workerBusy.store(false, std::memory_order_release);
    };

    bool startAtDashboard = cfg.manifestPath.empty();
    auto app = makeInkcellApp(cfg, bridge, model, startAtDashboard);
    app.engine().input_poll_ms(33).wake_fd(bridge.wakeFd()).on_wake([model, &bridge]() { model->drain(bridge); });
    app.engine().on_tick([model, &bridge, &app, &workerBusy, &worker, &joinWorker, &agent, ephemeral](inkcell::Tick) {
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
            worker = std::thread([&, prompt]() {
                std::atomic<bool> done{false};
                runAgentTurn(bridge, agent, prompt, model->activeSessionId, ephemeral, done);
                while (!done.load(std::memory_order_acquire))
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                g_running = true;
                workerBusy.store(false, std::memory_order_release);
            });
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
