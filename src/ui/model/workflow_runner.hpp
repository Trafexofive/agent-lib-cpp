#pragma once
// Hub-side workflow execution — binds WorkflowEngine to WorkflowRunHub.
// Real builtin tools via ToolRegistry; agent steps stubbed (success) so
// creative pipelines dogfood without an LLM. HITL uses AgentBridge ask.

#include <sstream>
#include <string>

#include <json/json.h>

#include "src/tools/dispatch.hpp"
#include "src/ui/bridge/agent_bridge.hpp"
#include "src/ui/model/workflow_run_model.hpp"
#include "src/workflows/workflow_engine.hpp"

namespace cortex::mk3::ui::model {

namespace detail {

inline Json::Value parseToolJson(const std::string& raw) {
    Json::Value parsed;
    Json::CharReaderBuilder r;
    std::string errs;
    std::istringstream ss(raw);
    if (Json::parseFromStream(r, ss, &parsed, &errs) && parsed.isObject())
        return parsed;
    Json::Value fail;
    fail["success"] = false;
    fail["error"] = "tool returned non-json";
    fail["raw"] = raw.substr(0, 400);
    return fail;
}

inline Json::Value dispatchBuiltinTool(const std::string& name, const Json::Value& params) {
    tools::registerDefaults();
    return parseToolJson(tools::dispatch(name, params));
}

inline void seedHubDefaults(Json::Value& input) {
    if (!input.isMember("target") || input["target"].asString().empty())
        input["target"] = "hub";
    if (!input.isMember("environment") || input["environment"].asString().empty())
        input["environment"] = "staging";
    if (!input.isMember("topic") || input["topic"].asString().empty())
        input["topic"] = "terminal-native content systems";
    if (!input.isMember("format") || input["format"].asString().empty())
        input["format"] = "essay";
    if (!input.isMember("tone") || input["tone"].asString().empty())
        input["tone"] = "technical";
    if (!input.isMember("audience") || input["audience"].asString().empty())
        input["audience"] = "builders";
    if (!input.isMember("out_dir") || input["out_dir"].asString().empty())
        input["out_dir"] = "/tmp/cortex-content";
    if (!input.isMember("angle") || input["angle"].asString().empty())
        input["angle"] = "first-principles";
}

}  // namespace detail

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

    // Real builtins — same registry agent turns use.
    rt.executeTool = [](const std::string& name, const Json::Value& params) -> Json::Value {
        return detail::dispatchBuiltinTool(name, params);
    };

    // Hub has no live Agent graph — stub success so agent nodes light up green
    // and creative pipelines remain runnable offline. Agent runtime path can
    // replace this when a bound Agent is present.
    rt.executeAgent = [&](const workflows::WorkflowAgentInvocation& inv) -> Json::Value {
        hub.note("agent.stub", inv.name + " · " + inv.instruction.substr(0, 120));
        Json::Value r;
        r["success"] = true;
        r["stub"] = true;
        r["agent"] = inv.name;
        r["output"] = std::string("[hub-stub:") + inv.name + "] " + inv.instruction;
        r["angles"] = Json::Value(Json::arrayValue);
        r["angles"].append("first-principles");
        r["angles"].append("contrarian");
        r["angles"].append("operator-diary");
        return r;
    };

    rt.executeEmit = [&](const std::string& event, const Json::Value& payload) {
        std::string summary = event;
        if (payload.isObject()) {
            if (payload.isMember("note")) summary += " · " + payload["note"].asString();
            if (payload.isMember("topic")) summary += " · " + payload["topic"].asString();
            if (payload.isMember("draft_path"))
                summary += " · " + payload["draft_path"].asString();
        }
        hub.note("emit", summary);
    };

    rt.executeCheckpoint = [&](const std::string& id, const Json::Value& state) {
        std::string msg = id;
        if (state.isObject() && state.isMember("message"))
            msg += " · " + state["message"].asString();
        else if (state.isObject() && state.isMember("draft"))
            msg += " · " + state["draft"].asString();
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
            // Single confirm-style card when possible
            Json::Value cards(Json::arrayValue);
            Json::Value c;
            c["id"] = "answer";
            c["type"] = "text";
            c["title"] = text;
            if (!def.empty()) c["defaultValue"] = def;
            cards.append(c);
            ask["cards"] = cards;
            Json::Value ans = bridge->requestAsk(ask);
            hub.markRunning();
            if (ans.isObject()) {
                if (ans.isMember("answer")) return ans["answer"];
                if (ans.isMember("value")) return ans["value"];
                if (ans.isMember("results") && ans["results"].isObject() &&
                    ans["results"].isMember("answer"))
                    return ans["results"]["answer"];
            }
            if (ans.isString()) return ans;
            if (!def.empty()) return Json::Value(def);
            return ans;
        }
        hub.markRunning();
        return def.empty() ? Json::Value("y") : Json::Value(def);
    };

    rt.executeRelic = [](const std::string& name, const std::string& action,
                         const Json::Value&) -> Json::Value {
        Json::Value r;
        r["success"] = false;
        r["error"] = "relic not bound in hub runner: " + name + "." + action;
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
    detail::seedHubDefaults(input);

    hub.note("input", "topic=" + input.get("topic", "").asString() +
                          " format=" + input.get("format", "").asString() +
                          " out=" + input.get("out_dir", "").asString());

    auto result = engine.execute(mf, rt, input);
    hub.finish(result);
    return result;
}

inline bool workflowRunnablePath(const std::string& path, const std::string& name) {
    if (path.empty()) return false;
    if (name == "workflow_spec" || name == "spec") return false;
    if (name.size() >= 5 && name.compare(name.size() - 5, 5, "_spec") == 0) return false;
    return true;
}

}  // namespace cortex::mk3::ui::model
