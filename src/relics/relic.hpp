#pragma once
// =============================================================================
// agent-lib-MK3 — Relic Abstract Base Class
// Single-responsibility: a Relic is a named service endpoint with standard
// request/response semantics. Concrete relics implement specific backends
// (session journal, state checkpoint, HTTP proxy, Docker containers).
// =============================================================================

#include <json/json.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cortex::mk3::relics {

// ── Relic result ──
struct RelicResult {
    bool success = false;
    std::string error;
    Json::Value data;

    static RelicResult ok() {
        return {true, "", Json::Value()};
    }
    static RelicResult ok(Json::Value d) {
        return {true, "", std::move(d)};
    }
    static RelicResult fail(const std::string& err) {
        return {false, err, Json::Value()};
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Relic — abstract base class for all relic types
// ═══════════════════════════════════════════════════════════════════════════
class Relic {
   public:
    virtual ~Relic() = default;

    /// Unique name for this relic (matches manifest name)
    virtual const std::string& name() const = 0;

    /// Human-readable description
    virtual std::string description() const {
        return {};
    }

    /// List of supported endpoints (e.g., "record", "query", "save", "load")
    virtual std::vector<std::string> endpoints() const = 0;

    /// Handle a request. `endpoint` is the operation name (e.g., "record"),
    /// `params` contains the request parameters.
    virtual RelicResult handle(const std::string& endpoint, const Json::Value& params) = 0;

    /// Health check — returns true if the relic is operational
    virtual bool isHealthy() const {
        return true;
    }

    /// Relic metadata as JSON
    virtual Json::Value toJson() const {
        Json::Value j;
        j["name"] = name();
        j["description"] = description();
        Json::Value eps(Json::arrayValue);
        for (const auto& e : endpoints())
            eps.append(e);
        j["endpoints"] = eps;
        return j;
    }
};

// ── Shared pointer alias ──
using RelicPtr = std::shared_ptr<Relic>;

/// Helper: create a RelicResult from a JSON string response
inline RelicResult relicResultFromJson(const std::string& jsonStr) {
    if (jsonStr.empty())
        return RelicResult::fail("empty response");
    Json::Value parsed;
    Json::CharReaderBuilder r;
    std::string errs;
    std::istringstream ss(jsonStr);
    if (!Json::parseFromStream(r, ss, &parsed, &errs))
        return RelicResult::fail("cannot parse response: " + errs);
    RelicResult res;
    res.success = parsed.get("success", false).asBool();
    res.error = parsed.get("error", "").asString();
    if (parsed.isMember("data"))
        res.data = parsed["data"];
    if (res.data.isNull() && parsed.isMember("result"))
        res.data = parsed["result"];
    return res;
}

}  // namespace cortex::mk3::relics
