#pragma once
// =============================================================================
// Cortex MK3 — AgentBridge
//
// Thread-safe event conduit between the blocking/streaming Agent runtime and the
// inkcell UI thread. Phase 1 will wire Agent::prompt callbacks into publish().
// =============================================================================

#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ui_event.hpp"

namespace cortex::mk3::ui {

class AgentBridge {
   public:
    AgentBridge() {
        wakeFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wakeFd_ < 0) {
            throw std::runtime_error(std::string("eventfd failed: ") + std::strerror(errno));
        }
    }

    ~AgentBridge() {
        if (wakeFd_ >= 0)
            ::close(wakeFd_);
    }

    AgentBridge(const AgentBridge&) = delete;
    AgentBridge& operator=(const AgentBridge&) = delete;

    AgentBridge(AgentBridge&& other) noexcept {
        std::lock_guard<std::mutex> lock(other.mu_);
        wakeFd_ = other.wakeFd_;
        other.wakeFd_ = -1;
        queue_ = std::move(other.queue_);
        snapshot_ = std::move(other.snapshot_);
    }

    AgentBridge& operator=(AgentBridge&& other) noexcept {
        if (this == &other)
            return *this;
        if (wakeFd_ >= 0)
            ::close(wakeFd_);
        std::scoped_lock lock(mu_, other.mu_);
        wakeFd_ = other.wakeFd_;
        other.wakeFd_ = -1;
        queue_ = std::move(other.queue_);
        snapshot_ = std::move(other.snapshot_);
        return *this;
    }

    int wakeFd() const { return wakeFd_; }

    void publish(UiEvent event) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            queue_.push_back(std::move(event));
        }
        wake();
    }

    void publishMany(std::vector<UiEvent> events) {
        if (events.empty())
            return;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& event : events)
                queue_.push_back(std::move(event));
        }
        wake();
    }

    std::vector<UiEvent> drain() {
        std::vector<UiEvent> out;
        {
            std::lock_guard<std::mutex> lock(mu_);
            out.reserve(queue_.size());
            while (!queue_.empty()) {
                out.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
            if (!out.empty()) {
                snapshot_.events.insert(snapshot_.events.end(), out.begin(), out.end());
                for (const auto& e : out) {
                    if (e.kind == UiEventKind::Status)
                        snapshot_.status = e.text;
                    else if (e.kind == UiEventKind::TurnDone)
                        snapshot_.running = false;
                    else if (e.kind == UiEventKind::Token || e.kind == UiEventKind::Protocol)
                        snapshot_.running = true;
                }
            }
        }
        drainWakeFd();
        return out;
    }

    UiSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        return snapshot_;
    }

    Json::Value requestAsk(const Json::Value& params) {
        {
            std::lock_guard<std::mutex> lock(askMu_);
            askPending_ = true;
            askComplete_ = false;
            askCancelled_ = false;
            askResult_ = Json::objectValue;
        }
        UiEvent event;
        event.kind = UiEventKind::AskDialog;
        event.json = params;
        publish(std::move(event));

        std::unique_lock<std::mutex> lock(askMu_);
        askCv_.wait(lock, [&] { return askComplete_ || askCancelled_; });
        Json::Value out;
        out["success"] = !askCancelled_;
        out["cancelled"] = askCancelled_;
        out["results"] = askResult_;
        askPending_ = false;
        return out;
    }

    void completeAsk(const Json::Value& results) {
        {
            std::lock_guard<std::mutex> lock(askMu_);
            if (!askPending_) return;
            askResult_ = results;
            askComplete_ = true;
        }
        askCv_.notify_all();
    }

    void cancelAsk() {
        {
            std::lock_guard<std::mutex> lock(askMu_);
            if (!askPending_) return;
            askCancelled_ = true;
        }
        askCv_.notify_all();
    }

    bool askPending() const {
        std::lock_guard<std::mutex> lock(askMu_);
        return askPending_ && !askComplete_ && !askCancelled_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        queue_.clear();
        snapshot_ = {};
        drainWakeFd();
    }

   private:
    int wakeFd_ = -1;
    mutable std::mutex mu_;
    std::deque<UiEvent> queue_;
    UiSnapshot snapshot_;
    mutable std::mutex askMu_;
    std::condition_variable askCv_;
    bool askPending_ = false;
    bool askComplete_ = false;
    bool askCancelled_ = false;
    Json::Value askResult_ = Json::objectValue;

    void wake() const {
        if (wakeFd_ < 0)
            return;
        uint64_t one = 1;
        ssize_t n = ::write(wakeFd_, &one, sizeof(one));
        (void)n;  // EAGAIN is fine; fd already readable.
    }

    void drainWakeFd() const {
        if (wakeFd_ < 0)
            return;
        uint64_t value = 0;
        while (::read(wakeFd_, &value, sizeof(value)) == sizeof(value)) {
        }
    }
};

}  // namespace cortex::mk3::ui
