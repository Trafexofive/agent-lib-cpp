// ToolScene harness: render smoke at multiple sizes + keymap behavior
// (field focus, inline bool/enum edit, composer commit, history browse).
// No live tools::dispatch — the sync worker is exercised by the operator.

#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "src/ui/scenes/tool_scene.hpp"

using namespace cortex::mk3;
using namespace cortex::mk3::ui;

namespace {
int failures = 0;

void check(bool condition, const std::string& name) {
    std::cout << "  " << name << "... " << (condition ? "PASS" : "FAIL") << "\n";
    if (!condition) ++failures;
}

inkcell::KeyEvent key(inkcell::KeyCode code, char ch = 0) {
    inkcell::KeyEvent event;
    event.code = code;
    event.ch = ch;
    return event;
}

void type(scenes::ToolScene& scene, const std::string& text) {
    for (char ch : text) scene.on_key(key(inkcell::KeyCode::Character, ch));
}

std::string surfaceText(const inkcell::Surface& surface) {
    std::string out;
    for (int y = 0; y < surface.height(); ++y) {
        for (int x = 0; x < surface.width(); ++x) out += surface.at({x, y}).glyph;
        out += '\n';
    }
    return out;
}

std::string writeManifest() {
    const std::string dir = "/tmp/mk3-toolscene-smoke";
    ::system(("mkdir -p " + dir).c_str());
    const std::string path = dir + "/tool.yml";
    std::ofstream f(path);
    f << "kind: Tool\n"
         "name: smoke_tool\n"
         "version: \"1.0\"\n"
         "summary: smoke tool for the tool scene harness\n"
         "description: |\n"
         "  Multi-line description used to exercise header wrapping.\n"
         "runtime: python3\n"
         "entrypoint: ./main.py\n"
         "input_schema:\n"
         "  type: object\n"
         "  required: [query]\n"
         "  properties:\n"
         "    query:\n"
         "      type: string\n"
         "      description: The text to process\n"
         "    count:\n"
         "      type: integer\n"
         "    verbose:\n"
         "      type: boolean\n"
         "    mode:\n"
         "      type: string\n"
         "      enum: [fast, careful, slow]\n"
         "    tags:\n"
         "      type: array\n"
         "      items: { type: string }\n"
         "    meta:\n"
         "      type: object\n"
         "examples:\n"
         "  - description: basic\n"
         "    params:\n"
         "      query: hello\n"
         "      count: 3\n"
         "      verbose: true\n"
         "      mode: fast\n"
         "      tags: [a, b]\n";
    return path;
}

void test_render() {
    auto cfg = std::make_shared<InkcellAppConfig>();
    cfg->provider = "openai-codex";
    cfg->model = "gpt-5.5";
    cfg->agentName = "builtin";
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    model->activeToolManifestPath = writeManifest();
    model->activeToolName = "smoke_tool";
    scenes::ToolScene scene(*cfg, bridge, model);
    scene.on_enter();

    for (const inkcell::Size size : {inkcell::Size{80, 24}, inkcell::Size{120, 34},
                                     inkcell::Size{160, 44}}) {
        inkcell::Surface surface(size);
        scene.draw(surface);
        std::string rendered = surfaceText(surface);
        const std::string label = std::to_string(size.w) + "x" + std::to_string(size.h);
        check(rendered.find("smoke_tool") != std::string::npos, "tool title at " + label);
        check(rendered.find("INPUT") != std::string::npos, "INPUT pane at " + label);
        check(rendered.find("OUTPUT") != std::string::npos, "OUTPUT pane at " + label);
        check(rendered.find("○ idle") != std::string::npos, "idle status strip at " + label);
    }

    // Field-level rendering needs enough pane rows — assert on the wide sizes.
    for (const inkcell::Size size : {inkcell::Size{120, 34}, inkcell::Size{160, 44}}) {
        inkcell::Surface surface(size);
        scene.draw(surface);
        std::string rendered = surfaceText(surface);
        const std::string label = std::to_string(size.w) + "x" + std::to_string(size.h);
        check(rendered.find("[int]") != std::string::npos, "int chip at " + label);
        check(rendered.find("[bool]") != std::string::npos, "bool chip at " + label);
        check(rendered.find("[enum]") != std::string::npos, "enum chip at " + label);
        check(rendered.find("hello") != std::string::npos, "example default value at " + label);
    }
}

void test_keymap() {
    auto cfg = std::make_shared<InkcellAppConfig>();
    cfg->provider = "openai-codex";
    cfg->model = "gpt-5.5";
    cfg->agentName = "builtin";
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    model->activeToolManifestPath = writeManifest();
    model->activeToolName = "smoke_tool";
    scenes::ToolScene scene(*cfg, bridge, model);
    scene.on_enter();

    // Field navigation does not crash and moves focus.
    scene.on_key(key(inkcell::KeyCode::Character, 'j'));
    scene.on_key(key(inkcell::KeyCode::Character, 'k'));
    scene.on_key(key(inkcell::KeyCode::Character, 'j'));

    // e opens the composer for the focused (scalar) field.
    scene.on_key(key(inkcell::KeyCode::Character, 'e'));
    check(model->composer.focused, "e opens the composer for a field");
    type(scene, "warp");
    scene.on_key(key(inkcell::KeyCode::Enter));
    check(!model->composer.focused, "Enter commits the field edit");

    // Esc closes a fresh composer edit with no crash.
    scene.on_key(key(inkcell::KeyCode::Character, 'e'));
    scene.on_key(key(inkcell::KeyCode::Escape));
    check(!model->composer.focused, "Esc cancels a field edit");

    // History browse on empty history is a no-op flash (no crash).
    scene.on_key(key(inkcell::KeyCode::Character, '['));
    scene.on_key(key(inkcell::KeyCode::Character, ']'));

    // Tab toggles output focus without crashing; j/k then scrolls output.
    scene.on_key(key(inkcell::KeyCode::Tab));
    scene.on_key(key(inkcell::KeyCode::Character, 'j'));
    scene.on_key(key(inkcell::KeyCode::Tab));

    // Help overlay toggles.
    scene.on_key(key(inkcell::KeyCode::Character, '?'));
    check(model->helpVisible, "? shows help overlay");
    scene.on_key(key(inkcell::KeyCode::Character, '?'));
    check(!model->helpVisible, "? hides help overlay");
}

}  // namespace

int main() {
    std::cout << "tool scene tests\n";
    test_render();
    test_keymap();
    std::cout << (failures == 0 ? "all passed\n" : std::to_string(failures) + " FAILED\n");
    return failures == 0 ? 0 : 1;
}