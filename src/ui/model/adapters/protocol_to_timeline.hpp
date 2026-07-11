#pragma once
// Adapter: Agent protocol events -> UI timeline model.

#include <map>
#include <set>
#include <string>
#include <vector>

#include "src/core/agent.hpp"
#include "src/ui/model/timeline_model.hpp"

namespace cortex::mk3::ui::model {

struct ProtocolTimelineOptions {
    AgentPath path;
    std::set<std::string> childAgentNames;
    bool showThoughts = false;
    bool includeResponse = true;
};

inline std::string stablePrefix(const AgentPath& path) {
    if (path.parts.empty()) return "root";
    std::string out = "root";
    for (const auto& part : path.parts) out += "/" + part;
    return out;
}

inline bool isAgentAction(const std::string& type) { return type == "agent"; }

inline TimelineBlock thoughtBlock(const ProtocolEvent& ev, const ProtocolTimelineOptions& opts,
                                  size_t index) {
    TimelineBlock b;
    b.stableId = stablePrefix(opts.path) + ":thought:" + std::to_string(index);
    b.kind = BlockKind::Thought;
    b.status = BlockStatus::Idle;
    b.title = "thought";
    b.summary = ev.text;
    b.hasDetail = !ev.text.empty();
    b.rawBody = ev.text;
    b.tags.push_back("thought");
    return b;
}

inline TimelineBlock actionBlock(const ProtocolEvent& ev, const ProtocolTimelineOptions& opts) {
    const auto& a = ev.action;
    TimelineBlock b;
    b.stableId = stablePrefix(opts.path) + ":action:" + a.id;
    b.kind = BlockKind::Action;
    b.status = BlockStatus::Pending;
    b.title = a.type + ":" + a.name + (a.id.empty() ? "" : " #" + a.id);
    b.summary = a.body;
    b.actionId = a.id;
    b.actionType = a.type;
    b.actionName = a.name;
    b.rawBody = a.body;
    b.hasDetail = true;
    b.tags.push_back(a.type.empty() ? "action" : a.type);
    if (!a.mode.empty()) b.tags.push_back(a.mode);
    for (const auto& [k, v] : a.modifiers) b.metadata[k] = v;

    if (isAgentAction(a.type)) {
        b.drillable = opts.childAgentNames.count(a.name) > 0;
        b.related.agentPath = opts.path.child(a.name).parts;
        b.related.entityId = a.name;
        b.related.label = a.name;
        b.related.available = b.drillable;
    }
    return b;
}

inline TimelineBlock resultBlock(const ProtocolEvent& ev, const ProtocolTimelineOptions& opts,
                                 const std::map<std::string, ProtocolAction>& actionsById) {
    const auto& r = ev.result;
    TimelineBlock b;
    b.stableId = stablePrefix(opts.path) + ":result:" + r.id;
    b.kind = BlockKind::Result;
    b.status = r.ok ? BlockStatus::Ok : BlockStatus::Error;
    b.title = std::string(r.ok ? "result" : "error") + (r.id.empty() ? "" : " #" + r.id);
    if (!r.toolName.empty()) b.title += " " + r.toolName;
    b.summary = r.summary;
    b.actionId = r.id;
    b.actionName = r.toolName;
    b.rawBody = r.summary;
    b.hasDetail = true;
    b.tags.push_back(r.ok ? "ok" : "error");
    if (!r.toolName.empty()) b.tags.push_back(r.toolName);
    b.metadata["exit_code"] = std::to_string(r.exitCode);
    b.metadata["elapsed_ms"] = std::to_string(static_cast<int>(r.elapsedMs));
    b.metadata["output_bytes"] = std::to_string(r.outputBytes);

    auto actionIt = actionsById.find(r.id);
    if (actionIt != actionsById.end()) {
        const auto& a = actionIt->second;
        b.actionType = a.type;
        b.actionName = a.name;
        if (isAgentAction(a.type)) {
            b.drillable = opts.childAgentNames.count(a.name) > 0;
            b.related.agentPath = opts.path.child(a.name).parts;
            b.related.entityId = a.name;
            b.related.label = a.name;
            b.related.available = b.drillable;
            if (b.drillable) b.tags.push_back("drillable");
        }
    }
    return b;
}

inline TimelineBlock responseBlock(const ProtocolEvent& ev, const ProtocolTimelineOptions& opts,
                                   size_t index) {
    TimelineBlock b;
    b.stableId = stablePrefix(opts.path) + ":response:" + std::to_string(index);
    b.kind = BlockKind::Response;
    b.status = BlockStatus::Ok;
    b.title = "response";
    b.summary = ev.text;
    b.rawBody = ev.text;
    b.hasDetail = !ev.text.empty();
    b.tags.push_back("response");
    return b;
}

inline std::vector<TimelineBlock> protocolEventsToTimeline(
    const std::vector<ProtocolEvent>& events, const ProtocolTimelineOptions& opts = {}) {
    std::vector<TimelineBlock> blocks;
    std::map<std::string, ProtocolAction> actionsById;

    for (size_t i = 0; i < events.size(); ++i) {
        const auto& ev = events[i];
        switch (ev.kind) {
            case ProtocolEventKind::THOUGHT:
                if (opts.showThoughts) blocks.push_back(thoughtBlock(ev, opts, i));
                break;
            case ProtocolEventKind::ACTION:
                actionsById[ev.action.id] = ev.action;
                blocks.push_back(actionBlock(ev, opts));
                break;
            case ProtocolEventKind::RESULT:
                blocks.push_back(resultBlock(ev, opts, actionsById));
                break;
            case ProtocolEventKind::RESPONSE:
                if (opts.includeResponse) blocks.push_back(responseBlock(ev, opts, i));
                break;
        }
    }
    return blocks;
}

}  // namespace cortex::mk3::ui::model
