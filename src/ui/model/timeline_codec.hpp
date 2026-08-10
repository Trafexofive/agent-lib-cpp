#pragma once
// =============================================================================
// Timeline row types + JSON codec (foundation F2).
// Pure: protocol events + noise filter only. No Agent*, no Surface.
// =============================================================================

#include <json/json.h>

#include <memory>
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

// Action/result body presentation (Settings · BODY FMT carousel).
enum class BodyRenderMode : uint8_t { PrettyJson = 0, PrettyYaml = 1, Raw = 2 };

inline const char* bodyRenderModeName(BodyRenderMode m) {
    switch (m) {
        case BodyRenderMode::PrettyJson: return "json";
        case BodyRenderMode::PrettyYaml: return "yaml";
        case BodyRenderMode::Raw: return "raw";
    }
    return "json";
}

inline BodyRenderMode bodyRenderModeFromName(const std::string& s) {
    if (s == "yaml" || s == "pretty-yaml" || s == "pretty_yaml") return BodyRenderMode::PrettyYaml;
    if (s == "raw") return BodyRenderMode::Raw;
    if (s == "json" || s == "pretty" || s == "pretty-json" || s == "pretty_json")
        return BodyRenderMode::PrettyJson;
    // legacy compact → yaml (dense structured)
    if (s == "compact") return BodyRenderMode::PrettyYaml;
    return BodyRenderMode::PrettyJson;
}

inline bool tryParseJsonBody(const std::string& body, Json::Value& root) {
    size_t i = 0;
    while (i < body.size() &&
           (body[i] == ' ' || body[i] == '\t' || body[i] == '\n' || body[i] == '\r'))
        ++i;
    if (i >= body.size() || (body[i] != '{' && body[i] != '[')) return false;
    Json::CharReaderBuilder b;
    std::string errs;
    std::unique_ptr<Json::CharReader> r(b.newCharReader());
    return r->parse(body.data() + i, body.data() + body.size(), &root, &errs);
}

// Pretty JSON indent. Non-JSON unchanged.
inline std::string formatActionBodyPretty(const std::string& body) {
    Json::Value root;
    if (!tryParseJsonBody(body, root)) return body;
    Json::StreamWriterBuilder w;
    w["indentation"] = "  ";
    w["commentStyle"] = "None";
    return Json::writeString(w, root);
}

// Minimal JSON → YAML-ish (no external yaml lib). Good enough for tool params.
inline void jsonToYaml(const Json::Value& v, std::ostringstream& o, int indent) {
    auto pad = [&](int n) {
        for (int i = 0; i < n; ++i) o << "  ";
    };
    if (v.isObject()) {
        auto names = v.getMemberNames();
        if (names.empty()) {
            o << "{}";
            return;
        }
        bool first = true;
        for (const auto& k : names) {
            if (!first) o << '\n';
            first = false;
            pad(indent);
            o << k << ":";
            const Json::Value& c = v[k];
            if (c.isObject() || c.isArray()) {
                if ((c.isObject() && c.getMemberNames().empty()) ||
                    (c.isArray() && c.empty())) {
                    o << (c.isObject() ? " {}" : " []");
                } else {
                    o << '\n';
                    jsonToYaml(c, o, indent + 1);
                }
            } else if (c.isString()) {
                std::string s = c.asString();
                bool plain = !s.empty() && s.find_first_of(":#\n\r'\"") == std::string::npos;
                o << ' ';
                if (plain) o << s;
                else {
                    o << '"';
                    for (char ch : s) {
                        if (ch == '"' || ch == '\\') o << '\\';
                        if (ch == '\n') o << "\\n";
                        else if (ch != '\r') o << ch;
                    }
                    o << '"';
                }
            } else if (c.isBool()) {
                o << (c.asBool() ? " true" : " false");
            } else if (c.isNull()) {
                o << " null";
            } else if (c.isNumeric()) {
                o << ' ' << c.asString();
            } else {
                o << ' ' << c.toStyledString();
            }
        }
    } else if (v.isArray()) {
        if (v.empty()) {
            o << "[]";
            return;
        }
        for (Json::ArrayIndex i = 0; i < v.size(); ++i) {
            if (i) o << '\n';
            pad(indent);
            o << "-";
            const Json::Value& c = v[i];
            if (c.isObject() || c.isArray()) {
                o << '\n';
                jsonToYaml(c, o, indent + 1);
            } else if (c.isString()) {
                o << ' ' << c.asString();
            } else if (c.isBool()) {
                o << (c.asBool() ? " true" : " false");
            } else if (c.isNull()) {
                o << " null";
            } else if (c.isNumeric()) {
                o << ' ' << c.asString();
            }
        }
    } else if (v.isString()) {
        o << v.asString();
    } else if (v.isBool()) {
        o << (v.asBool() ? "true" : "false");
    } else if (v.isNull()) {
        o << "null";
    } else if (v.isNumeric()) {
        o << v.asString();
    }
}

