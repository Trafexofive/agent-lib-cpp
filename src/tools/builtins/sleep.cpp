// src/tools/builtins/sleep.cpp — bounded native sleep builtin
#include "builtins.hpp"
#include "common.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace cortex::mk3::tools::builtins {

static int durationMs(const Json::Value& p) {
    if (p.isMember("ms"))
        return p["ms"].asInt();
    if (p.isMember("milliseconds"))
        return p["milliseconds"].asInt();
    if (p.isMember("seconds"))
        return static_cast<int>(p["seconds"].asDouble() * 1000.0);
    if (p.isMember("duration")) {
        const auto& d = p["duration"];
        if (d.isInt() || d.isUInt())
            return d.asInt();
        if (d.isDouble())
            return static_cast<int>(d.asDouble() * 1000.0);
        if (d.isString()) {
            std::string s = d.asString();
            try {
                if (s.size() > 2 && s.substr(s.size() - 2) == "ms")
                    return std::stoi(s.substr(0, s.size() - 2));
                if (!s.empty() && s.back() == 's')
                    return static_cast<int>(std::stod(s.substr(0, s.size() - 1)) * 1000.0);
                return std::stoi(s);
            } catch (...) {
                return -1;
            }
        }
    }
    return 1000;
}

std::string sleep(const Json::Value& p) {
    int ms = durationMs(p);
    int maxMs = p.get("max_ms", 30000).asInt();
    maxMs = std::clamp(maxMs, 1, 300000);
    if (ms < 0)
        return jsonErr("invalid duration");
    if (ms > maxMs)
        return jsonErr("duration " + std::to_string(ms) + "ms exceeds max_ms " + std::to_string(maxMs));

    auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

    Json::Value r;
    r["success"] = true;
    r["requested_ms"] = ms;
    r["elapsed_ms"] = static_cast<Json::Int64>(elapsed);
    r["reason"] = p.get("reason", "").asString();
    r["output"] = "slept " + std::to_string(elapsed) + "ms";
    return jsonStr(r);
}

}  // namespace cortex::mk3::tools::builtins
