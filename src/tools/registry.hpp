#pragma once
// =============================================================================
// agent-lib-MK3 — Tool Registry
// Holds sovereign Tool objects. Backward-compatible with raw callback API.
// =============================================================================

#include <json/json.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "tool.hpp"

namespace cortex::mk3::tools {

std::string dispatch(const std::string& toolName, const Json::Value& params);
void registerDefaults();

// ── Convenience macro for static auto-registration ───────────────────────
#define REGISTER_TOOL(identifier, func_ptr)               \
    static bool ANONYMOUS_VAR(auto_register_##func_ptr) = \
        cortex::mk3::tools::ToolRegistry::instance().registerFunction(identifier, func_ptr)

#define ANONYMOUS_VAR(str) ANONYMOUS_CONCAT(str, __LINE__)
#define ANONYMOUS_CONCAT(str, line) ANONYMOUS_CONCAT_(str, line)
#define ANONYMOUS_CONCAT_(str, line) str##line

// ═══════════════════════════════════════════════════════════════════════════
// ToolRegistry — singleton registry of sovereign Tool objects
// ═══════════════════════════════════════════════════════════════════════════
class ToolRegistry {
   public:
    // ── Singleton ──
    static ToolRegistry& instance() {
        static ToolRegistry reg;
        return reg;
    }

    // ── Registration (new API) ──

    /// Register a sovereign Tool object.
    /// Returns true on success, false if a tool with this name already exists.
    bool registerTool(const Tool& tool) {
        if (tool.name().empty())
            return false;
        auto it = tools_.find(tool.name());
        if (it != tools_.end())
            return false;  // already registered
        tools_[tool.name()] = tool;
        return true;
    }

    /// Register a Tool by moving it in.
    bool registerTool(Tool&& tool) {
        if (tool.name().empty())
            return false;
        auto it = tools_.find(tool.name());
        if (it != tools_.end())
            return false;
        tools_[tool.name()] = std::move(tool);
        return true;
    }

    /// Register or replace — overwrites if exists
    bool registerOrReplace(const Tool& tool) {
        if (tool.name().empty())
            return false;
        tools_[tool.name()] = tool;
        return true;
    }

    // ── Registration (backward-compat API) ──

    /// Register a C++ function as a tool (creates a Tool internally).
    /// For full control (params, description), use registerTool() instead.
    bool registerFunction(const std::string& name, ToolCallback cb,
                          const std::string& description = "",
                          const std::vector<ToolParam>& params = {}) {
        // Check if already registered — if so, just update the callback
        auto it = tools_.find(name);
        if (it != tools_.end()) {
            // Create a new Tool with updated callback but keep existing definition
            // (We can't easily swap the callback on an existing Tool, so re-register)
            ToolDef def = it->second.definition();
            if (!description.empty())
                def.description = description;
            if (!params.empty())
                def.params = params;
            tools_[name] = Tool(def, std::move(cb));
            return true;
        }

        ToolDef def;
        def.name = name;
        def.description = description;
        def.params = params;
        def.isNative = true;
        Tool tool(def, std::move(cb));
        tools_[name] = std::move(tool);
        return true;
    }

    /// Register a stream-aware native function. Legacy callers can still execute
    /// it without a stream callback; Agent dispatch passes one when the TUI is live.
    bool registerStreamingFunction(const std::string& name, StreamingToolCallback cb,
                                   const std::string& description = "",
                                   const std::vector<ToolParam>& params = {}) {
        ToolDef def;
        auto it = tools_.find(name);
        if (it != tools_.end())
            def = it->second.definition();
        def.name = name;
        if (!description.empty())
            def.description = description;
        if (!params.empty())
            def.params = params;
        def.isNative = true;
        tools_[name] = Tool(def, std::move(cb));
        return true;
    }

    /// Legacy alias
    bool registerFn(const std::string& name, ToolCallback cb, const std::string& description = "",
                    const std::vector<ToolParam>& params = {}) {
        return registerFunction(name, std::move(cb), description, params);
    }

    bool registerStreamingFn(const std::string& name, StreamingToolCallback cb,
                             const std::string& description = "",
                             const std::vector<ToolParam>& params = {}) {
        return registerStreamingFunction(name, std::move(cb), description, params);
    }

    // ── Lookup ──

    /// Find a tool by name. Returns nullptr if not found.
    const Tool* findTool(const std::string& name) const {
        auto it = tools_.find(name);
        return (it != tools_.end()) ? &it->second : nullptr;
    }

    /// Get mutable pointer (for registration-time setup)
    Tool* findToolMut(const std::string& name) {
        auto it = tools_.find(name);
        return (it != tools_.end()) ? &it->second : nullptr;
    }

    /// Get the execution callback for a tool (backward compat).
    /// Returns nullptr if not found or if the tool has no native callback.
    ToolCallback get(const std::string& name) const {
        auto it = tools_.find(name);
        if (it == tools_.end())
            return nullptr;
        // For backward compat, wrap execute() into a callback
        const Tool& tool = it->second;
        if (tool.isNative()) {
            // We can't extract the original callback, but we can delegate
            return [&tool](const Json::Value& args) -> std::string { return tool.execute(args); };
        }
        // Script tools: wrap via execute
        return [tool](const Json::Value& args) -> std::string { return tool.execute(args); };
    }

    // ── Checks ──

    /// Check if a tool is registered.
    bool has(const std::string& name) const {
        return tools_.find(name) != tools_.end();
    }

    // ── Listing ──

    /// List all registered tool names.
    std::vector<std::string> listRegistered() const {
        std::vector<std::string> names;
        for (const auto& [name, _] : tools_)
            names.push_back(name);
        return names;
    }

    /// Get all registered tools
    std::vector<const Tool*> listTools() const {
        std::vector<const Tool*> tools;
        for (const auto& [_, tool] : tools_)
            tools.push_back(&tool);
        return tools;
    }

    /// Legacy alias
    std::vector<std::string> list() const {
        return listRegistered();
    }

    /// Get count of registered tools
    size_t count() const {
        return tools_.size();
    }

    // ── Delete copy/move ──
    ToolRegistry(const ToolRegistry&) = delete;
    ToolRegistry& operator=(const ToolRegistry&) = delete;
    ToolRegistry(ToolRegistry&&) = delete;
    ToolRegistry& operator=(ToolRegistry&&) = delete;

   private:
    ToolRegistry() = default;
    ~ToolRegistry() = default;
    std::map<std::string, Tool> tools_;
};

}  // namespace cortex::mk3::tools
