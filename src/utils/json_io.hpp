#pragma once
// =============================================================================
// agent-lib-MK3 — JSON IO helpers
//
// Thin convenience wrappers over jsoncpp so call sites don't repeat the
// StreamWriterBuilder/CharReaderBuilder boilerplate. Pure refactor: behavior
// is identical to the inline jsoncpp calls these replace.
// =============================================================================

#include <json/json.h>

#include <sstream>
#include <string>

namespace cortex::mk3::json {

// Serialize a Json::Value to a string. `pretty=true` uses 2-space indentation.
inline std::string stringify(const Json::Value& v, bool pretty = false) {
    Json::StreamWriterBuilder w;
    w["indentation"] = pretty ? "  " : "";
    return Json::writeString(w, v);
}

// Parse a string. Throws std::runtime_error on failure.
inline Json::Value parse(const std::string& s) {
    Json::CharReaderBuilder r;
    std::string errs;
    Json::Value out;
    std::istringstream ss(s);
    if (!Json::parseFromStream(r, ss, &out, &errs))
        throw std::runtime_error("json::parse: " + errs);
    return out;
}

// Parse a string, returning `fallback` on failure instead of throwing.
inline Json::Value tryParse(const std::string& s, Json::Value fallback = Json::Value()) {
    try {
        return parse(s);
    } catch (...) {
        return fallback;
    }
}

// Standard error-result envelope: {"success": false, "error": "<msg>"}
inline Json::Value error(const std::string& msg) {
    Json::Value v(Json::objectValue);
    v["success"] = false;
    v["error"] = msg;
    return v;
}

// Standard success envelope: {"success": true, "data": <data>}
inline Json::Value ok(const Json::Value& data = Json::Value(Json::objectValue)) {
    Json::Value v(Json::objectValue);
    v["success"] = true;
    v["data"] = data;
    return v;
}

}  // namespace cortex::mk3::json
