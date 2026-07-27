#pragma once
// =============================================================================
// SessionController — single active session id + coalesced async UI timeline.
// Extract-as-we-go foundation (session + perf + modularity audits).
// =============================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "manager.hpp"

namespace cortex::mk3::session {

// ── Unified session id mint (session audit F6) ──────────────────────────────
// One scheme everywhere: CLI, hub create, lazy arm, hub fork.
// Shape: <cwd-slug>-<unix_ms>[-<salt4hex>]
// Optional title prefix is NOT in the id; use metadata.name for display titles.
inline std::string slugPart(std::string s) {
    for (char& c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) c = static_cast<char>(std::tolower(uc));
        else c = '-';
    }
    // Collapse runs of '-'.
    std::string out;
    out.reserve(s.size());
    bool prevDash = false;
    for (char c : s) {
        if (c == '-') {
            if (!prevDash) out.push_back(c);
            prevDash = true;
        } else {
            out.push_back(c);
            prevDash = false;
        }
    }
    while (!out.empty() && out.front() == '-') out.erase(out.begin());
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.size() > 32) out.resize(32);
    return out.empty() ? "project" : out;
}

// Mint a process-local unique session id. Salt differentiates multi-mint
// within the same millisecond (hub fork + create + lazy arm).
inline std::string mintSessionId(const std::string& projectHint = {}) {
    static std::atomic<uint32_t> salt{0};
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    std::string project = projectHint;
    if (project.empty()) {
        try {
            project = slugPart(std::filesystem::current_path().filename().string());
        } catch (...) {
            project = "project";
        }
    } else {
        project = slugPart(project);
    }
    uint32_t s = salt.fetch_add(1, std::memory_order_relaxed) & 0xFFFFu;
    char buf[96];
    if (s == 0) {
        // First mint in this ms window can omit salt for shorter ids (CLI path).
        std::snprintf(buf, sizeof(buf), "%s-%lld", project.c_str(),
                      static_cast<long long>(ms));
    } else {
        std::snprintf(buf, sizeof(buf), "%s-%lld-%04x", project.c_str(),
                      static_cast<long long>(ms), s);
    }
    return std::string(buf);
}

// Process-wide active session identity for the experimental inkcell path.
// Kill dual-id flush: cfg seeds this once; lazy arm updates it; atexit reads it.
struct SessionRef {
    mutable std::mutex mtx;
    std::string id;
    bool ephemeral = false;  // true when --no-session (skip all disk). NOT CLI --ephemeral (exit-on-done).

    void clear() {
        std::lock_guard<std::mutex> g(mtx);
        id.clear();
        ephemeral = false;
    }
    void set(std::string newId, bool eph = false) {
        std::lock_guard<std::mutex> g(mtx);
        id = std::move(newId);
        ephemeral = eph;
    }
    std::string get() const {
        std::lock_guard<std::mutex> g(mtx);
        return id;
    }
    bool isEphemeral() const {
        std::lock_guard<std::mutex> g(mtx);
        return ephemeral;
    }
    bool empty() const {
        std::lock_guard<std::mutex> g(mtx);
        return id.empty();
    }
};

inline SessionRef& activeSession() {
    static SessionRef r;
    return r;
}

// Snapshot for background UI-timeline merge (no Agent* on the worker).
struct UiTimelineCommit {
    std::string sessionId;
    std::string baseDir;
    std::string uiTimelineJson;
    std::string agentName;
    std::string model;
    std::string provider;
    uint64_t generation = 0;
};

// Coalescing async writer: rapid TurnDone + route-away + atexit collapse to
// the latest generation. Process-local singleton.
class AsyncUiTimelineWriter {
   public:
    static AsyncUiTimelineWriter& instance() {
        static AsyncUiTimelineWriter w;
        return w;
    }

    void enqueue(UiTimelineCommit c) {
        if (c.sessionId.empty() || c.uiTimelineJson.empty() || c.uiTimelineJson == "[]")
            return;
        {
            std::lock_guard<std::mutex> g(mtx_);
            ensureWorker_();
            pending_ = std::move(c);
            hasPending_ = true;
        }
        cv_.notify_one();
    }

    // Block until the latest enqueued generation is written (or idle).
    void flush() {
        uint64_t want = 0;
        {
            std::lock_guard<std::mutex> g(mtx_);
            if (hasPending_) want = pending_.generation;
            else want = writtenGen_;
        }
        if (want == 0 && !hasPending_) return;
        for (;;) {
            std::unique_lock<std::mutex> g(mtx_);
            if (!hasPending_ && writtenGen_ >= want) return;
            // Wake worker if something is waiting.
            cv_.notify_one();
            doneCv_.wait_for(g, std::chrono::milliseconds(50));
        }
    }

