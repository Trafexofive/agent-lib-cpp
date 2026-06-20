#pragma once
// =============================================================================
// agent-lib-MK3 — Feed Sovereign Class
// Single-responsibility: a Feed owns its name, poll function, caching,
// manifest metadata, and result formatting.
// =============================================================================

#include <json/json.h>

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace cortex::mk3::feeds {

// ── Feed result ──
struct FeedResult {
    std::string name;
    std::string summary;
    std::string json;
    bool ok = true;
};

// ── Poll function signature ──
using FeedFn = std::function<FeedResult()>;

// ── Feed tool handler signature — receives params (Json::Value), returns
// a result (Json::Value). Used to reconfigure the feed resource itself
// (pin/unpin, watchdog url list, etc.) — distinct from a poll.
using FeedToolFn = std::function<Json::Value(const Json::Value&)>;

// ── Feed tool descriptor — exposed to the model via the <feeds> prompt block
struct FeedToolSpec {
    std::string name;
    std::string description;
};

// ═══════════════════════════════════════════════════════════════════════════
// Feed — sovereign class for one feed source
// ═══════════════════════════════════════════════════════════════════════════
class Feed {
   public:
    // ── Constructors ──

    /// Default — creates an invalid feed
    Feed() = default;

    /// Construct with name + poll function
    Feed(const std::string& name, FeedFn pollFn) : name_(name), pollFn_(std::move(pollFn)) {
    }

    /// Construct with name + poll function + initial poll
    Feed(const std::string& name, FeedFn pollFn, bool pollImmediate)
        : name_(name), pollFn_(std::move(pollFn)) {
        if (pollImmediate) {
            refresh();
        }
    }

    // ── Move constructor/assignment ──
    Feed(Feed&& other) noexcept
        : name_(std::move(other.name_)),
          pollFn_(std::move(other.pollFn_)),
          latest_(std::move(other.latest_)) {
    }

    Feed& operator=(Feed&& other) noexcept {
        if (this != &other) {
            name_ = std::move(other.name_);
            pollFn_ = std::move(other.pollFn_);
            latest_ = std::move(other.latest_);
        }
        return *this;
    }

    // ── No copying (mutex-protected state) ──
    Feed(const Feed&) = delete;
    Feed& operator=(const Feed&) = delete;

    // ── Accessors ──

    const std::string& name() const noexcept {
        return name_;
    }
    bool hasPollFn() const noexcept {
        return pollFn_ != nullptr;
    }
    bool isValid() const noexcept {
        return !name_.empty() && pollFn_ != nullptr;
    }

    // ── Polling ──

    /// Poll the feed, update cache, return result
    FeedResult poll() {
        FeedResult r;
        r.name = name_;
        if (!pollFn_) {
            r.ok = false;
            r.summary = "no poll function";
            return r;
        }
        try {
            r = pollFn_();
        } catch (const std::exception& e) {
            r = {name_, std::string("poll error: ") + e.what(), "{}", false};
        } catch (...) {
            r = {name_, "poll error: unknown", "{}", false};
        }

        // Update cache
        {
            std::lock_guard<std::mutex> lock(cacheMu_);
            latest_ = r;
            lastPoll_ = std::chrono::steady_clock::now();
        }

        return r;
    }

    /// Get cached result (if available), otherwise poll
    FeedResult get() {
        {
            std::lock_guard<std::mutex> lock(cacheMu_);
            if (latest_.has_value()) {
                return latest_.value();
            }
        }
        return poll();
    }

    /// Poll and cache fresh data
    FeedResult refresh() {
        return poll();
    }

    /// Get cached result without polling (returns empty if never polled)
    FeedResult cached() const {
        std::lock_guard<std::mutex> lock(cacheMu_);
        if (latest_.has_value())
            return latest_.value();
        return {name_, "", "{}", false};
    }

    // ── Tools — reconfigure the feed resource itself (pin/unpin, watchdog, …) ──

    /// Register a tool handler. Multiple tools per feed are allowed.
    void registerTool(const std::string& name, FeedToolFn handler) {
        std::lock_guard<std::mutex> lock(toolsMu_);
        tools_[name] = std::move(handler);
    }

    /// Register a tool descriptor (prompt-side metadata only).
    void registerToolSpec(const FeedToolSpec& spec) {
        std::lock_guard<std::mutex> lock(specsMu_);
        specs_[spec.name] = spec;
    }

    /// Check if a tool is registered.
    bool hasTool(const std::string& name) const {
        std::lock_guard<std::mutex> lock(toolsMu_);
        return tools_.count(name) > 0;
    }

    /// Call a tool. Returns {success, output, error} Json::Value.
    Json::Value callTool(const std::string& name, const Json::Value& params) {
        FeedToolFn fn;
        {
            std::lock_guard<std::mutex> lock(toolsMu_);
            auto it = tools_.find(name);
            if (it == tools_.end()) {
                Json::Value err;
                err["success"] = false;
                err["error"] = "feed has no tool: " + name_;
                return err;
            }
            fn = it->second;
        }
        Json::Value result;
        try {
            result = fn(params);
        } catch (const std::exception& e) {
            result["success"] = false;
            result["error"] = std::string("tool threw: ") + e.what();
        } catch (...) {
            result["success"] = false;
            result["error"] = "tool threw: unknown";
        }
        if (!result.isMember("success"))
            result["success"] = true;
        return result;
    }

    /// List registered tool specs (prompt-side metadata).
    std::vector<FeedToolSpec> toolSpecs() const {
        std::lock_guard<std::mutex> lock(specsMu_);
        std::vector<FeedToolSpec> out;
        out.reserve(specs_.size());
        for (const auto& [_, s] : specs_)
            out.push_back(s);
        return out;
    }

    /// Check if cache is fresh (polled within N seconds)
    bool isFresh(int maxAgeSecs = 5) const {
        std::lock_guard<std::mutex> lock(cacheMu_);
        if (!latest_.has_value())
            return false;
        auto age = std::chrono::steady_clock::now() - lastPoll_;
        return std::chrono::duration_cast<std::chrono::seconds>(age).count() < maxAgeSecs;
    }

    /// Clear the cache (forces next get() to poll)
    void clearCache() {
        std::lock_guard<std::mutex> lock(cacheMu_);
        latest_.reset();
    }

    // ── Formatting ──

    /// Format feed result into a Markdown snippet for prompt injection
    std::string formatForPrompt() const {
        std::lock_guard<std::mutex> lock(cacheMu_);
        if (!latest_.has_value() || !latest_->ok)
            return {};
        std::ostringstream ss;
        ss << "### " << name_ << "\n";
        if (!latest_->summary.empty())
            ss << latest_->summary << "\n";
        return ss.str();
    }

    /// Full result as JSON
    Json::Value toJson() const {
        std::lock_guard<std::mutex> lock(cacheMu_);
        Json::Value j;
        j["name"] = name_;
        if (!latest_.has_value()) {
            j["ok"] = false;
            j["summary"] = "not polled yet";
            return j;
        }
        j["ok"] = latest_->ok;
        j["summary"] = latest_->summary;
        // Parse and embed json field
        if (!latest_->json.empty()) {
            Json::Value parsed;
            Json::CharReaderBuilder r;
            std::string errs;
            std::istringstream ss(latest_->json);
            if (Json::parseFromStream(r, ss, &parsed, &errs)) {
                j["data"] = parsed;
            }
        }
        return j;
    }

   private:
    std::string name_;
    FeedFn pollFn_;

    mutable std::mutex cacheMu_;
    std::optional<FeedResult> latest_;
    std::chrono::steady_clock::time_point lastPoll_;

    mutable std::mutex toolsMu_;
    std::map<std::string, FeedToolFn> tools_;

    mutable std::mutex specsMu_;
    std::map<std::string, FeedToolSpec> specs_;
};

}  // namespace cortex::mk3::feeds
