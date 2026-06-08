// ─────────────────────────────────────────────────────────────────────────────
// Action Dispatcher — routes parsed actions to tools, agents, or relics
// Modular: each dispatch target is self-contained.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "../protocol/parser.hpp"
#include "../tools/registry.hpp"
#include "../relics/builtin_relics.hpp"
#include "../relics/docker_dispatcher.hpp"
#include "../feeds/feed_engine.hpp"
#include <json/json.h>
#include <sstream>
#include <map>
#include <string>

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
        if (Json::parseFromStream(r, ss, &parsed, &errs)) return parsed;
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
        std::string endpoint = action.params.get("endpoint", action.name).asString();
        if (endpoint == action.name && !action.content.empty()) {
            endpoint = action.content;
        }
        auto rr = drd.dispatch(action.name, endpoint, action.params);
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

    // Fall back to builtin relic dispatcher
    std::string endpoint = action.params.get("endpoint", action.name).asString();
    if (endpoint == action.name && !action.content.empty()) {
        endpoint = action.content;
    }
    auto rr = relics::RelicDispatcher::instance().dispatch(action.name, endpoint, action.params);
    Json::Value result;
    result["success"] = rr.success;
    if (rr.success) result["data"] = rr.data;
    else result["error"] = rr.error;
    return result;
}

// ── Agent dispatcher (placeholder — sub-agent delegation) ──
// Called with a callback to the parent agent for sub-agent execution
using AgentDispatchFn = std::function<Json::Value(const std::string& agentName,
                                                    const std::string& instruction)>;

inline Json::Value dispatchAgent(const protocol::ParsedAction& action,
                                  AgentDispatchFn delegate) {
    if (!delegate) {
        Json::Value err;
        err["success"] = false;
        err["error"] = "No sub-agent dispatcher configured";
        return err;
    }
    std::string instruction = action.content.empty()
        ? action.params.get("instruction", "Execute task").asString()
        : action.content;
    Json::Value subResult = delegate(action.name, instruction);
    std::string output = subResult.isString() ? subResult.asString() : "";
    Json::Value result;
    result["success"] = !output.empty();
    result["output"] = output;
    return result;
}

// ── Feed dispatcher — agent calls feed as action, triggers poll with optional params ──
inline Json::Value dispatchFeed(const protocol::ParsedAction& action) {
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

    auto fr = feeds::FeedEngine::instance().pollOne(action.name);

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

// ── Unified dispatcher ──
struct ActionDispatcher {
    AgentDispatchFn agentDelegate;

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
    return action.name + ":"
         + Json::writeString(Json::StreamWriterBuilder(), action.params)
         + ":" + action.content;
}

} // namespace dispatch
} // namespace mk3
} // namespace cortex
