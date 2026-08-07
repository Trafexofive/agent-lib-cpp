#pragma once
// inkcell::App assembly: command registry (chat + hub) + key bindings + scene
// registration. The lone scene-to-app wiring point; scenes stay self-contained.

#include "inkcell/app.hpp"
#include "src/ui/bridge/agent_bridge.hpp"
#include "src/ui/model/inkcell_app_model.hpp"
#include "src/ui/model/inkcell_commands.hpp"
#include "src/ui/scenes/agent_scene.hpp"
#include "src/ui/scenes/main_scene.hpp"
#include "src/ui/scenes/relic_scene.hpp"
#include "src/ui/scenes/tool_scene.hpp"
#include "src/ui/scenes/workflow_scene.hpp"

namespace cortex::mk3::ui {

inline inkcell::App makeInkcellApp(const InkcellAppConfig &cfg,
                                   AgentBridge &bridge,
                                   std::shared_ptr<ShellModel> model,
                                   bool startAtDashboard) {
    // Dogfood inkcell CommandRegistry: chat + hub inventories (help overlay
    // reads chat). IMPORTANT: bind hub registry to a named temporary —
    // range-for over hubCommandRegistry().all() dangles (temporary
    // CommandRegistry dies before loop body).
    inkcell::CommandRegistry commands = chatCommandRegistry();
    {
        inkcell::CommandRegistry hub = hubCommandRegistry();
        for (const auto &c : hub.all())
            commands.add(c);
    }

    inkcell::App app;
    app.tick_ms(33)
        .commands(std::move(commands))
        .bind("q", "app.quit", "Quit")
        .bind("up", "scroll.up", "Up")
        .bind("down", "scroll.down", "Down")
        .bind("r", "shell.toggle_raw", "Toggle raw")
        .bind("t", "shell.toggle_thoughts", "Toggle thoughts")
        // Ctrl chords are handled in AgentScene::on_key so they work while
        // typing:
        //   Ctrl-T thoughts · Ctrl-O truncate · Ctrl-R raw · Ctrl-X stop
        //   Ctrl-J/K fine transcript scroll (±1 line; Home/End jump ends)
        .bind("i", "shell.focus_composer", "Focus composer")
        .bind("m", "scene.main", "Dashboard")
        .bind("esc", "shell.focus_timeline", "Focus history")
        .route("scene.agent", "agent")
        .route("scene.main", "main")
        .route("scene.tool", "tool")
        .route("scene.relic", "relic")
        .route("scene.workflow", "workflow")
        .scene<scenes::MainScene>("main", cfg, bridge, model)
        .scene<scenes::AgentScene>("agent", cfg, bridge, model)
        .scene<scenes::ToolScene>("tool", cfg, bridge, model)
        .scene<scenes::RelicScene>("relic", cfg, bridge, model)
        .scene<scenes::WorkflowScene>("workflow", cfg, bridge, model)
        .initial_scene(startAtDashboard ? "main" : "agent");
    return app;
}
} // namespace cortex::mk3::ui
