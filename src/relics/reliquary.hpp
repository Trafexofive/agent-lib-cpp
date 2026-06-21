#pragma once
// =============================================================================
// agent-lib-MK3 — Reliquary: unified relic registry
//
// A Reliquary is a single dispatch surface for all relic backends:
//   - In-process relics (C++ Relic subclasses, e.g. session journal)
//   - Docker-managed relics (HTTP-fronted services with lifecycle)
//   - Remote relics (HTTP-only, no lifecycle)
//
// The agent's dispatch path talks only to Reliquary. Adding a new relic
// backend means implementing the Relic interface and registering an
// instance — nothing else changes in the agent.
// =============================================================================

#include <json/json.h>

#include <memory>
#include <string>
#include <vector>

#include "relic.hpp"

namespace cortex::mk3::relics {

// ── Reliquary — singleton registry of all Relic instances ──
class Reliquary {
   public:
    static Reliquary& instance() {
        static Reliquary r;
        return r;
    }

    // Register a relic instance. If a relic with the same name is already
    // registered, the new instance is ignored (dedup) and false is returned.
    bool registerRelic(RelicPtr relic) {
        if (!relic || relic->name().empty())
            return false;
        if (has(relic->name()))
            return false;
        relics_.push_back(std::move(relic));
        return true;
    }

    // Find a relic by name. Returns nullptr if not found.
    Relic* find(const std::string& name) const {
        for (const auto& r : relics_) {
            if (r->name() == name)
                return r.get();
        }
        return nullptr;
    }

    bool has(const std::string& name) const {
        return find(name) != nullptr;
    }

    // Dispatch to a relic by name. Returns the same RelicResult the
    // underlying handle() returned. If the relic is unknown, returns
    // a fail("unknown relic") result.
    RelicResult dispatch(const std::string& name, const std::string& endpoint,
                         const Json::Value& params) const {
        Relic* r = find(name);
        if (!r)
            return RelicResult::fail("Unknown relic: " + name);
        return r->handle(endpoint, params);
    }

    // List all registered relic names.
    std::vector<std::string> names() const {
        std::vector<std::string> out;
        out.reserve(relics_.size());
        for (const auto& r : relics_)
            out.push_back(r->name());
        return out;
    }

    // Health check across all registered relics.
    std::map<std::string, bool> healthCheckAll() const {
        std::map<std::string, bool> status;
        for (const auto& r : relics_)
            status[r->name()] = r->isHealthy();
        return status;
    }

    // Test-only: clear all registered relics.
    void clear() {
        relics_.clear();
    }

   private:
    Reliquary() = default;
    std::vector<RelicPtr> relics_;
};

}  // namespace cortex::mk3::relics