// Convert XML <param name="k">v</param> blocks into a simple key: value map
// so tool action bodies display as readable YAML instead of raw XML.
inline std::string xmlParamsToYaml(const std::string& body) {
    std::ostringstream o;
    size_t pos = 0;
    bool any = false;
    while (true) {
        auto start = body.find("<param name=\"", pos);
        if (start == std::string::npos) start = body.find("<param name='", pos);
        if (start == std::string::npos) break;
        char quote = body[start + 12] == '"' ? '"' : '\'';
        auto nameEnd = body.find(quote, start + 13);
        if (nameEnd == std::string::npos) break;
        std::string key = body.substr(start + 13, nameEnd - start - 13);
        auto tagClose = body.find('>', nameEnd);
        if (tagClose == std::string::npos) break;
        auto valEnd = body.find("</param>", tagClose + 1);
        if (valEnd == std::string::npos) break;
        std::string val = body.substr(tagClose + 1, valEnd - tagClose - 1);
        // Trim whitespace
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\n' || s.front() == '\r' || s.front() == '\t'))
                s.erase(0, 1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\t'))
                s.pop_back();
        };
        trim(key);
        trim(val);
        if (!any) any = true;
        o << key << ": ";
        // Long values: indent continuation
        if (val.find('\n') != std::string::npos || val.size() > 60) {
            o << "|\n";
            std::istringstream lines(val);
            std::string line;
            while (std::getline(lines, line)) {
                o << "  " << line << '\n';
            }
        } else {
            o << val << '\n';
        }
        pos = valEnd + 8;
    }
    if (!any) return body;  // no XML params found
    return o.str();
}

inline std::string formatActionBodyYaml(const std::string& body) {
    // JSON first (most common), then XML params, then raw.
    Json::Value root;
    if (tryParseJsonBody(body, root)) {
        std::ostringstream o;
        jsonToYaml(root, o, 0);
        return o.str();
    }
    // XML <params> block — convert to key: value YAML.
    if (body.find("<param") != std::string::npos) {
        auto y = xmlParamsToYaml(body);
        if (y != body) return y;
    }
    return body;
}

// Short chips for the TOOL header line (path / command / url …).
inline std::string actionBodyMetaChips(const std::string& body, int maxChips = 3) {
    Json::Value root;
    if (!tryParseJsonBody(body, root) || !root.isObject()) return {};
    static const char* kKeys[] = {"path", "command", "cmd", "url", "pattern",
                                  "query", "name", "file", "cwd", "mode", "op"};
    std::string out;
    int n = 0;
    for (const char* key : kKeys) {
        if (!root.isMember(key)) continue;
        const Json::Value& v = root[key];
        if (!v.isString() && !v.isNumeric() && !v.isBool()) continue;
        std::string s = v.isString() ? v.asString() : v.asString();
        if (s.empty()) continue;
        // basename-ish for paths
        if (std::string(key) == "path" || std::string(key) == "file") {
            auto slash = s.find_last_of("/");
            if (slash != std::string::npos && slash + 1 < s.size())
                s = s.substr(slash + 1);
        }
        if (s.size() > 36) s = s.substr(0, 34) + "…";
        for (char& c : s)
            if (c == '\n' || c == '\r') c = ' ';
        if (!out.empty()) out += "  ·  ";
        out += s;
        if (++n >= maxChips) break;
    }
    return out;
}

inline std::string formatActionBodyForChat(const std::string& body,
                                           BodyRenderMode mode = BodyRenderMode::PrettyJson) {
    switch (mode) {
        case BodyRenderMode::Raw: return body;
        case BodyRenderMode::PrettyYaml: return formatActionBodyYaml(body);
        case BodyRenderMode::PrettyJson:
        default: return formatActionBodyPretty(body);
    }
}

