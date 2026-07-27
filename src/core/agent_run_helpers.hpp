#pragma once
// Run-loop helpers peeled from agent.cpp (sub-agent expansion, result tags).

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "types.hpp"

namespace cortex::mk3 {

inline std::string stripModelOwnedRuntimeTags(const std::string &s) {
    static const std::regex responseRe(
        R"(<response\b[^>]*>[\s\S]*?</response>)");
    static const std::regex resultRe(R"(<result\b[^>]*>[\s\S]*?</result>)");
    return std::regex_replace(std::regex_replace(s, responseRe, ""), resultRe,
                              "");
}

inline std::string
formatDelegatedTrace(const std::string &agentName,
                     const std::string &instruction,
                     const std::vector<std::string> &prompts,
                     const std::vector<std::string> &outputs) {
    std::ostringstream os;
    os << "## Delegated Agent: " << agentName << "\n\n";
    os << "### INSTRUCTION\n\n" << instruction << "\n\n";
    for (size_t i = 0; i < prompts.size(); ++i) {
        os << "### SUB-ITERATION " << (i + 1) << " PROMPT\n\n";
        os << prompts[i] << "\n\n";
        if (i < outputs.size()) {
            os << "### SUB-ITERATION " << (i + 1) << " RESPONSE\n\n";
            os << outputs[i] << "\n\n";
        }
    }
    return os.str();
}

inline bool jsonBool(const Json::Value &params, const std::string &key,
                     bool def = false) {
    if (!params.isObject() || !params.isMember(key))
        return def;
    const Json::Value &v = params[key];
    if (v.isBool())
        return v.asBool();
    if (v.isString()) {
        std::string s = v.asString();
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s == "true" || s == "1" || s == "yes" || s == "on";
    }
    if (v.isNumeric())
        return v.asInt() != 0;
    return def;
}

inline Json::Value makeSubAgentResult(const std::string &output,
                                      const std::string &trace,
                                      bool dumpContext) {
    Json::Value r;
    r["success"] = true;
    r["output"] = output;
    if (dumpContext)
        r["trace"] = trace;
    return r;
}

inline std::vector<std::string> splitPath(const std::string &path) {
    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string item;
    while (std::getline(ss, item, '.')) {
        if (!item.empty())
            parts.push_back(item);
    }
    return parts;
}

inline const Json::Value *
lookupResultPath(const std::map<std::string, Json::Value> &results,
                 const std::string &id, const std::string &path) {
    auto it = results.find(id);
    if (it == results.end())
        return nullptr;
    const Json::Value *cur = &it->second;
    for (const auto &part : splitPath(path)) {
        if (cur->isObject() && cur->isMember(part)) {
            cur = &((*cur)[part]);
        } else if (cur->isArray()) {
            char *end = nullptr;
            long idx = std::strtol(part.c_str(), &end, 10);
            if (!end || *end != '\0' || idx < 0 || idx >= (long)cur->size())
                return nullptr;
            cur = &((*cur)[(Json::ArrayIndex)idx]);
        } else {
            return nullptr;
        }
    }
    return cur;
}

inline std::string jsonValueToInlineString(const Json::Value &v) {
    if (v.isString())
        return v.asString();
    if (v.isBool())
        return v.asBool() ? "true" : "false";
    if (v.isInt64())
        return std::to_string(v.asInt64());
    if (v.isUInt64())
        return std::to_string(v.asUInt64());
    if (v.isDouble()) {
        std::ostringstream os;
        os << v.asDouble();
        return os.str();
    }
    if (v.isNull())
        return "null";
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

inline std::string safeSessionPart(std::string s) {
    for (char &c : s) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok)
            c = '_';
    }
    return s;
}

inline bool subAgentSessionPersistenceEnabled(const std::string &value) {
    return value == "session" || value == "sessions" || value == "persistent" ||
           value == "disk" || value == "true";
}

inline std::string deriveSubAgentSessionId(const AgentContext &ctx,
                                           const AgentConfig &cfg,
                                           const std::string &agentName) {
    if (ctx.sessionId.empty())
        return "";
    if (!subAgentSessionPersistenceEnabled(cfg.subAgentPersistence))
        return "";
    return safeSessionPart(ctx.sessionId) + "__subagent__" +
           safeSessionPart(agentName);
}

