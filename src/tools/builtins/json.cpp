// src/tools/builtins/json.cpp — json native builtin
#include "builtins.hpp"
#include "common.hpp"

#include <cstdlib>
#include <sstream>

namespace cortex::mk3::tools::builtins {

std::string json(const Json::Value& p) {
    std::string op = p.get("op", "").asString();
    if (op.empty())
        op = p.get("action", "").asString();
    if (op.empty())
        return jsonErr("op is required (parse|query|validate|pretty|minify)");

    auto parse = [&](Json::Value& v, std::string& err) -> bool {
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
    };

    if (op == "parse") {
        Json::Value v;
        std::string err;
        if (!parse(v, err))
            return jsonErr("Invalid JSON: " + err);
        Json::Value out;
        out["success"] = true;
        out["value"] = v;
        return jsonStr(out);
    }

    if (op == "query") {
        Json::Value cur;
        std::string err;
        if (!parse(cur, err))
            return jsonErr("Invalid JSON: " + err);
        std::string path = p.get("path", "").asString();
        if (path.empty())
            path = p.get("query", "").asString();
        if (path.empty())
            return jsonErr("query path is required");

        std::istringstream parts(path);
        std::string seg;
        while (std::getline(parts, seg, '.')) {
            if (seg.empty())
                continue;
            if (cur.isObject()) {
                if (!cur.isMember(seg))
                    return jsonErr("query: key not found: " + seg);
                cur = cur[seg];
            } else if (cur.isArray()) {
                char* end = nullptr;
                long idx = std::strtol(seg.c_str(), &end, 10);
                if (!end || *end != '\0' || idx < 0 || idx >= (long)cur.size())
                    return jsonErr("query: invalid array index: " + seg);
                cur = cur[(Json::ArrayIndex)idx];
            } else {
                return jsonErr("query: cannot descend into scalar at: " + seg);
            }
        }
        Json::Value out;
        out["success"] = true;
        out["value"] = cur;
        return jsonStr(out);
    }

    if (op == "validate") {
        Json::Value v;
        std::string err;
        bool ok = parse(v, err);
        Json::Value out;
        out["valid"] = ok;
        if (!ok)
            out["errors"] = err;
        out["success"] = true;
        return jsonStr(out);
    }

    if (op == "pretty" || op == "minify") {
        Json::Value v;
        std::string err;
        if (!parse(v, err))
            return jsonErr("Invalid JSON: " + err);
        Json::StreamWriterBuilder w;
        w["indentation"] = op == "pretty" ? "  " : "";
        Json::Value out;
        out["success"] = true;
        out["formatted"] = Json::writeString(w, v);
        return jsonStr(out);
    }

    return jsonErr("Unknown json op: " + op);
}

}  // namespace cortex::mk3::tools::builtins
