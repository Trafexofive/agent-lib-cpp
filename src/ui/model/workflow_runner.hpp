#pragma once
// Hub-side workflow execution — binds WorkflowEngine to WorkflowRunHub.
// No LLM required for emit/switch/map/try_catch/checkpoint/return smoke paths.

#include <string>

#include <json/json.h>

#include "src/ui/bridge/agent_bridge.hpp"
#include "src/ui/model/workflow_run_model.hpp"
#include "src/workflows/workflow_engine.hpp"

namespace cortex::mk3::ui::model {

inline workflows::WorkflowResult runWorkflowOnHub(const std::string& path, WorkflowRunHub& hub,
                                                  AgentBridge* bridge = nullptr) {
    auto& engine = workflows::WorkflowEngine::instance();
    auto& loaded = engine.load(path);
    if (!loaded.isValid()) {
        hub.fail("failed to load workflow: " + path);
        workflows::WorkflowResult r;
        r.success = false;
        r.error = "failed to load workflow: " + path;
        return r;
    }

    const auto& mf = loaded.manifest();
    hub.prepare(mf, path);

    workflows::WorkflowRuntime rt;
    rt.onProgress = [&](const workflows::StepProgress& p) { hub.onProgress(p); };
    rt.shouldCancel = [&]() { return hub.cancelRequested(); };

    rt.executeTool = [](const std::string& name, const Json::Value& /*params*/) -> Json::Value {
        Json::Value r;
        r["success"] = false;
        r["error"] = "tool not bound in hub runner: " + name;
        return r;
    };
    rt.executeAgent = [](const workflows::WorkflowAgentInvocation& inv) -> Json::Value {
        Json::Value r;
        r["success"] = false;
        r["error"] = "agent not bound in hub runner: " + inv.name;
        return r;
    };
    rt.executeEmit = [&](const std::string& event, const Json::Value& payload) {
        std::string summary = event;
        if (payload.isObject() && payload.isMember("note"))
            summary += " · " + payload["note"].asString();
        hub.note("emit", summary);
    };
    rt.executeCheckpoint = [&](const std::string& id, const Json::Value& state) {
        std::string msg = id;
        if (state.isObject() && state.isMember("message"))
            msg += " · " + state["message"].asString();
        hub.note("checkpoint", msg);
    };
    rt.executeHuman = [&](const std::string& id, const Json::Value& prompt) -> Json::Value {
        std::string text = prompt.get("prompt", id).asString();
        std::string def = prompt.get("default", "").asString();
        hub.markHitl(id, text);
        if (bridge) {
            Json::Value ask;
            ask["prompt"] = text;
            if (!def.empty()) ask["default"] = def;
            ask["title"] = "workflow · " + id;
            Json::Value ans = bridge->requestAsk(ask);
            hub.markRunning();
            if (ans.isObject() && ans.isMember("value")) return ans["value"];
            if (ans.isString()) return ans;
            if (!def.empty()) return Json::Value(def);
            return ans;
        }
        hub.markRunning();
        return def.empty() ? Json::Value("ok") : Json::Value(def);
    };
    rt.executeRelic = [](const std::string& name, const std::string& action,
                         const Json::Value&) -> Json::Value {
        Json::Value r;
        r["success"] = false;
        r["error"] = "relic not bound: " + name + "." + action;
        return r;
    };
    rt.executeFeed = [](const std::string&, const Json::Value&) -> Json::Value {
        return Json::Value(Json::objectValue);
    };
    rt.executeWorkflow = [&](const std::string& name,
                             const Json::Value& params) -> workflows::WorkflowResult {
        return engine.run(name, rt, params);
    };

    hub.markRunning();
    Json::Value input = defaultInputFromSchema(mf.inputSchema);
    // Always ensure a target so smoke-test and friends validate.
    if (!input.isMember("target") || input["target"].asString().empty())
        input["target"] = "hub";
    if (!input.isMember("environment")) input["environment"] = "staging";

    hub.note("input", "target=" + input.get("target", "").asString() +
                          " env=" + input.get("environment", "").asString());

    auto result = engine.execute(mf, rt, input);
    hub.finish(result);
    return result;
}

inline bool workflowRunnablePath(const std::string& path, const std::string& name) {
    if (path.empty()) return false;
    if (name == "workflow_spec") return false;
    // specs / docs often named *_spec
    if (name.size() >= 5 && name.compare(name.size() - 5, 5, "_spec") == 0) return false;
    return true;
}

}  // namespace cortex::mk3::ui::model