// Result storage keeps near-raw summary + trailing meta line. Display reformats.
inline std::string formatResultBodyForChat(const ProtocolResult& res) {
    std::string body;
    if (protocol::looksLikeSymbolDump(res.summary)) {
        body = "[symbol dump · " + std::to_string(res.summary.size()) + " bytes · collapsed]";
    } else {
        body = res.summary;  // raw; projectOneRow applies BodyRenderMode
    }
    std::string meta;
    if (res.elapsedMs > 0)
        meta += std::to_string(static_cast<int>(res.elapsedMs)) + "ms";
    if (res.outputBytes > 0) {
        if (!meta.empty()) meta += " · ";
        meta += std::to_string(res.outputBytes) + "B";
    }
    if (res.exitCode != 0) {
        if (!meta.empty()) meta += " · ";
        meta += "exit " + std::to_string(res.exitCode);
    }
    if (!meta.empty()) {
        if (!body.empty()) body = meta + "\n" + body;
        else body = meta;
    }
    return body;
}

// Apply BodyRenderMode to a stored result body (meta first line preserved).
inline std::string formatStoredResultBody(const std::string& stored, BodyRenderMode mode) {
    if (stored.empty()) return stored;
    size_t nl = stored.find('\n');
    std::string meta, payload;
    // Heuristic: first line is meta if it looks like "12ms" / "12ms · 4B".
    if (nl != std::string::npos) {
        std::string first = stored.substr(0, nl);
        bool looksMeta = false;
        if (first.find("ms") != std::string::npos || first.find("exit ") != std::string::npos ||
            first.find(" bytes") != std::string::npos)
            looksMeta = true;
        if (looksMeta) {
            meta = first;
            payload = stored.substr(nl + 1);
        } else {
            payload = stored;
        }
    } else {
        payload = stored;
    }
    payload = formatActionBodyForChat(payload, mode);
    if (!meta.empty()) return meta + (payload.empty() ? "" : ("\n" + payload));
    return payload;
}

// ProtocolEvent → TimelineRow (no Agent tree; drillable resolved by caller).
inline TimelineRow rowFromProtocol(const ProtocolEvent& pe) {
    TimelineRow row;
    if (pe.kind == ProtocolEventKind::THOUGHT) {
        // HOT PATH: thoughts stream token-by-token. isThoughtNoise() runs a full
        // XML markup strip over the entire buffer — O(n) per token → UI death.
        // Only run the heavy strip on small/early payloads; large streams use the
        // cheap symbol-dump probe (early-exit scan) and let sanitize clamp later.
        const bool heavyCheck = pe.text.size() <= 512;
        if (protocol::looksLikeSymbolDump(pe.text) ||
            (heavyCheck && protocol::isThoughtNoise(pe.text))) {
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
        // Keep status text as-is for strict mirror (filters still apply in UI).
        row.kind = TimelineKind::Status;
        row.title = pe.text.rfind("[STEER]", 0) == 0       ? "steer"
                    : pe.text.rfind("[LIMIT]", 0) == 0     ? "limit"
                    : pe.text.rfind("[FINALIZE]", 0) == 0 ? "finalize"
                    : pe.text.rfind("[TIMEOUT]", 0) == 0  ? "timeout"
                    : pe.text.rfind("[ERROR]", 0) == 0    ? "error"
                                                            : "status";
        row.body = pe.text;
        row.ok = pe.text.find("⚠") == std::string::npos && pe.text.find("error") == std::string::npos
                 && pe.text.find("TIMEOUT") == std::string::npos;
    } else if (pe.kind == ProtocolEventKind::ACTION) {
        row.kind = TimelineKind::Action;
        row.actionType = pe.action.type;
        row.actionName = pe.action.name;
        row.actionId = pe.action.id;
        row.drillable = (pe.action.type == "agent" && !pe.action.name.empty());
        // Compact header — body holds params (pretty JSON when possible).
        row.title = pe.action.type + ":" + pe.action.name + " #" + pe.action.id;
        if (row.drillable) row.title += "  ↳ enter";
        // Keep near-raw; projectOneRow applies Pretty/Compact/Raw.
        row.body = pe.action.body;
    } else if (pe.kind == ProtocolEventKind::RESULT) {
        row.kind = TimelineKind::Result;
        row.ok = pe.result.ok;
        row.actionId = pe.result.id;
        row.actionName = pe.result.toolName;
        row.drillable = false;
        row.title = "#" + pe.result.id + " " + pe.result.toolName;
        row.body = formatResultBodyForChat(pe.result);
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
