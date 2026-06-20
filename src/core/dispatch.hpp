// ─────────────────────────────────────────────────────────────────────────────
// Action Dispatcher — routes parsed actions to tools, agents, or relics
// Modular: each dispatch target is self-contained.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <json/json.h>

#include <map>
#include <sstream>
#include <string>

#include "../protocol/parser.hpp"
#include "../relics/docker_dispatcher.hpp"
#include "../tools/registry.hpp"
#include "../workflows/workflow_engine.hpp"

namespace cortex {
namespace mk3 {
namespace dispatch {

// ── Tool dispatcher ──
inline Json::Value dispatchTool(const protocol::ParsedAction& action) {
    auto& reg = tools::ToolRegistry::instance();
    auto fn = reg.get(action.name);
    if (fn) {
        std::string result = fn(action.params);
        Json::Value parsed;
        Json::CharReaderBuilder r;
        std::string errs;
        std::istringstream ss(result);
        if (Json::parseFromStream(r, ss, &parsed, &errs))
            return parsed;
        Json::Value fallback;
        fallback["output"] = result;
        fallback["success"] = true;
        return fallback;
    }
    Json::Value err;
    err["success"] = false;
    err["error"] = "Unknown tool: " + action.name;
    return err;
}

// ── Relic dispatcher ──
inline Json::Value dispatchRelic(const protocol::ParsedAction& action) {
    // Try Docker relic dispatcher first (handles managed + remote)
    auto& drd = relics::DockerRelicDispatcher::instance();
    if (drd.getRelic(action.name)) {
        Json::Value relicParams = action.params;
        std::string endpoint = relicParams.get("endpoint", action.name).asString();
        if (endpoint == action.name && !action.content.empty()) {
            endpoint = action.content;
        }
        if (relicParams.isMember("body") && relicParams["body"].isObject())
            relicParams = relicParams["body"];
        relicParams.removeMember("endpoint");
        relicParams.removeMember("method");
        auto rr = drd.dispatch(action.name, endpoint, relicParams);
        Json::Value result;
        result["success"] = rr.success;
        if (rr.success) {
            // Parse data as JSON if possible
            Json::Value parsed;
            Json::CharReaderBuilder r;
            std::string errs;
            std::istringstream ss(rr.data);
            if (Json::parseFromStream(r, ss, &parsed, &errs))
                result["data"] = parsed;
            else
                result["data"] = rr.data;
        } else {
            result["error"] = rr.error;
        }
        return result;
    }
}

// ── Agent dispatcher — sub-agent delegation.
// Callback receives the full ParsedAction so XML modifiers such as
// ephemeral="true" and dump_context="true" survive into runtime behavior.
using AgentDispatchFn =
    std::function<Json::Value(const protocol::ParsedAction& action, const std::string& instruction)>;

inline Json::Value dispatchAgent(const protocol::ParsedAction& action, AgentDispatchFn delegate) {
    if (!delegate) {
        Json::Value err;
        err["success"] = false;
        err["error"] = "No sub-agent dispatcher configured";
        return err;
    }
    // Accept any of the names LLMs commonly use; instruction was hard-coded
    // and any other key silently fell back to "Execute task".
    std::string instruction;
    if (!action.content.empty()) {
        instruction = action.content;
    } else {
        for (const char* key : {"instruction", "query", "task", "prompt", "input", "message"}) {
            std::string v = action.params.get(key, "").asString();
            if (!v.empty()) {
                instruction = v;
                break;
            }
        }
        if (instruction.empty())
            instruction = "Execute task";
    }
    Json::Value subResult = delegate(action, instruction);
    Json::Value result;
    if (subResult.isString()) {
        // Sub returned a plain string — DP01: empty string is still a valid
        // response (e.g. the sub chose to say nothing). Don't treat it as failure.
        result["success"] = true;
        result["output"] = subResult.asString();
    } else if (subResult.isObject()) {
        // Sub returned a structured result; honour its own success field if present.
        result["success"] = subResult.get("success", true).asBool();
        result["output"] = subResult.get("output", Json::Value("")).asString();
        if (subResult.isMember("error"))
            result["error"] = subResult["error"];
    } else {
        result["success"] = false;
        result["error"] = "sub-agent returned unexpected type";
    }
    return result;
}

// ── Feed dispatcher — agent calls feed as action, triggers poll with optional params ──
// If action.name contains a '.' the part before is the feed name and the part
// after is a tool name registered on that feed (e.g. "workspace.pin"). Otherwise
// the action is treated as a poll (existing behavior).
//
// Dotted form is always interpreted as a tool call — it never silently falls
// through to polling the dotted literal as a feed name. This keeps errors
// unambiguous when the model misspells a feed tool.
inline Json::Value dispatchFeed(const protocol::ParsedAction& action) {
    auto& engine = feeds::FeedEngine::instance();

    // Tool call path: name = "<feed>.<tool>"
    auto dot = action.name.find('.');
    if (dot != std::string::npos) {
        std::string feedName = action.name.substr(0, dot);
        std::string toolName = action.name.substr(dot + 1);

        // Empty tool name (trailing dot) is always an error.
        if (toolName.empty()) {
            Json::Value err;
            err["success"] = false;
            err["feed"] = feedName;
            err["error"] = "feed action name '" + action.name + "' has empty tool name";
            return err;
        }

        // Unknown feed must be reported, not silently polled.
        if (!engine.has(feedName)) {
            Json::Value err;
            err["success"] = false;
            err["feed"] = feedName;
            err["tool"] = toolName;
            err["error"] = "unknown feed: " + feedName;
            return err;
        }

        // Known feed but unknown tool — report missing tool, don't poll.
        if (!engine.feedHasTool(feedName, toolName)) {
            Json::Value err;
            err["success"] = false;
            err["feed"] = feedName;
            err["tool"] = toolName;
            err["error"] = "unknown feed tool: " + action.name;
            return err;
        }

        Json::Value result = engine.callFeedTool(feedName, toolName, action.params);
        result["feed"] = feedName;
        result["tool"] = toolName;
        return result;
    }

    // Poll path: name = "<feed>" (existing behavior)
    // Pass action params to feed script as FEED_PARAMS env var
    std::string paramsJson;
    if (!action.params.isNull() && !action.params.empty()) {
        Json::StreamWriterBuilder w;
        w["indentation"] = "";
        paramsJson = Json::writeString(w, action.params);
    } else if (!action.content.empty()) {
        paramsJson = action.content;
    }
    setenv("FEED_PARAMS", paramsJson.c_str(), 1);

    auto fr = engine.pollOne(action.name, true);

    Json::Value result;
    result["success"] = fr.ok;
    if (fr.ok) {
        if (!fr.json.empty()) {
            Json::Value parsed;
            Json::CharReaderBuilder r;
            std::string errs;
            std::istringstream ss(fr.json);
            if (Json::parseFromStream(r, ss, &parsed, &errs))
                result["data"] = parsed;
            else
                result["data"] = fr.json;
        }
        result["summary"] = fr.summary;
    } else {
        result["error"] = fr.summary;
    }
    return result;
}

// ── Workflow dispatcher — executes a named workflow through the WorkflowEngine ──
using WorkflowDispatchFn = std::function<workflows::WorkflowResult(const std::string& workflowName,
                                                                   const Json::Value& params)>;

inline Json::Value dispatchWorkflow(const protocol::ParsedAction& action,
                                    WorkflowDispatchFn executor) {
    if (!executor) {
        Json::Value err;
        err["success"] = false;
        err["error"] = "No workflow executor configured";
        return err;
    }
    auto wfResult = executor(action.name, action.params);
    Json::Value result;
    result["success"] = wfResult.success;
    result["workflow"] = wfResult.workflowName;
    result["elapsed_ms"] = wfResult.elapsedMs;
    result["step_count"] = (int)wfResult.stepIds.size();
    Json::Value outputs(Json::objectValue);
    for (auto& [id, val] : wfResult.outputs)
        outputs[id] = val;
    result["outputs"] = outputs;

    // Build a compact output string so the model sees what happened
    std::ostringstream summary;
    summary << "workflow=" << wfResult.workflowName
            << " success=" << (wfResult.success ? "ok" : "fail") << " steps=[";
    for (size_t i = 0; i < wfResult.stepIds.size(); ++i) {
        if (i > 0)
            summary << ", ";
        summary << wfResult.stepIds[i];
        auto it = wfResult.outputs.find(wfResult.stepIds[i]);
        if (it != wfResult.outputs.end() && it->second.isMember("success"))
            summary << (it->second["success"].asBool() ? ":ok" : ":fail");
    }
    summary << "]";
    if (!wfResult.error.empty())
        summary << " error=" << wfResult.error;
    result["output"] = summary.str();
    if (!wfResult.error.empty())
        result["error"] = wfResult.error;
    if (!wfResult.diagnostics.empty()) {
        Json::Value diags(Json::arrayValue);
        for (auto& d : wfResult.diagnostics)
            diags.append(d);
        result["diagnostics"] = diags;
    }
    return result;
}

// ── Unified dispatcher ──
struct ActionDispatcher {
    AgentDispatchFn agentDelegate;
    WorkflowDispatchFn workflowDelegate;

    Json::Value dispatch(const protocol::ParsedAction& action) {
        switch (action.type) {
            case protocol::ActionType::TOOL:
                return dispatchTool(action);
            case protocol::ActionType::RELIC:
                return dispatchRelic(action);
            case protocol::ActionType::AGENT:
                return dispatchAgent(action, agentDelegate);
            case protocol::ActionType::FEED:
                return dispatchFeed(action);
            case protocol::ActionType::WORKFLOW:
                return dispatchWorkflow(action, workflowDelegate);
            default:
                Json::Value err;
                err["success"] = false;
                err["error"] = "Unknown action type";
                return err;
        }
    }
};

// ── Dedup helper ──
inline std::string dedupKey(const protocol::ParsedAction& action) {
    return action.name + ":" + Json::writeString(Json::StreamWriterBuilder(), action.params) + ":" +
           action.content;
}

}  // namespace dispatch
}  // namespace mk3
}  // namespace cortex
