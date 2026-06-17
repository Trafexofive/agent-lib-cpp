// ─────────────────────────────────────────────────────────────────────────────
// Built-in Relics — filesystem-backed persistence and checkpointing
// Now subclass the abstract Relic base (src/relics/relic.hpp).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <curl/curl.h>
#include <json/json.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "relic.hpp"

namespace cortex::mk3::relics {

namespace fs = std::filesystem;

// ── Base path resolution ──
inline fs::path relicBase() {
    const char* home = getenv("HOME");
    return home ? fs::path(home) / ".cortex" : fs::path("/tmp/.cortex");
}

// ═══════════════════════════════════════════════════════════════════════════
// SessionJournal — session event recording and querying
// ═══════════════════════════════════════════════════════════════════════════
class SessionJournal : public Relic {
   public:
    static SessionJournal& instance() {
        static SessionJournal j;
        return j;
    }

    // ── Relic interface ──
    const std::string& name() const override {
        static const std::string kName = "session_journal";
        return kName;
    }

    std::string description() const override {
        return "Session event recording and querying — filesystem-backed JSONL journal";
    }

    std::vector<std::string> endpoints() const override {
        return {"record", "query", "export", "prune"};
    }

    RelicResult handle(const std::string& endpoint, const Json::Value& params) override {
        std::string ns = params.get("namespace", "default").asString();

        if (endpoint == "record") {
            std::string et = params.get("event_type", "unknown").asString();
            std::string sid = params.get("session_id", "").asString();
            return record(ns, et, params["content"], sid);
        }
        if (endpoint == "query") {
            std::string sid = params.get("session_id", "").asString();
            std::string et = params.get("event_type", "").asString();
            int limit = params.get("limit", 100).asInt();
            return query(ns, sid, et, limit);
        }
        if (endpoint == "export") {
            return exportSession(ns, params.get("session_id", "").asString());
        }
        if (endpoint == "prune") {
            int age = params.get("max_age_seconds", 2592000).asInt();
            int maxRec = params.get("max_records", -1).asInt();
            return prune(ns, age, maxRec);
        }
        return RelicResult::fail("Unknown endpoint: " + endpoint);
    }

    // ── Business logic (unchanged) ──

    // POST /journal/{namespace}/record
    RelicResult record(const std::string& ns, const std::string& eventType,
                       const Json::Value& content, const std::string& sessionId = "") {
        auto dir = relicBase() / "sessions" / ns;
        std::error_code ec;
        fs::create_directories(dir, ec);

        Json::Value entry;
        entry["timestamp"] =
            (Json::Int64)std::chrono::system_clock::now().time_since_epoch().count();
        entry["event_type"] = eventType;
        entry["content"] = content;
        if (!sessionId.empty())
            entry["session_id"] = sessionId;

        // Append to journal file (one file per namespace per day)
        auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm;
        localtime_r(&t, &tm);
        char date[16];
        strftime(date, sizeof(date), "%Y-%m-%d", &tm);

        auto path = dir / (std::string(date) + ".jsonl");
        std::ofstream f(path, std::ios::app);
        if (!f)
            return {false, "Cannot open journal file", {}};

        Json::StreamWriterBuilder w;
        w["indentation"] = "";
        f << Json::writeString(w, entry) << "\n";

        RelicResult r;
        r.success = true;
        r.data["path"] = path.string();
        return r;
    }

    // GET /journal/{namespace} — query events
    RelicResult query(const std::string& ns, const std::string& sessionId = "",
                      const std::string& eventType = "", int limit = 100) {
        auto dir = relicBase() / "sessions" / ns;
        if (!fs::exists(dir))
            return {true, "", Json::Value(Json::arrayValue)};

        Json::Value results(Json::arrayValue);
        int count = 0;

        for (auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file())
                continue;
            std::ifstream f(entry.path());
            std::string line;
            while (std::getline(f, line) && count < limit) {
                if (line.empty())
                    continue;
                Json::Value ev;
                Json::CharReaderBuilder r;
                std::string errs;
                std::istringstream ss(line);
                if (!Json::parseFromStream(r, ss, &ev, &errs))
                    continue;

                // Filter
                if (!sessionId.empty() && ev.get("session_id", "").asString() != sessionId)
                    continue;
                if (!eventType.empty() && ev.get("event_type", "").asString() != eventType)
                    continue;

                results.append(ev);
                count++;
            }
        }

