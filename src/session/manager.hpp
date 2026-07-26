#pragma once
// =============================================================================
// agent-lib-MK3 — Session Manager
// Ported from MK2. JSON session files in ~/.cortex/sessions/
// Process-local write lock serializes load-merge-save across Agent / UI / atexit.
// =============================================================================

#include <json/json.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "../core/types.hpp"

namespace cortex::mk3::session {

// Recursive: Agent::saveSession may load() then save() under the same call stack.
inline std::recursive_mutex& ioMutex() {
    static std::recursive_mutex m;
    return m;
}

class SessionManager {
   public:
    explicit SessionManager(const std::string& baseDir = "");

    Session load(const std::string& id) const;
    // pretty=false → compact JSON (faster long-session commits).
    void save(const Session& s, bool pretty = false) const;
    void remove(const std::string& id) const;
    bool exists(const std::string& id) const;

    struct SessionInfo {
        std::string id;
        std::string agentName;  // runtime agent identity
        std::string title;      // operator display name (metadata.name) or first-prompt snippet
        std::string model;
        std::string updated;
        size_t turnCount = 0;
        bool hasUiTimeline = false;
    };
    std::vector<SessionInfo> list() const;

    Session create(const std::string& id, const std::string& agent, const std::string& model,
                   const std::string& provider) const;
    void appendRecord(const std::string& id, const SessionRecord& r) const;
    void prune(const std::string& id, size_t maxRecords = 100) const;

    bool exportToFile(const std::string& id, const std::string& path) const;
    Session importFromFile(const std::string& path) const;

    static std::vector<SessionRecord> importLegacyHistory(const std::vector<std::string>& h);
    static std::vector<std::string> exportLegacyHistory(const std::vector<SessionRecord>& recs);
    static std::string iso8601();

    const std::string& baseDir() const {
        return baseDir_;
    }

   private:
    std::string baseDir_;
    std::string sessionPath(const std::string& id) const;
};

}  // namespace cortex::mk3::session