inline Json::Value expansionResultView(const Json::Value &result) {
    Json::Value view = result;
    if (result.isMember("output") && result["output"].isString()) {
        Json::Value parsed;
        Json::CharReaderBuilder r;
        std::string errs;
        std::istringstream ss(result["output"].asString());
        if (Json::parseFromStream(r, ss, &parsed, &errs)) {
            view["json"] = parsed;
            if (parsed.isObject()) {
                for (const auto &key : parsed.getMemberNames()) {
                    if (!view.isMember(key))
                        view[key] = parsed[key];
                }
            }
        }
    }
    return view;
}

inline Json::Value
expandValueRefs(const Json::Value &value,
                const std::map<std::string, Json::Value> &results) {
    static const std::regex refRe(
        R"(\$\{([A-Za-z_][A-Za-z0-9_-]*)(?:\.([^}]+))?\})");

    if (value.isObject()) {
        Json::Value out(Json::objectValue);
        for (const auto &key : value.getMemberNames()) {
            out[key] = expandValueRefs(value[key], results);
        }
        return out;
    }
    if (value.isArray()) {
        Json::Value out(Json::arrayValue);
        for (Json::ArrayIndex i = 0; i < value.size(); ++i) {
            out.append(expandValueRefs(value[i], results));
        }
        return out;
    }
    if (!value.isString())
        return value;

    const std::string s = value.asString();
    std::smatch exact;
    if (std::regex_match(s, exact, refRe)) {
        std::string id = exact[1].str();
        std::string path = exact.size() > 2 ? exact[2].str() : "";
        const Json::Value *resolved = lookupResultPath(results, id, path);
        if (resolved)
            return *resolved;
        return value;
    }

    std::string out;
    std::string::const_iterator start = s.cbegin();
    std::smatch m;
    while (std::regex_search(start, s.cend(), m, refRe)) {
        out += m.prefix().str();
        std::string id = m[1].str();
        std::string path = m.size() > 2 ? m[2].str() : "";
        const Json::Value *resolved = lookupResultPath(results, id, path);
        out += resolved ? jsonValueToInlineString(*resolved) : m[0].str();
        start = m.suffix().first;
    }
    out.append(start, s.cend());
    return out;
}

inline std::string buildResultTag(const std::string &id,
                                  const Json::Value &result,
                                  bool compact = false) {
    std::ostringstream os;
    bool ok = result.isMember("success") && result["success"].asBool();
    int exit = result.isMember("exit_code") ? result["exit_code"].asInt()
                                            : (ok ? 0 : -1);
    double ms =
        result.isMember("_elapsed_ms") ? result["_elapsed_ms"].asDouble() : 0;

    os << "<result id=\"" << id << "\" ok=\"" << (ok ? "true" : "false")
       << "\"";
    if (exit != 0)
        os << " exit=\"" << exit << "\"";
    if (ms > 0)
        os << " ms=\"" << std::fixed << std::setprecision(1) << ms << "\"";

    // Extract primary output body
    std::string body;
    for (const char *key : {"content", "output", "stdout", "result", "results",
                            "data", "value"}) {
        if (!result.isMember(key))
            continue;
        if (result[key].isString()) {
            body = result[key].asString();
        } else {
            Json::StreamWriterBuilder w;
            w["indentation"] = "";
            body = Json::writeString(w, result[key]);
        }
        break;
    }
    if (body.empty() && result.isMember("error") && result["error"].isString())
        body = "error: " + result["error"].asString();
    // Structured context_* results: serialize compact JSON so the LLM still
    // sees path/bytes/cycles in <result> tags (not an empty body).
    if (body.empty() && result.isObject()) {
        Json::Value slim;
        for (const char *k :
             {"success", "path", "mode", "bytes", "cycles_remaining",
              "pinned_count", "peek_count", "note", "error", "keys"}) {
            if (result.isMember(k))
                slim[k] = result[k];
        }
        if (!slim.empty()) {
            Json::StreamWriterBuilder w;
            w["indentation"] = "";
            body = Json::writeString(w, slim);
        }
    }

    if (!body.empty()) {
        size_t bytes = body.size();
        if (compact && bytes > 2000) {
            body = body.substr(0, 2000);
            os << " bytes=\"" << bytes << "\" truncated=\"true\"";
        } else if (bytes > 0) {
            os << " bytes=\"" << bytes << "\"";
        }
        os << ">" << body << "</result>";
    } else {
        os << "/>";
    }
    return os.str();
}


}  // namespace cortex::mk3