        RelicResult r;
        r.success = true;
        r.data = results;
        return r;
    }

    // GET /journal/{namespace}/export
    RelicResult exportSession(const std::string& ns, const std::string& sessionId) {
        auto result = query(ns, sessionId, "", 10000);
        if (!result.success)
            return result;

        Json::Value exportDoc;
        exportDoc["namespace"] = ns;
        exportDoc["session_id"] = sessionId;
        exportDoc["exported_at"] =
            (Json::Int64)std::chrono::system_clock::now().time_since_epoch().count();
        exportDoc["events"] = result.data;

        result.data = exportDoc;
        return result;
    }

    // DELETE /journal/{namespace}/prune
    RelicResult prune(const std::string& ns, int maxAgeSecs = 2592000, int maxRecords = -1) {
        (void)maxRecords;  // unused
        auto dir = relicBase() / "sessions" / ns;
        if (!fs::exists(dir))
            return {true, "Nothing to prune", {}};

        auto cutoff = std::chrono::system_clock::now() - std::chrono::seconds(maxAgeSecs);
        int pruned = 0;

        for (auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file())
                continue;
            auto ftime = fs::last_write_time(entry);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            if (sctp < cutoff) {
                fs::remove(entry.path());
                pruned++;
            }
        }

        RelicResult r;
        r.success = true;
        r.data["pruned"] = pruned;
        return r;
    }

   private:
    // Singleton — private constructor
    SessionJournal() = default;
    friend class RelicDispatcher;
};

// ═══════════════════════════════════════════════════════════════════════════
// StateCheckpoint — agent state serialization for crash recovery
// ═══════════════════════════════════════════════════════════════════════════
class StateCheckpoint : public Relic {
   public:
    static StateCheckpoint& instance() {
        static StateCheckpoint c;
        return c;
    }

    // ── Relic interface ──
    const std::string& name() const override {
        static const std::string kName = "state_checkpoint";
        return kName;
    }

    std::string description() const override {
        return "Agent state serialization for crash recovery — filesystem-backed JSON checkpoints";
    }

    std::vector<std::string> endpoints() const override {
        return {"save", "load", "latest", "list", "gc"};
    }

    RelicResult handle(const std::string& endpoint, const Json::Value& params) override {
        std::string ns = params.get("namespace", "default").asString();

        if (endpoint == "save") {
            return save(ns, params.get("agent_name", "agent").asString(), params["state"],
                        params.get("label", "").asString());
        }
        if (endpoint == "load" || endpoint == "latest") {
            return load(ns, params.get("agent_name", "agent").asString());
        }
        if (endpoint == "list") {
            return list(ns, params.get("agent_name", "").asString());
        }
        if (endpoint == "gc") {
            int age = params.get("max_age_seconds", 604800).asInt();
            int maxPer = params.get("max_per_agent", 5).asInt();
            return gc(ns, age, maxPer);
        }
        return RelicResult::fail("Unknown endpoint: " + endpoint);
    }

    // ── Business logic (unchanged) ──

    // POST /checkpoints/{namespace}/{agent_name}
    RelicResult save(const std::string& ns, const std::string& agentName, const Json::Value& state,
                     const std::string& label = "") {
        auto dir = relicBase() / "checkpoints" / ns / agentName;
        std::error_code ec;
        fs::create_directories(dir, ec);

        Json::Value checkpoint;
        checkpoint["agent_name"] = agentName;
        checkpoint["namespace"] = ns;
        checkpoint["timestamp"] =
            (Json::Int64)std::chrono::system_clock::now().time_since_epoch().count();
        checkpoint["state"] = state;
        if (!label.empty())
            checkpoint["label"] = label;

        auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm;
        localtime_r(&t, &tm);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);

        auto path = dir / (std::string("ckpt-") + ts + ".json");
        std::ofstream f(path);
        if (!f)
            return {false, "Cannot write checkpoint", {}};

