#pragma once
// Run-loop helpers peeled from agent.cpp (sub-agent expansion, result tags).

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "types.hpp"

namespace cortex::mk3 {

// Collapse stream stutter: same action id emitted twice in one generation
// (hollow {} then full, or full then full). Keep the last body per id.
inline std::string collapseDuplicateActionTags(const std::string &s) {
    static const std::regex actRe(
        R"(<action[^>]*id="([^"]+)"[^>]*>[\s\S]*?</action>)",
        std::regex::ECMAScript);
    std::smatch m;
    struct Hit {
        size_t begin = 0;
        size_t end = 0;
        std::string id;
        std::string tag;
    };
    std::vector<Hit> hits;
    std::string::const_iterator it = s.cbegin();
    while (std::regex_search(it, s.cend(), m, actRe)) {
        Hit h;
        h.begin = static_cast<size_t>(m[0].first - s.cbegin());
        h.end = static_cast<size_t>(m[0].second - s.cbegin());
        h.id = m[1].str();
        h.tag = m[0].str();
        hits.push_back(std::move(h));
        it = m[0].second;
    }
    if (hits.size() < 2)
        return s;
    std::set<size_t> drop;
    std::map<std::string, size_t> lastIdx;
    for (size_t i = 0; i < hits.size(); ++i) {
        auto prev = lastIdx.find(hits[i].id);
        if (prev != lastIdx.end())
            drop.insert(prev->second);
        lastIdx[hits[i].id] = i;
    }
    if (drop.empty())
        return s;
    std::string rebuilt;
    size_t cursor = 0;
    for (size_t i = 0; i < hits.size(); ++i) {
        const auto &h = hits[i];
        if (h.begin > cursor)
            rebuilt.append(s, cursor, h.begin - cursor);
        if (!drop.count(i))
            rebuilt.append(h.tag);
        cursor = h.end;
    }
    if (cursor < s.size())
        rebuilt.append(s, cursor, std::string::npos);
    return rebuilt;
}

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

// Child returned foreign tool mesh / leaked plan soup — not a real answer.
inline bool looksLikeForeignToolSoup(const std::string &s) {
    if (s.size() < 40) return false;
    int hits = 0;
    if (s.find("<<<<<") != std::string::npos) ++hits;
    if (s.find("|path|") != std::string::npos || s.find("|list|") != std::string::npos)
        ++hits;
    if (s.find("tool fs_read") != std::string::npos ||
        s.find("tool list") != std::string::npos)
        ++hits;
    // Dense space-separated tool tokens on few lines (same class as UI hang).
    int nl = 0, sp = 0;
    for (char c : s) {
        if (c == '\n') ++nl;
        else if (c == ' ' || c == '\t') ++sp;
    }
    if (s.size() > 2000 && nl <= 4 && sp > 80) ++hits;
    return hits >= 2;
}

inline Json::Value makeSubAgentResult(const std::string &output,
                                      const std::string &trace,
                                      bool dumpContext) {
    Json::Value r;
    const bool soup = looksLikeForeignToolSoup(output);
    const bool empty = output.empty() ||
                       (output.find_first_not_of(" \t\n\r") == std::string::npos);
    r["success"] = !soup && !empty;
    r["output"] = output;
    if (soup) {
        r["protocol_error"] = true;
        r["error"] =
            "sub-agent returned foreign tool grammar / plan dump, not a final "
            "report — treat as failed child, do not wait or re-inspect as progress";
    } else if (empty) {
        r["error"] = "sub-agent returned empty output";
    }
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

// Truncate on a UTF-8 codepoint boundary (no mid-rune mojibake in history).
inline std::string utf8SafePrefix(const std::string &s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t i = maxBytes;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    if (i == 0) i = maxBytes;  // pathological; prefer hard cut over empty
    return s.substr(0, i);
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
                            "data", "value", "tree"}) {
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
    // Structured object results with NO known output key: keep the whole
    // payload instead of dropping it.
    // Whitelisting silently dropped those to a bare {"success":true}, so the
    // model never saw tool data. Serialize all non-metadata keys instead of
    // throwing the payload away. Metadata keys (success/exit_*/ms/_elapsed_/
    // stdout_/stderr_/signal) stay out of the body — they are runtime noise,
    // carried in the <result> attributes when meaningful.
    if (body.empty() && result.isObject()) {
        static const std::set<std::string> meta = {
            "success", "exit", "exit_code", "signal", "ms", "_elapsed_ms",
            "timed_out", "stdout_truncated", "stderr_truncated",
            "stdout", "stderr", "truncated", "error"};
        Json::Value slim;
        for (auto it = result.begin(); it != result.end(); ++it) {
            const std::string k = it.key().asString();
            if (meta.count(k))
                continue;
            slim[k] = *it;
        }
        if (!slim.empty()) {
            Json::StreamWriterBuilder w;
            w["indentation"] = "";
            body = Json::writeString(w, slim);
        }
    }

    if (!body.empty()) {
        const size_t bytes = body.size();
        // CARDINAL RULE: history/prompt must keep tool + subagent sources of
        // truth. compact=true used to mean "UI teaser" but was wrongly used on
        // history (2KB) — that starved parents of child scout reports.
        // Full path: only a pathological safety rail (512KiB).
        // Preview path (explicit): 4KB teaser for UI-only callers.
        const size_t kCap = compact ? size_t(4 * 1024) : size_t(512 * 1024);
        if (bytes > kCap) {
            body = utf8SafePrefix(body, kCap) +
                   "\n…[truncated safety — full payload retained in actionResults_]";
            os << " bytes=\"" << bytes << "\" truncated=\"true\"";
        } else {
            os << " bytes=\"" << bytes << "\"";
        }
        os << ">" << body << "</result>";
    } else {
        os << "/>";
    }
    return os.str();
}



inline std::string trimCopy(std::string s) {
    size_t a = s.find_first_not_of(" \t\n\r");
    if (a == std::string::npos)
        return std::string();
    size_t b = s.find_last_not_of(" \t\n\r");
    return s.substr(a, b - a + 1);
}

inline std::string pickSalvage(const std::string &raw, const std::string &responseBody) {
    // Prefer structured response body (non-final <response>) over raw stream.
    std::string r = trimCopy(responseBody);
    if (!r.empty())
        return r;
    return trimCopy(raw);
}

inline std::string buildRecoveryCorrection(const std::string &salvage, bool nonFinalResponse) {
std::ostringstream os;
os << "[PROTOCOL RECOVERY] Previous model output had no valid "
      "<response final=\"true\"> and no executable <action>.\n";
if (nonFinalResponse)
    os << "A <response> body was seen without final=\"true\". "
          "Re-emit it wrapped correctly.\n";
else
    os << "Bare / non-protocol text is invisible to the operator as a "
          "final answer.\n";
os << "\nSalvaged content — put this inside <response final=\"true\"> "
      "(edit if needed) OR continue with an <action>:\n"
      "----- BEGIN SALVAGE -----\n";
// Cap injection so a huge bare dump cannot blow the next prompt.
const size_t kMax = 12000;
if (salvage.size() > kMax) {
    os << salvage.substr(0, kMax) << "\n…[truncated "
       << (salvage.size() - kMax) << " bytes]";
} else {
    os << salvage;
}
os << "\n----- END SALVAGE -----\n\n"
      "Emit EXACTLY one of:\n"
      "  <response final=\"true\">…</response>\n"
      "  <action type=\"tool\" name=\"…\" id=\"…\">…</action>\n";
return os.str();
}

// Thought-only streak: model produced content but no <action> and no final.
// Multi-thought in ONE generation is fine; N consecutive generations without
// tools/final burns API cost and is almost always a re-plan loop.
inline std::string buildThoughtOnlyNudge(int streak, int softCap) {
    // Inline XML — never English prose. The model sees what it should emit.
    std::ostringstream os;
    os << "<thought>" << streak << "/" << softCap
       << " consecutive turns with only thoughts — no actions, no final. "
          "THIS turn must emit either an <action> or "
          "<response final=\"true\">. Do not restate the plan."
          "</thought>";
    return os.str();
}

inline std::string buildThoughtOnlyHardStop(int streak) {
    std::ostringstream os;
    os << "[THOUGHT-ONLY HARD STOP] " << streak
       << " consecutive generations without tools or final. "
          "Loop aborted to protect budget. "
          "Re-run with a tighter prompt or a stronger model.";
    return os.str();
}

}  // namespace cortex::mk3
