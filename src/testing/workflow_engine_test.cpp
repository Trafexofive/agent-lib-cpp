// =============================================================================
// agent-lib-MK3 — Workflow engine execution tests
//
// Verifies workflow manifests are not just prompt metadata: YAML-loaded
// workflows execute through WorkflowRuntime callbacks, resolve ${input.*} and
// ${step.*} variables, and honor on_error policy.
// =============================================================================

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "src/workflows/workflow_engine.hpp"

using namespace cortex::mk3::workflows;
namespace fs = std::filesystem;

static int passed = 0, failed = 0;

#define TEST(name)                           \
    do {                                     \
        std::cout << "  " << name << "... "; \
    } while (0)
#define PASS()                 \
    do {                       \
        passed++;              \
        std::cout << "PASS\n"; \
    } while (0)
#define FAIL(msg)                             \
    do {                                      \
        failed++;                             \
        std::cout << "FAIL: " << msg << "\n"; \
        return;                               \
    } while (0)
#define CHECK(cond, msg) \
    do {                 \
        if (!(cond)) {   \
            FAIL(msg);   \
        }                \
    } while (0)

static void writeFile(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << s;
}

static fs::path fixturePath(const std::string& name) {
    fs::path root = fs::temp_directory_path() / "mk3-workflow-engine-tests";
    fs::create_directories(root);
    return root / name;
}

struct ToolCall {
    std::string name;
    Json::Value params;
};

void test_yaml_workflow_executes_tool_steps_with_variable_resolution() {
    TEST("YAML workflow executes tool steps with input and step variables");

    fs::path wfPath = fixturePath("variable-resolution.yml");
    writeFile(wfPath, R"YAML(kind: Workflow
version: "1.0"
name: variable-resolution
summary: "exercise input and step interpolation"
steps:
  - id: echo_target
    type: tool
    tool: exec
    params:
      command: "echo ${input.target}"
  - id: consume_previous
    type: tool
    tool: exec
    params:
      command: "printf ${echo_target.output}"
)YAML");

    WorkflowEngine engine;
    auto& workflow = engine.load(wfPath.string());
    CHECK(workflow.isValid(), "workflow did not load");
    CHECK(workflow.name() == "variable-resolution", "workflow name not parsed");
    CHECK(workflow.steps().size() == 2, "workflow steps not parsed");

    std::vector<ToolCall> calls;
    WorkflowRuntime rt;
    rt.executeTool = [&](const std::string& name, const Json::Value& params) -> Json::Value {
        calls.push_back({name, params});
        Json::Value r;
        r["success"] = true;
        r["output"] = params.get("command", "").asString();
        return r;
    };

    Json::Value input;
    input["target"] = "src/core/agent.cpp";
    auto result = engine.execute(workflow, rt, input);

    CHECK(result.success, "workflow execution failed: " + result.error);
    CHECK(calls.size() == 2, "expected two tool calls");
    CHECK(calls[0].name == "exec", "first tool name wrong");
    CHECK(calls[0].params["command"].asString() == "echo src/core/agent.cpp",
          "input variable was not resolved");
    CHECK(calls[1].params["command"].asString() == "printf echo src/core/agent.cpp",
          "step output variable was not resolved");
    CHECK(result.stepIds.size() == 2, "step ids not recorded");
    CHECK(result.outputs.count("consume_previous") == 1, "final step output missing");

    PASS();
}

void test_on_error_skip_continues_after_failed_step() {
    TEST("on_error skip records diagnostic and continues");

    fs::path wfPath = fixturePath("skip-error.yml");
    writeFile(wfPath, R"YAML(kind: Workflow
version: "1.0"
name: skip-error
steps:
  - id: optional_step
    type: tool
    tool: optional
    on_error: skip
    params:
      command: "fail me"
  - id: required_step
    type: tool
    tool: required
    params:
      command: "still runs"
)YAML");

    WorkflowEngine engine;
    auto& workflow = engine.load(wfPath.string());
    CHECK(workflow.isValid(), "workflow did not load");

    std::vector<std::string> calls;
    WorkflowRuntime rt;
    rt.executeTool = [&](const std::string& name, const Json::Value&) -> Json::Value {
        calls.push_back(name);
        Json::Value r;
        if (name == "optional") {
            r["success"] = false;
            r["error"] = "optional failed";
        } else {
            r["success"] = true;
            r["output"] = "ok";
        }
        return r;
    };

    auto result = engine.execute(workflow, rt);

    CHECK(result.success, "skip failure should not fail workflow");
    CHECK(calls.size() == 2, "workflow did not continue after skipped failure");
    CHECK(result.diagnostics.size() == 1, "skip failure diagnostic missing");
    CHECK(result.diagnostics[0].find("optional failed") != std::string::npos,
          "skip diagnostic does not include tool error");
    CHECK(result.outputs.count("required_step") == 1, "required step output missing");

    PASS();
}

