// voice-console — an inkcell TUI that ties the voice pipeline together:
// live state from voice.py (wake→STT→harness→TTS) + a prompt box that runs
// headless cortex-mk3 turns and streams the rendered blocks.
//
//   --live                    run the interactive TUI
//   --snapshot [path]         render one frame (ANSI) to path or stdout
//   (default)                 snapshot to stdout
//
// Env overrides: CORTEX_BIN, VOICE_MANIFEST, VOICE_STATE.

#include <cstdio>
#include <iostream>
#include <string>

#include "inkcell/inkcell.hpp"
#include "scene.hpp"
#include "model.hpp"

namespace {

inline vc::AppConfig config_from_env() {
    vc::AppConfig c;
    if (const char* e = std::getenv("CORTEX_BIN"))
        c.bin = e;
    else
        c.bin = "../../cortex-mk3";
    if (const char* e = std::getenv("VOICE_MANIFEST"))
        c.manifest = e;
    else
        c.manifest =
            "../../playground/local-transcription/manifests/agents/voice/agent.yml";
    if (const char* e = std::getenv("VOICE_STATE"))
        c.state_path = e;
    else
        c.state_path = "/tmp/voice_console_state.json";
    return c;
}

inline inkcell::CommandRegistry command_registry() {
    return inkcell::CommandRegistry{}
        .add({"app.quit", "Quit", "app", "Exit cleanly", "q", true, true, {"app"}})
        .add({"voice.clear.prompt", "Clear Prompt", "voice", "Clear the prompt box", "esc",
              true, true, {"input"}});
}

inline inkcell::KeyMap keymap() {
    inkcell::KeyMap keys;
    inkcell::bind_command_defaults(keys, command_registry());
    keys.bind("ctrl-c", "app.quit", "Quit");
    keys.bind("escape", "app.quit", "Quit");  // when the prompt is empty
    return keys;
}

inline inkcell::App build_app() {
    inkcell::App app;
    app.theme(inkcell::Theme::deep_space())
        .commands(command_registry())
        .keymap(keymap())
        .tick_ms(100)
        .scene<vc::VoiceConsoleScene>("voice", config_from_env())
        .initial_scene("voice");
    return app;
}

inline int snapshot(const std::string& out) {
    std::string body;
    {
        inkcell::App app = build_app();
        app.render_to(std::cout, "voice", {118, 32});
    }
    (void)out;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--live") {
            inkcell::App app = build_app();
            return app.run("voice");
        }
        if (a == "--snapshot") {
            return snapshot(i + 1 < argc ? argv[i + 1] : "");
        }
    }
    return snapshot("");
}
