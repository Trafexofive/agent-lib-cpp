#pragma once
// =============================================================================
// Timeline row types + JSON codec (foundation F2).
// Pure: protocol events + noise filter only. No Agent*, no Surface.
// =============================================================================

#include <json/json.h>

#include <sstream>
#include <string>
#include <vector>

#include "src/protocol/events.hpp"
#include "src/protocol/noise.hpp"
#include "src/ui/text/sanitize.hpp"

namespace cortex::mk3::ui {

enum class PageState { Loading, Populated, Empty, Error };

enum class TimelineKind {
    User,
    Status,
    Stream,
    Thought,
    Action,
    Result,
    Response,
    Final,
    Error,
    Log
};

struct TimelineRow {
    TimelineKind kind = TimelineKind::Log;
    std::string title;
    std::string body;
    bool ok = true;
    std::string actionType;  // tool|agent|feed|relic|workflow
    std::string actionName;
    std::string actionId;
    bool drillable = false;
};

inline const char* kindGlyph(TimelineKind k, bool ok = true) {
    switch (k) {
        case TimelineKind::User:
            return ">";
        case TimelineKind::Status:
            return "◐";
        case TimelineKind::Stream:
            return "…";
        case TimelineKind::Thought:
            return "·";
        case TimelineKind::Action:
            return "◆";
        case TimelineKind::Result:
            return ok ? "✓" : "✗";
        case TimelineKind::Response:
            return "▸";
        case TimelineKind::Final:
            return "■";
        case TimelineKind::Error:
            return "✗";
        case TimelineKind::Log:
            return " ";
    }
    return " ";
}

inline const char* timelineKindName(TimelineKind k) {
    switch (k) {
        case TimelineKind::User:
            return "user";
        case TimelineKind::Status:
            return "status";
        case TimelineKind::Stream:
            return "stream";
        case TimelineKind::Thought:
            return "thought";
        case TimelineKind::Action:
            return "action";
        case TimelineKind::Result:
            return "result";
        case TimelineKind::Response:
            return "response";
        case TimelineKind::Final:
            return "final";
        case TimelineKind::Error:
            return "error";
        case TimelineKind::Log:
            return "log";
    }
    return "log";
}

inline TimelineKind timelineKindFromName(const std::string& s) {
    if (s == "user") return TimelineKind::User;
    if (s == "status") return TimelineKind::Status;
    if (s == "stream") return TimelineKind::Stream;
    if (s == "thought") return TimelineKind::Thought;
    if (s == "action") return TimelineKind::Action;
    if (s == "result") return TimelineKind::Result;
    if (s == "response") return TimelineKind::Response;
    if (s == "final") return TimelineKind::Final;
    if (s == "error") return TimelineKind::Error;
    return TimelineKind::Log;
}

inline bool timelineRowPersistable(const TimelineRow& row) {
    if (row.kind == TimelineKind::Stream) return false;
    if (row.kind == TimelineKind::Status) return false;
    if (row.body.empty() && row.kind != TimelineKind::Action) return false;
    return true;
}

inline std::string serializeTimeline(const std::vector<TimelineRow>& rows) {
    Json::Value arr(Json::arrayValue);
    for (const auto& row : rows) {
        if (!timelineRowPersistable(row)) continue;
        Json::Value o;
        o["kind"] = timelineKindName(row.kind);
        o["title"] = row.title;
        o["body"] = row.body;
        o["ok"] = row.ok;
        if (!row.actionType.empty()) o["actionType"] = row.actionType;
        if (!row.actionName.empty()) o["actionName"] = row.actionName;
        if (!row.actionId.empty()) o["actionId"] = row.actionId;
        if (row.drillable) o["drillable"] = true;
        arr.append(o);
    }
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, arr);
}

inline std::vector<TimelineRow> deserializeTimeline(const std::string& json) {
    std::vector<TimelineRow> out;
    if (json.empty()) return out;
    Json::Value root;
    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream iss(json);
    if (!Json::parseFromStream(rb, iss, &root, &errs) || !root.isArray()) return out;
    out.reserve(root.size());
    for (const auto& o : root) {
        if (!o.isObject()) continue;
        TimelineRow row;
        row.kind = timelineKindFromName(o.get("kind", "log").asString());
        row.title = o.get("title", "").asString();
        row.body = o.get("body", "").asString();
        if (!row.body.empty()) row.body = sanitizeForDisplay(row.body);
        row.ok = o.get("ok", true).asBool();
        row.actionType = o.get("actionType", "").asString();
        row.actionName = o.get("actionName", "").asString();
        row.actionId = o.get("actionId", "").asString();
        row.drillable = o.get("drillable", false).asBool();
        out.push_back(std::move(row));
    }
    return out;
}

// ProtocolEvent → TimelineRow (no Agent tree; drillable resolved by caller).
inline TimelineRow rowFromProtocol(const ProtocolEvent& pe) {
    TimelineRow row;
    if (pe.kind == ProtocolEventKind::THOUGHT) {
        if (protocol::isThoughtNoise(pe.text)) {
            row.kind = TimelineKind::Log;
            row.title = "noise";
            row.body.clear();
            row.ok = true;
            return row;
        }
        row.kind = TimelineKind::Thought;
        row.title = "thought";
        row.body = pe.text;
    } else if (pe.kind == ProtocolEventKind::STATUS) {
        row.kind = TimelineKind::Status;
        row.title = pe.text.rfind("[LIMIT]", 0) == 0     ? "limit"
                    : pe.text.rfind("[FINALIZE]", 0) == 0 ? "finalize"
                                                          : "status";
        row.body = pe.text;
        row.ok = pe.text.find("⚠") == std::string::npos && pe.text.find("error") == std::string::npos;
    } else if (pe.kind == ProtocolEventKind::ACTION) {
        row.kind = TimelineKind::Action;
        row.actionType = pe.action.type;
        row.actionName = pe.action.name;
        row.actionId = pe.action.id;
        row.drillable = (pe.action.type == "agent" && !pe.action.name.empty());
        row.title = pe.action.type + ":" + pe.action.name + " #" + pe.action.id;
        if (row.drillable) row.title += "  ↳ enter";
        row.body = pe.action.body;
    } else if (pe.kind == ProtocolEventKind::RESULT) {
        row.kind = TimelineKind::Result;
        row.ok = pe.result.ok;
        row.actionId = pe.result.id;
        row.actionName = pe.result.toolName;
        row.drillable = false;
        row.title = "#" + pe.result.id + " " + pe.result.toolName;
        row.body = pe.result.summary;
        if (pe.result.elapsedMs > 0)
            row.body += "\n" + std::to_string(static_cast<int>(pe.result.elapsedMs)) + "ms";
    } else if (pe.kind == ProtocolEventKind::RESPONSE) {
        row.kind = TimelineKind::Response;
        row.title = "response";
        row.body = pe.text;
    } else if (pe.kind == ProtocolEventKind::RETRY) {
        row.kind = TimelineKind::Log;
        row.title = "RETRY";
        row.body = pe.text;
        row.ok = false;
    }
    return row;
}

}  // namespace cortex::mk3::ui
