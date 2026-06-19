// src/core/builtins/context_common.hpp — shared helpers for Agent-owned context builtins
#pragma once

#include <filesystem>
#include <string>

#include <json/json.h>

namespace cortex::mk3::agent_builtins {

inline std::string canonicaliseContextKey(const std::string& path) {
    std::error_code ec;
    auto p = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
    if (ec || p.empty())
        return std::filesystem::absolute(path).lexically_normal().string();
    return p.string();
}

inline Json::Value contextErr(const std::string& msg) {
    Json::Value r;
    r["success"] = false;
    r["error"] = msg;
    return r;
}

}  // namespace cortex::mk3::agent_builtins
