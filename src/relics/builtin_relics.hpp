// ─────────────────────────────────────────────────────────────────────────────
// Built-in Relics — filesystem-backed persistence and checkpointing
// No Docker, no network — direct filesystem from the runtime.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <json/json.h>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace cortex {
namespace mk3 {
namespace relics {

namespace fs = std::filesystem;

// ── Relic result ──
struct RelicResult {
    bool success = false;
    std::string error;

    Json::Value data;
};

// ── Base path resolution ──
inline fs::path relicBase() {
    const char *home = getenv("HOME");
    return home ? fs::path(home) / ".cortex" : fs::path("/tmp/.cortex");
}

// ═══════════════════════════════════════════════════════════════════════════
// session_journal — session event recording and querying
// ═══════════════════════════════════════════════════════════════════════════
class SessionJournal {
public:
    static SessionJournal &instance() {
        static SessionJournal j;
        return j;
    }

    // POST /journal/{namespace}/record
    RelicResult record(const std::string &ns, const std::string &eventType,
                       const Json::Value &content,
                       const std::string &sessionId = "") {
        auto dir = relicBase() / "sessions" / ns;
        std::error_code ec;
        fs::create_directories(dir, ec);

        Json::Value entry;
        entry["timestamp"] = (Json::Int64)std::chrono::system_clock::now()
                                 .time_since_epoch()
                                 .count();
        entry["event_type"] = eventType;
        entry["content"] = content;
        if (!sessionId.empty())
            entry["session_id"] = sessionId;

        // Append to journal file (one file per namespace per day)
        auto t = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
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
    RelicResult query(const std::string &ns, const std::string &sessionId = "",
                      const std::string &eventType = "", int limit = 100) {
        auto dir = relicBase() / "sessions" / ns;
        if (!fs::exists(dir))
            return {true, "", Json::Value(Json::arrayValue)};

        Json::Value results(Json::arrayValue);
        int count = 0;

        for (auto &entry : fs::directory_iterator(dir)) {
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
                if (!sessionId.empty() &&
                    ev.get("session_id", "").asString() != sessionId)
                    continue;
                if (!eventType.empty() &&
                    ev.get("event_type", "").asString() != eventType)
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
    RelicResult exportSession(const std::string &ns,
                              const std::string &sessionId) {
        auto result = query(ns, sessionId, "", 10000);
        if (!result.success)
            return result;

        Json::Value exportDoc;
        exportDoc["namespace"] = ns;
        exportDoc["session_id"] = sessionId;
        exportDoc["exported_at"] = (Json::Int64)std::chrono::system_clock::now()
                                       .time_since_epoch()
                                       .count();
        exportDoc["events"] = result.data;

        result.data = exportDoc;
        return result;
    }

    // DELETE /journal/{namespace}/prune
    RelicResult prune(const std::string &ns, int maxAgeSecs = 2592000,
                      int maxRecords = -1) {
        auto dir = relicBase() / "sessions" / ns;
        if (!fs::exists(dir))
            return {true, "Nothing to prune", {}};

        auto cutoff =
            std::chrono::system_clock::now() - std::chrono::seconds(maxAgeSecs);
        int pruned = 0;

        for (auto &entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file())
                continue;
            auto ftime = fs::last_write_time(entry);
            auto sctp = std::chrono::time_point_cast<
                std::chrono::system_clock::duration>(
                ftime - fs::file_time_type::clock::now() +
                std::chrono::system_clock::now());
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
};

// ═══════════════════════════════════════════════════════════════════════════
// state_checkpoint — agent state serialization for crash recovery
// ═══════════════════════════════════════════════════════════════════════════
class StateCheckpoint {
public:
    static StateCheckpoint &instance() {
        static StateCheckpoint c;
        return c;
    }

    // POST /checkpoints/{namespace}/{agent_name}
    RelicResult save(const std::string &ns, const std::string &agentName,
                     const Json::Value &state, const std::string &label = "") {
        auto dir = relicBase() / "checkpoints" / ns / agentName;
        std::error_code ec;
        fs::create_directories(dir, ec);

        Json::Value checkpoint;
        checkpoint["agent_name"] = agentName;
        checkpoint["namespace"] = ns;
        checkpoint["timestamp"] = (Json::Int64)std::chrono::system_clock::now()
                                      .time_since_epoch()
                                      .count();
        checkpoint["state"] = state;
        if (!label.empty())
            checkpoint["label"] = label;

        auto t = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
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
    RelicResult load(const std::string &ns, const std::string &agentName) {
        auto dir = relicBase() / "checkpoints" / ns / agentName;
        if (!fs::exists(dir))
            return {false, "No checkpoints found", {}};

        // Find newest checkpoint file
        fs::path newest;
        auto newestTime = fs::file_time_type::min();
        for (auto &entry : fs::directory_iterator(dir)) {
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
    RelicResult list(const std::string &ns, const std::string &agentName = "") {
        auto base = relicBase() / "checkpoints" / ns;
        if (!fs::exists(base))
            return {true, "", Json::Value(Json::arrayValue)};

        Json::Value results(Json::arrayValue);

        if (!agentName.empty()) {
            auto dir = base / agentName;
            if (!fs::exists(dir))
                return {true, "", Json::Value(Json::arrayValue)};
            for (auto &entry : fs::directory_iterator(dir)) {
                if (entry.path().extension() != ".json")
                    continue;
                Json::Value item;
                item["agent"] = agentName;
                item["file"] = entry.path().filename().string();
                results.append(item);
            }
        } else {
            for (auto &entry : fs::directory_iterator(base)) {
                if (!entry.is_directory())
                    continue;
                for (auto &ckpt : fs::directory_iterator(entry.path())) {
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
    RelicResult gc(const std::string &ns, int maxAgeSecs = 604800,
                   int maxPerAgent = 5) {
        auto base = relicBase() / "checkpoints" / ns;
        if (!fs::exists(base))
            return {true, "Nothing to GC", {}};

        auto cutoff =
            std::chrono::system_clock::now() - std::chrono::seconds(maxAgeSecs);
        int removed = 0;

        for (auto &agentDir : fs::directory_iterator(base)) {
            if (!agentDir.is_directory())
                continue;

            // Collect checkpoints sorted by time
            std::vector<std::pair<fs::file_time_type, fs::path>> ckpts;
            for (auto &ckpt : fs::directory_iterator(agentDir.path())) {
                if (ckpt.path().extension() != ".json")
                    continue;
                ckpts.push_back({fs::last_write_time(ckpt), ckpt.path()});
            }
            std::sort(ckpts.begin(), ckpts.end(),
                      [](auto &a, auto &b) { return a.first > b.first; });

            // Keep maxPerAgent most recent, remove rest + old ones
            for (size_t i = 0; i < ckpts.size(); i++) {
                auto sctp = std::chrono::time_point_cast<
                    std::chrono::system_clock::duration>(
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
};

// ═══════════════════════════════════════════════════════════════════════════
// Relic Dispatcher — routes <action type="relic"> calls
// ═══════════════════════════════════════════════════════════════════════════
class RelicDispatcher {
public:
    static RelicDispatcher &instance() {
        static RelicDispatcher d;
        return d;
    }

    RelicResult dispatch(const std::string &relicName,
                         const std::string &endpoint,
                         const Json::Value &params) {
        // Built-in relics
        if (relicName == "session_journal")
            return dispatchJournal(endpoint, params);
        if (relicName == "state_checkpoint")
            return dispatchCheckpoint(endpoint, params);

        // Unknown
        return {false, "Unknown relic: " + relicName, {}};
    }

private:
    RelicResult dispatchJournal(const std::string &endpoint,
                                const Json::Value &params) {
        auto &j = SessionJournal::instance();
        std::string ns = params.get("namespace", "default").asString();

        if (endpoint == "record") {
            std::string et = params.get("event_type", "unknown").asString();
            std::string sid = params.get("session_id", "").asString();
            return j.record(ns, et, params["content"], sid);
        }
        if (endpoint == "query") {
            std::string sid = params.get("session_id", "").asString();
            std::string et = params.get("event_type", "").asString();
            int limit = params.get("limit", 100).asInt();
            return j.query(ns, sid, et, limit);
        }
        if (endpoint == "export") {
            return j.exportSession(ns, params.get("session_id", "").asString());
        }
        if (endpoint == "prune") {
            int age = params.get("max_age_seconds", 2592000).asInt();
            int maxRec = params.get("max_records", -1).asInt();
            return j.prune(ns, age, maxRec);
        }
        return {false, "Unknown endpoint: " + endpoint, {}};
    }

    RelicResult dispatchCheckpoint(const std::string &endpoint,
                                   const Json::Value &params) {
        auto &c = StateCheckpoint::instance();
        std::string ns = params.get("namespace", "default").asString();

        if (endpoint == "save") {
            return c.save(ns, params.get("agent_name", "agent").asString(),
                          params["state"], params.get("label", "").asString());
        }
        if (endpoint == "load" || endpoint == "latest") {
            return c.load(ns, params.get("agent_name", "agent").asString());
        }
        if (endpoint == "list") {
            return c.list(ns, params.get("agent_name", "").asString());
        }
        if (endpoint == "gc") {
            int age = params.get("max_age_seconds", 604800).asInt();
            int maxPer = params.get("max_per_agent", 5).asInt();
            return c.gc(ns, age, maxPer);
        }
        return {false, "Unknown endpoint: " + endpoint, {}};
    }
};

} // namespace relics
} // namespace mk3
} // namespace cortex
