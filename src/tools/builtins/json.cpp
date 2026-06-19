// src/tools/builtins/json.cpp — json native builtin
#include "builtins.hpp"
#include "common.hpp"

#include <cstdlib>
#include <sstream>

namespace cortex::mk3::tools::builtins {

static bool parseInput(const Json::Value& p, Json::Value& v, std::string& err) {
    if (!p.isMember("data")) {
        err = "data is required";
        return false;
    }
    const Json::Value& dataVal = p["data"];
    if (!dataVal.isString()) {
        v = dataVal;
        return true;
    }
    Json::CharReaderBuilder r;
    std::istringstream ss(dataVal.asString());
    return Json::parseFromStream(r, ss, &v, &err);
}

static std::string unescapePointerToken(std::string s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '~' && i + 1 < s.size()) {
            if (s[i + 1] == '0') {
                out += '~';
                ++i;
                continue;
            }
            if (s[i + 1] == '1') {
                out += '/';
                ++i;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

static bool descend(Json::Value& cur, const std::string& seg, std::string& err) {
    if (cur.isObject()) {
        if (!cur.isMember(seg)) {
            err = "key not found: " + seg;
            return false;
        }
        cur = cur[seg];
        return true;
    }
    if (cur.isArray()) {
        char* end = nullptr;
        long idx = std::strtol(seg.c_str(), &end, 10);
        if (!end || *end != '\0' || idx < 0 || idx >= (long)cur.size()) {
            err = "invalid array index: " + seg;
            return false;
        }
        cur = cur[(Json::ArrayIndex)idx];
        return true;
    }
    err = "cannot descend into scalar at: " + seg;
    return false;
}

static bool queryValue(Json::Value root, const std::string& path, Json::Value& out, std::string& err) {
    if (path.empty() || path == "." || path == "/") {
        out = root;
        return true;
    }
    if (!path.empty() && path[0] == '/') {
        size_t pos = 1;
        while (pos <= path.size()) {
            size_t slash = path.find('/', pos);
            std::string seg = unescapePointerToken(path.substr(pos, slash == std::string::npos ? slash : slash - pos));
            if (!descend(root, seg, err))
                return false;
            if (slash == std::string::npos)
                break;
            pos = slash + 1;
        }
        out = root;
        return true;
    }
    std::istringstream parts(path);
    std::string seg;
    while (std::getline(parts, seg, '.')) {
        if (seg.empty())
            continue;
        if (!descend(root, seg, err))
            return false;
    }
    out = root;
    return true;
}

static std::string valueType(const Json::Value& v) {
    if (v.isObject())
        return "object";
    if (v.isArray())
        return "array";
    if (v.isString())
        return "string";
    if (v.isBool())
        return "boolean";
    if (v.isNumeric())
        return "number";
    if (v.isNull())
        return "null";
    return "unknown";
}

std::string json(const Json::Value& p) {
    std::string op = p.get("op", p.get("action", "").asString()).asString();
    if (op.empty())
        return jsonErr("op is required (parse|query|validate|pretty|minify|keys|length|type)");

    if (op == "validate") {
        Json::Value v;
        std::string err;
        bool ok = parseInput(p, v, err);
        Json::Value out;
        out["success"] = true;
        out["valid"] = ok;
        if (!ok)
            out["errors"] = err;
        return jsonStr(out);
    }

    Json::Value root;
    std::string err;
    if (!parseInput(p, root, err))
        return jsonErr("Invalid JSON: " + err);

    if (op == "parse") {
        Json::Value out;
        out["success"] = true;
        out["value"] = root;
        out["type"] = valueType(root);
        return jsonStr(out);
    }

    if (op == "pretty" || op == "minify") {
        Json::StreamWriterBuilder w;
        w["indentation"] = op == "pretty" ? "  " : "";
        Json::Value out;
        out["success"] = true;
        out["formatted"] = Json::writeString(w, root);
        return jsonStr(out);
    }

    std::string path = p.get("path", p.get("query", "").asString()).asString();
    Json::Value selected;
    if (!queryValue(root, path, selected, err))
        return jsonErr("query: " + err);

    Json::Value out;
    out["success"] = true;
    if (op == "query") {
        out["value"] = selected;
        out["type"] = valueType(selected);
        return jsonStr(out);
    }
    if (op == "type") {
        out["type"] = valueType(selected);
        return jsonStr(out);
    }
    if (op == "length") {
        if (selected.isArray() || selected.isObject() || selected.isString())
            out["length"] = static_cast<Json::UInt64>(selected.size());
        else
            out["length"] = 0;
        return jsonStr(out);
    }
    if (op == "keys") {
        Json::Value keys(Json::arrayValue);
        if (!selected.isObject())
            return jsonErr("keys: selected value is not an object");
        for (const auto& name : selected.getMemberNames())
            keys.append(name);
        out["keys"] = keys;
        return jsonStr(out);
    }

    return jsonErr("Unknown json op: " + op);
}

}  // namespace cortex::mk3::tools::builtins