    ~AsyncUiTimelineWriter() {
        {
            std::lock_guard<std::mutex> g(mtx_);
            stop_ = true;
        }
        cv_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

   private:
    AsyncUiTimelineWriter() = default;
    AsyncUiTimelineWriter(const AsyncUiTimelineWriter&) = delete;
    AsyncUiTimelineWriter& operator=(const AsyncUiTimelineWriter&) = delete;

    void ensureWorker_() {
        if (workerStarted_) return;
        workerStarted_ = true;
        worker_ = std::thread([this] { loop_(); });
    }

    void loop_() {
        for (;;) {
            UiTimelineCommit job;
            {
                std::unique_lock<std::mutex> g(mtx_);
                cv_.wait(g, [&] { return stop_ || hasPending_; });
                if (stop_ && !hasPending_) return;
                if (!hasPending_) continue;
                job = std::move(pending_);
                hasPending_ = false;
            }
            write_(job);
            {
                std::lock_guard<std::mutex> g(mtx_);
                writtenGen_ = std::max(writtenGen_, job.generation);
            }
            doneCv_.notify_all();
        }
    }

    static void write_(const UiTimelineCommit& job) {
        try {
            SessionManager sm(job.baseDir);
            Session s;
            if (sm.exists(job.sessionId)) {
                s = sm.load(job.sessionId);
            } else {
                s.id = job.sessionId;
                s.agentName = job.agentName;
                s.model = job.model;
                s.provider = job.provider;
                s.created = SessionManager::iso8601();
            }
            s.uiTimelineJson = job.uiTimelineJson;
            s.updated = SessionManager::iso8601();
            if (s.agentName.empty()) s.agentName = job.agentName;
            if (s.model.empty()) s.model = job.model;
            if (s.provider.empty()) s.provider = job.provider;
            sm.save(s, /*pretty=*/false);
        } catch (...) {
            // Fail-soft: never throw across worker → UI.
        }
    }

    std::mutex mtx_;
    std::condition_variable cv_;
    std::condition_variable doneCv_;
    std::thread worker_;
    bool workerStarted_ = false;
    bool stop_ = false;
    bool hasPending_ = false;
    UiTimelineCommit pending_;
    uint64_t writtenGen_ = 0;
};

// Deep-copy fork including ui_timeline (session audit F5 / S0.4).
inline Session forkSession(SessionManager& sm, const std::string& fromId, const std::string& newId,
                           const std::string& sessionName = "") {
    Session src = sm.load(fromId);
    Session fork = sm.create(newId, src.agentName, src.model, src.provider);
    fork.records = src.records;
    fork.contextFeeds = src.contextFeeds;
    fork.metadata = src.metadata;
    fork.renderedHistory = src.renderedHistory;
    fork.uiTimelineJson = src.uiTimelineJson;
    if (!sessionName.empty()) fork.metadata["name"] = sessionName;
    fork.metadata["forked_from"] = fromId;
    fork.updated = SessionManager::iso8601();
    sm.save(fork, /*pretty=*/false);
    return fork;
}

// Set operator display title (metadata.name). Empty title clears the name.
inline bool setSessionTitle(SessionManager& sm, const std::string& id, const std::string& title) {
    if (id.empty() || !sm.exists(id)) return false;
    Session s = sm.load(id);
    if (title.empty())
        s.metadata.erase("name");
    else
        s.metadata["name"] = title;
    s.updated = SessionManager::iso8601();
    sm.save(s, /*pretty=*/false);
    return true;
}

// Merge ui timeline onto disk synchronously (tests / fallback).
inline void commitUiTimelineSync(SessionManager& sm, const std::string& id,
                                 const std::string& uiTimelineJson, const std::string& agentName,
                                 const std::string& model, const std::string& provider) {
    if (id.empty() || uiTimelineJson.empty() || uiTimelineJson == "[]") return;
    Session s;
    if (sm.exists(id)) {
        s = sm.load(id);
    } else {
        s.id = id;
        s.agentName = agentName;
        s.model = model;
        s.provider = provider;
        s.created = SessionManager::iso8601();
    }
    s.uiTimelineJson = uiTimelineJson;
    s.updated = SessionManager::iso8601();
    if (s.agentName.empty()) s.agentName = agentName;
    if (s.model.empty()) s.model = model;
    if (s.provider.empty()) s.provider = provider;
    sm.save(s, /*pretty=*/false);
}

}  // namespace cortex::mk3::session
