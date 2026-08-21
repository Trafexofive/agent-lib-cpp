#pragma once
// ask_tool wire contract — shared by TUI dialog, stdin fallback, dispatch.
// One JSON shape in, one JSON shape out. No TUI types here.

#include <json/json.h>

#include <string>

namespace cortex::mk3::tools {

// Canonical params: title, message, cards[], timeout_sec.
// Legacy aliases accepted once here: question/prompt → title,
// options + no cards → one choice card, default → defaultValue.
inline Json::Value normalizeAskParams(Json::Value p) {
    if (!p.isObject())
        p = Json::Value(Json::objectValue);

    if (!p.isMember("timeout_sec") && p.isMember("timeout") && p["timeout"].isNumeric())
        p["timeout_sec"] = p["timeout"];

    std::string title = p.get("title", "").asString();
    if (title.empty())
        title = p.get("question", p.get("prompt", "")).asString();
    if (!title.empty())
        p["title"] = title;

    const bool hasCards =
        p.isMember("cards") && p["cards"].isArray() && p["cards"].size() > 0;
    if (!hasCards) {
        Json::Value card(Json::objectValue);
        card["id"] = "response";
        card["title"] = title.empty() ? "Question" : title;
        card["message"] = p.get("message", "").asString();
        if (p.isMember("defaultValue"))
            card["defaultValue"] = p["defaultValue"];
        else if (p.isMember("default"))
            card["defaultValue"] = p["default"];
        if (p.isMember("options") && p["options"].isArray() && p["options"].size() > 0) {
            card["type"] = "choice";
            card["options"] = p["options"];
        } else {
            card["type"] = "text";
        }
        Json::Value cards(Json::arrayValue);
        cards.append(card);
        p["cards"] = cards;
    }
    if (p.get("title", "").asString().empty())
        p["title"] = "Agent asks";
    return p;
}

inline Json::Value askResult(bool success, bool cancelled, bool timedOut,
                             Json::Value results, const std::string& error = {}) {
    if (!results.isObject())
        results = Json::Value(Json::objectValue);
    Json::Value answered(Json::arrayValue);
    for (const auto& key : results.getMemberNames())
        answered.append(key);
    Json::Value out;
    out["success"] = success && !cancelled && !timedOut;
    out["cancelled"] = cancelled && !timedOut;
    out["timed_out"] = timedOut;
    out["results"] = std::move(results);
    out["answered"] = answered;
    out["count"] = static_cast<Json::UInt64>(answered.size());
    if (!error.empty())
        out["error"] = error;
    return out;
}

}  // namespace cortex::mk3::tools
