#pragma once
// =============================================================================
// agent-lib-MK3 — Session Manager
// JSON session files under a STABLE data root (not CWD):
//   $CORTEX_SESSIONS_DIR
//   else $CORTEX_HOME/.cortex/sessions
//   else $XDG_DATA_HOME/cortex/sessions
//   else ~/.cortex/sessions
// CORTEX_HOME is a *parent* root (like $HOME): data lands under
// $CORTEX_HOME/.cortex — never bare $CORTEX_HOME/sessions, which polluted
// working trees when CORTEX_HOME pointed at a repo.
// Process-local write lock serializes load-merge-save across Agent / UI / atexit.
// =============================================================================

#include <json/json.h>

#include <cstdlib>
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

// Stable cortex data root — never CWD (CWD changes break sessions across repos).
inline std::filesystem::path cortexDataRoot() {
    // CORTEX_HOME is the parent of .cortex (same shape as ~/.cortex). Bare
    // $CORTEX_HOME/sessions + /state leaked into repos when CORTEX_HOME was
    // set to a project dir — always append the .cortex subdir.
    if (const char* ch = std::getenv("CORTEX_HOME"); ch && ch[0])
        return std::filesystem::path(ch) / ".cortex";
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && xdg[0])
        return std::filesystem::path(xdg) / "cortex";
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home && home[0] ? home : ".") / ".cortex";
}

inline std::string defaultSessionsDir() {
    if (const char* e = std::getenv("CORTEX_SESSIONS_DIR"); e && e[0])
        return std::string(e);
    return (cortexDataRoot() / "sessions").string();
}

inline std::string defaultStateDir() {
    if (const char* e = std::getenv("CORTEX_STATE_DIR"); e && e[0])
        return std::string(e);
    return (cortexDataRoot() / "state").string();
}

// Legacy project-local store (pre-fix). Read fallback only.
inline std::string cwdLegacySessionsDir() {
    std::error_code ec;
    auto p = std::filesystem::current_path(ec) / ".cortex" / "sessions";
    if (ec)
        return {};
    return p.string();
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
    std::vector<SessionInfo> list(bool includeCwdLegacy = true) const;

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