        Json::StreamWriterBuilder w;
        w["indentation"] = "  ";
        f << Json::writeString(w, checkpoint);

        RelicResult r;
        r.success = true;
        r.data["path"] = path.string();
        return r;
    }

    // GET /checkpoints/{namespace}/{agent_name}/latest
    RelicResult load(const std::string& ns, const std::string& agentName) {
        auto dir = relicBase() / "checkpoints" / ns / agentName;
        if (!fs::exists(dir))
            return {false, "No checkpoints found", {}};

        // Find newest checkpoint file
        fs::path newest;
        auto newestTime = fs::file_time_type::min();
        for (auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().extension() != ".json")
                continue;
            auto t = fs::last_write_time(entry);
            if (t > newestTime) {
                newestTime = t;
                newest = entry.path();
            }
        }

        if (newest.empty())
            return {false, "No checkpoints found", {}};

        std::ifstream f(newest);
        if (!f)
            return {false, "Cannot read checkpoint", {}};

        Json::Value checkpoint;
        Json::CharReaderBuilder r;
        std::string errs;
        if (!Json::parseFromStream(r, f, &checkpoint, &errs))
            return {false, "Corrupt checkpoint: " + errs, {}};

        RelicResult res;
        res.success = true;
        res.data = checkpoint;
        return res;
    }

    // GET /checkpoints/{namespace} — list checkpoints
    RelicResult list(const std::string& ns, const std::string& agentName = "") {
        auto base = relicBase() / "checkpoints" / ns;
        if (!fs::exists(base))
            return {true, "", Json::Value(Json::arrayValue)};

        Json::Value results(Json::arrayValue);

        if (!agentName.empty()) {
            auto dir = base / agentName;
            if (!fs::exists(dir))
                return {true, "", Json::Value(Json::arrayValue)};
            for (auto& entry : fs::directory_iterator(dir)) {
                if (entry.path().extension() != ".json")
                    continue;
                Json::Value item;
                item["agent"] = agentName;
                item["file"] = entry.path().filename().string();
                results.append(item);
            }
        } else {
            for (auto& entry : fs::directory_iterator(base)) {
                if (!entry.is_directory())
                    continue;
                for (auto& ckpt : fs::directory_iterator(entry.path())) {
                    if (ckpt.path().extension() != ".json")
                        continue;
                    Json::Value item;
                    item["agent"] = entry.path().filename().string();
                    item["file"] = ckpt.path().filename().string();
                    results.append(item);
                }
            }
        }

        RelicResult r;
        r.success = true;
        r.data = results;
        return r;
    }

    // DELETE /checkpoints/{namespace}/gc
    RelicResult gc(const std::string& ns, int maxAgeSecs = 604800, int maxPerAgent = 5) {
        auto base = relicBase() / "checkpoints" / ns;
        if (!fs::exists(base))
            return {true, "Nothing to GC", {}};

        auto cutoff = std::chrono::system_clock::now() - std::chrono::seconds(maxAgeSecs);
        int removed = 0;

        for (auto& agentDir : fs::directory_iterator(base)) {
            if (!agentDir.is_directory())
                continue;

            // Collect checkpoints sorted by time
            std::vector<std::pair<fs::file_time_type, fs::path>> ckpts;
            for (auto& ckpt : fs::directory_iterator(agentDir.path())) {
                if (ckpt.path().extension() != ".json")
                    continue;
                ckpts.push_back({fs::last_write_time(ckpt), ckpt.path()});
            }
            std::sort(ckpts.begin(), ckpts.end(),
                      [](auto& a, auto& b) { return a.first > b.first; });

            // Keep maxPerAgent most recent, remove rest + old ones
            for (size_t i = 0; i < ckpts.size(); i++) {
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ckpts[i].first - fs::file_time_type::clock::now() +
                    std::chrono::system_clock::now());
                if (i >= (size_t)maxPerAgent || sctp < cutoff) {
                    fs::remove(ckpts[i].second);
                    removed++;
                }
            }
        }

        RelicResult r;
        r.success = true;
        r.data["removed"] = removed;
        return r;
    }

   private:
    StateCheckpoint() = default;
    friend class RelicDispatcher;
};

