#pragma once
// =============================================================================
// agent-lib-MK3 — Tools Dispatch
// Public API for tool execution.
// =============================================================================

#include "registry.hpp"
#include <json/json.h>
#include <string>

namespace cortex::mk3::tools {

// Register all built-in tools (idempotent)
void registerDefaults();

// Dispatch a tool call by name — resolves aliases automatically
std::string dispatch(const std::string& toolName, const Json::Value& params);

} // namespace cortex::mk3::tools
