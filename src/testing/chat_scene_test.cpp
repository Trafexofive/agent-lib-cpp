#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "src/ui/scenes/agent_scene.hpp"

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

void type(scenes::AgentScene& scene, const std::string& text) {
    for (char ch : text) scene.on_key(key(inkcell::KeyCode::Character, ch));
}

void test_ask_choice_roundtrip() {
    InkcellAppConfig cfg;
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    scenes::AgentScene scene(cfg, bridge, model);
    scene.on_enter();

    Json::Value params;
    params["chainTitle"] = "Pick worker";
    Json::Value card;
    card["id"] = "worker";
    card["type"] = "choice";
    card["title"] = "Worker";
    card["options"].append("reader");
    card["options"].append("tester");
    params["cards"].append(card);

    auto future = std::async(std::launch::async, [&] { return bridge.requestAsk(params); });
    for (int i = 0; i < 100 && !bridge.askPending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    scene.update({}, {});
    check(model->askActive, "scene activates ask overlay from worker request");
    scene.on_key(key(inkcell::KeyCode::ArrowDown));
    scene.on_key(key(inkcell::KeyCode::Enter));
    scene.update({}, {});
    Json::Value result = future.get();
    check(result["success"].asBool() && result["results"]["worker"].asString() == "tester",
          "scene returns selected ask choice to worker");
}

void test_slash_and_completion() {
    InkcellAppConfig cfg;
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    scenes::AgentScene scene(cfg, bridge, model);
    scene.on_enter();

    type(scene, "/help");
    scene.on_key(key(inkcell::KeyCode::Enter));
    check(!model->rootRows.empty() && model->rootRows.back().title == "commands",
          "slash help is intercepted locally");
    check(model->pendingSubmit.empty(), "slash help is not sent to agent");

    model->composer.value.clear();
    model->composer.cursor = 0;
    type(scene, "/debug");
    scene.on_key(key(inkcell::KeyCode::Tab));
    check(model->composer.value == "/debugger ", "Tab completes dynamic prompt command");
    scene.on_key(key(inkcell::KeyCode::Enter));
    check(!model->composer.value.empty() && model->composer.value.find("debug") != std::string::npos,
          "dynamic command expands into reviewed composer text");
    check(model->pendingSubmit.empty(), "dynamic expansion is not auto-submitted");
}

void test_ctrl_c_state() {
    InkcellAppConfig cfg;
    AgentBridge bridge;
    auto model = std::make_shared<ShellModel>();
    scenes::AgentScene scene(cfg, bridge, model);
    model->running = true;
    g_running = true;
    scene.on_key(key(inkcell::KeyCode::CtrlC));
    check(!g_running && model->status == "cancelling", "Ctrl-C requests active turn cancellation");
    g_running = true;
}
}  // namespace

int main() {
    std::cout << "Chat scene integration tests\n";
    test_ask_choice_roundtrip();
    test_slash_and_completion();
    test_ctrl_c_state();
    std::cout << "\n" << (failures == 0 ? "all passed" : "failures: " + std::to_string(failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