// ═══════════════════════════════════════════════════════════════════════════
// RelicDispatcher — routes <action type="relic"> calls using RelicPtr
// ═══════════════════════════════════════════════════════════════════════════
class RelicDispatcher {
   public:
    static RelicDispatcher& instance() {
        static RelicDispatcher d;
        return d;
    }

    // ── Registration ──

    /// Register a Relic by shared pointer (most common)
    void registerRelic(RelicPtr relic) {
        if (!relic)
            return;
        relics_[relic->name()] = std::move(relic);
    }

    /// Register a Relic by unique pointer (converts to shared)
    void registerRelic(std::unique_ptr<Relic> relic) {
        if (!relic)
            return;
        relics_[relic->name()] = std::move(relic);
    }

    /// Register an HTTP relic by base URL (legacy compat)
    void registerHttpRelic(const std::string& name, const std::string& baseUrl) {
        relicUrls_[name] = baseUrl;
    }

    // ── Dispatch ──

    /// Dispatch to the right relic by name.
    /// Falls back to HTTP relics for unknown names.
    RelicResult dispatch(const std::string& relicName, const std::string& endpoint,
                         const Json::Value& params) {
        // Check registered relic objects first
        auto it = relics_.find(relicName);
        if (it != relics_.end()) {
            return it->second->handle(endpoint, params);
        }

        // Fallback to built-in singletons (legacy direct access)
        if (relicName == "session_journal") {
            return SessionJournal::instance().handle(endpoint, params);
        }
        if (relicName == "state_checkpoint") {
            return StateCheckpoint::instance().handle(endpoint, params);
        }

        // Docker/HTTP relics — try to call via HTTP
        auto urlIt = relicUrls_.find(relicName);
        if (urlIt != relicUrls_.end()) {
            return dispatchHttp(urlIt->second, endpoint, params);
        }

        // Unknown
        return RelicResult::fail("Unknown relic: " + relicName);
    }

    /// Get a registered relic by name
    RelicPtr getRelic(const std::string& name) const {
        auto it = relics_.find(name);
        if (it != relics_.end())
            return it->second;
        return nullptr;
    }

    /// List all registered relic names
    std::vector<std::string> listRelics() const {
        std::vector<std::string> names;
        for (const auto& [name, _] : relics_)
            names.push_back(name);
        // Always include built-in relics
        names.push_back("session_journal");
        names.push_back("state_checkpoint");
        // Also list HTTP relics
        for (const auto& [name, _] : relicUrls_) {
            if (std::find(names.begin(), names.end(), name) == names.end())
                names.push_back(name);
        }
        return names;
    }

    /// Check if a relic is available
    bool hasRelic(const std::string& name) const {
        if (relics_.find(name) != relics_.end())
            return true;
        if (name == "session_journal" || name == "state_checkpoint")
            return true;
        return relicUrls_.find(name) != relicUrls_.end();
    }

   private:
    std::map<std::string, RelicPtr> relics_;
    std::map<std::string, std::string> relicUrls_;  // name → base_url (legacy)

    // ── HTTP dispatch (legacy — for Docker-based relics) ──

    RelicResult dispatchHttp(const std::string& baseUrl, const std::string& endpoint,
                             const Json::Value& params) {
        std::string url = baseUrl + "/" + endpoint;
        CURL* curl = curl_easy_init();
        if (!curl)
            return {false, "CURL init failed", {}};
        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(
            curl, CURLOPT_WRITEFUNCTION, +[](void* p, size_t s, size_t n, void* u) -> size_t {
                auto* b = static_cast<std::string*>(u);
                b->append(static_cast<char*>(p), s * n);
                return s * n;
            });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        if (!params.empty()) {
            Json::StreamWriterBuilder w;
            w["indentation"] = "";
            std::string body = Json::writeString(w, params);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
            struct curl_slist* h = nullptr;
            h = curl_slist_append(h, "Content-Type: application/json");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
        }
        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK)
            return {false, "Relic unreachable: " + url, {}};
        return {true, "", response};
    }
};

}  // namespace cortex::mk3::relics
