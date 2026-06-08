#pragma once
// =============================================================================
// agent-lib-MK3 — Tool Registry
// Upgraded from MK2 (src/tools/ToolRegistry.hpp) — copy/move deletion, macros.
// =============================================================================

#include "../core/types.hpp"
#include <json/json.h>
#include <string>
#include <functional>
#include <map>

namespace cortex::mk3::tools {

using ToolCallback = std::function<std::string(const Json::Value&)>;

class ToolRegistry {
public:
    // Singleton access (Meyers' singleton — thread-safe in C++11+)
    static ToolRegistry& instance() {
        static ToolRegistry reg;
        return reg;
    }

    // Register a C++ function as a tool. Returns true on success.
    bool registerFunction(const std::string& name, ToolCallback cb) {
        registry_[name] = std::move(cb);
        return true;
    }

    // Legacy alias — kept for backward compatibility
    bool registerFn(const std::string& name, ToolCallback cb) {
        return registerFunction(name, std::move(cb));
    }

    // Retrieve a callback by name. Returns nullptr if not found.
    ToolCallback get(const std::string& name) const {
        auto it = registry_.find(name);
        return (it != registry_.end()) ? it->second : nullptr;
    }

    // Check if a tool is registered.
    bool has(const std::string& name) const {
        return registry_.find(name) != registry_.end();
    }

    // List all registered tool names.
    std::vector<std::string> listRegistered() const {
        std::vector<std::string> names;
        for (auto& [k, _] : registry_) names.push_back(k);
        return names;
    }

    // Legacy alias — kept for backward compatibility
    std::vector<std::string> list() const {
        return listRegistered();
    }

    // Delete copy and move — singleton
    ToolRegistry(const ToolRegistry&) = delete;
    ToolRegistry& operator=(const ToolRegistry&) = delete;
    ToolRegistry(ToolRegistry&&) = delete;
    ToolRegistry& operator=(ToolRegistry&&) = delete;

private:
    ToolRegistry() = default;
    ~ToolRegistry() = default;
    std::map<std::string, ToolCallback> registry_;
};

// ── Convenience macro for static auto-registration ───────────────────────
// Usage: REGISTER_TOOL("my_tool", myToolFunction);
#define REGISTER_TOOL(identifier, func_ptr) \
    static bool ANONYMOUS_VAR(auto_register_##func_ptr) = \
        cortex::mk3::tools::ToolRegistry::instance().registerFunction(identifier, func_ptr)

#define ANONYMOUS_VAR(str) ANONYMOUS_CONCAT(str, __LINE__)
#define ANONYMOUS_CONCAT(str, line) ANONYMOUS_CONCAT_(str, line)
#define ANONYMOUS_CONCAT_(str, line) str##line

} // namespace cortex::mk3::tools
