#pragma once
// App assembly: Main control page + Agent/History chat.
// One-way deps: app -> scenes -> views/layout/theme/model -> bridge.

#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "inkcell/app.hpp"
#include "src/ui/bridge/agent_bridge.hpp"
#include "src/ui/model/inkcell_app_model.hpp"
#include "src/ui/scenes/agent_scene.hpp"
#include "src/ui/scenes/main_scene.hpp"

namespace cortex::mk3::ui {

inline void installShellTick(inkcell::App& app, AgentBridge& bridge, const std::shared_ptr<ShellModel>& model) {
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

inline inkcell::App makeAgentShellApp(const InkcellAppConfig& cfg, AgentBridge& bridge,
                                      std::shared_ptr<ShellModel> model, bool startAtMain) {
    inkcell::App app;
    app.tick_ms(33)
        .bind("q", "app.quit", "Quit")
        .bind("up", "scroll.up", "Up")
        .bind("down", "scroll.down", "Down")
        .bind("r", "shell.toggle_raw", "Toggle raw")
        .bind("t", "shell.toggle_thoughts", "Toggle thoughts")
        .bind("i", "shell.focus_composer", "Focus composer")
        .bind("m", "scene.main", "Main menu")
        .bind("esc", "shell.focus_timeline", "Focus timeline / back")
        .route("scene.agent", "agent")
        .route("scene.main", "main")
        .scene<scenes::MainScene>("main", cfg, bridge, model)
        .scene<scenes::AgentScene>("agent", cfg, bridge, model)
        .initial_scene(startAtMain ? "main" : "agent");
    return app;
}

inline void runAgentTurn(AgentBridge& bridge, Agent& agent, const std::string& prompt,
                         const std::string& sessionId, bool ephemeral, std::atomic<bool>& done) {
    try {
        bridge.publish(UiEvent::status("agent running"));
        size_t rawSeen = 0;
        size_t eventSeen = 0;
        auto onToken = [&](const std::string&, bool) {
            const std::string& raw = agent.rawLlOutput();
            if (raw.size() > rawSeen) {
                bridge.publish(UiEvent::token(raw.substr(rawSeen)));
                rawSeen = raw.size();
            }
            const auto& events = agent.protocolEvents();
            while (eventSeen < events.size()) bridge.publish(UiEvent::protocolEvent(events[eventSeen++]));
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

inline int runInkcellShell(const InkcellAppConfig& cfg) {
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    auto app = makeAgentShellApp(cfg, bridge, model, true);
    installShellTick(app, bridge, model);
    if (snapshotMode()) {
        app.render_to(std::cout, "main", {120, 34});
        return 0;
    }
    return app.run("main");
}

inline int runInkcellSmoke(const InkcellAppConfig& cfg) { return runInkcellShell(cfg); }

inline int runInkcellOneShot(const InkcellAppConfig& cfg, Agent& agent, const std::string& prompt,
                             const std::string& sessionId, bool ephemeral) {
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    model->setRootAgent(&agent);
    std::atomic<bool> done{false};
    std::thread worker([&]() { runAgentTurn(bridge, agent, prompt, sessionId, ephemeral, done); });

    auto app = makeAgentShellApp(cfg, bridge, model, false);
    installShellTick(app, bridge, model);
    int rc = 0;
    if (snapshotMode()) {
        while (!done.load(std::memory_order_acquire)) {
            model->drain(bridge);
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

inline int runInkcellRepl(const InkcellAppConfig& cfg, Agent& agent, const std::string& sessionId, bool ephemeral) {
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    model->setRootAgent(&agent);
    std::atomic<bool> workerBusy{false};
    std::thread worker;

    auto joinWorker = [&]() {
        if (worker.joinable()) worker.join();
        workerBusy.store(false, std::memory_order_release);
    };

    bool startAtMain = cfg.manifestPath.empty();
    auto app = makeAgentShellApp(cfg, bridge, model, startAtMain);
    app.engine().input_poll_ms(33).wake_fd(bridge.wakeFd()).on_wake([model, &bridge]() { model->drain(bridge); });
    app.engine().on_tick([model, &bridge, &app, &workerBusy, &worker, &joinWorker, &agent, sessionId, ephemeral](
                             inkcell::Tick) {
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
            workerBusy.store(true, std::memory_order_release);
            model->running = true;
            model->done = false;
            model->failed = false;
            model->status = "running";
            joinWorker();
            worker = std::thread([&, prompt]() {
                std::atomic<bool> done{false};
                runAgentTurn(bridge, agent, prompt, sessionId, ephemeral, done);
                while (!done.load(std::memory_order_acquire))
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                workerBusy.store(false, std::memory_order_release);
            });
        }
    });

    if (snapshotMode()) {
        app.render_to(std::cout, startAtMain ? "main" : "agent", {120, 34});
        return 0;
    }

    int rc = app.run(startAtMain ? "main" : "agent");
    g_running = false;
    joinWorker();
    g_running = true;
    return rc;
}

}  // namespace cortex::mk3::ui