void test_on_error_abort_stops_after_failed_step() {
    TEST("on_error abort fails workflow and stops");

    fs::path wfPath = fixturePath("abort-error.yml");
    writeFile(wfPath, R"YAML(kind: Workflow
version: "1.0"
name: abort-error
steps:
  - id: required_step
    type: tool
    tool: required
    on_error: abort
    params:
      command: "fail me"
  - id: never_runs
    type: tool
    tool: later
    params:
      command: "must not run"
)YAML");

    WorkflowEngine engine;
    auto& workflow = engine.load(wfPath.string());
    CHECK(workflow.isValid(), "workflow did not load");

    std::vector<std::string> calls;
    WorkflowRuntime rt;
    rt.executeTool = [&](const std::string& name, const Json::Value&) -> Json::Value {
        calls.push_back(name);
        Json::Value r;
        r["success"] = false;
        r["error"] = "required failed";
        return r;
    };

    auto result = engine.execute(workflow, rt);

    CHECK(!result.success, "abort failure should fail workflow");
    CHECK(result.error == "required failed", "workflow error should preserve tool error");
    CHECK(calls.size() == 1, "workflow did not stop after abort failure");
    CHECK(result.outputs.count("never_runs") == 0, "aborted step output should be absent");

    PASS();
}

void test_agent_step_propagates_modifiers_to_callback() {
    TEST("agent step passes ephemeral + dump_context to callback");

    fs::path wfPath = fixturePath("agent-modifiers.yml");
    writeFile(wfPath, R"YAML(kind: Workflow
version: "1.0"
name: agent-modifiers
steps:
  - id: think
    type: agent
    agent: planner
    params:
      instruction: "outline a plan"
      ephemeral: true
      dump_context: true
  - id: follow_up
    type: agent
    agent: planner
    params:
      instruction: "execute the plan"
)YAML");

    WorkflowEngine engine;
    auto& workflow = engine.load(wfPath.string());
    CHECK(workflow.isValid(), "agent-modifiers workflow did not load");

    std::vector<WorkflowAgentInvocation> invocations;
    WorkflowRuntime rt;
    rt.executeAgent = [&](const WorkflowAgentInvocation& inv) -> Json::Value {
        invocations.push_back(inv);
        Json::Value r;
        r["success"] = true;
        r["output"] = "ok:" + inv.instruction;
        if (inv.dumpContext)
            r["trace"] = "trace-for:" + inv.name;
        return r;
    };

    auto result = engine.execute(workflow, rt);
    CHECK(result.success, "agent-modifiers workflow did not succeed");
    CHECK(invocations.size() == 2, "expected two agent invocations");

    // First step: ephemeral + dump_context true, instruction carried.
    CHECK(invocations[0].name == "planner", "first invocation name");
    CHECK(invocations[0].instruction == "outline a plan",
          "first invocation instruction");
    CHECK(invocations[0].ephemeral == true,
          "first invocation ephemeral not propagated");
    CHECK(invocations[0].dumpContext == true,
          "first invocation dump_context not propagated");

    // Second step: defaults — both modifiers false.
    CHECK(invocations[1].name == "planner", "second invocation name");
    CHECK(invocations[1].instruction == "execute the plan",
          "second invocation instruction");
    CHECK(invocations[1].ephemeral == false,
          "second invocation should default ephemeral to false");
    CHECK(invocations[1].dumpContext == false,
          "second invocation should default dump_context to false");

    // First step's output should include the trace the callback returned
    // when dump_context was honored.
    auto& thinkOut = result.outputs["think"];
    CHECK(thinkOut.isMember("trace"),
          "first agent step should expose trace when dump_context=true");
    CHECK(thinkOut["trace"].asString() == "trace-for:planner",
          "first agent step trace content");

    PASS();
}

int main() {
    std::cout.setf(std::ios::unitbuf);
    std::cout << "\n╔══════════════════════════════════════════╗\n";
    std::cout << "║   MK3 Workflow Engine Tests             ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n\n";

    test_yaml_workflow_executes_tool_steps_with_variable_resolution();
    test_on_error_skip_continues_after_failed_step();
    test_on_error_abort_stops_after_failed_step();
    test_agent_step_propagates_modifiers_to_callback();

    std::cout << "\n──────────────────────────────────────────\n";
    std::cout << "  " << passed << " passed, " << failed << " failed\n";
    std::cout << "──────────────────────────────────────────\n";
    return failed > 0 ? 1 : 0;
}
