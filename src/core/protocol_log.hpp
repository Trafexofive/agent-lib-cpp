#pragma once
// Thread-safe protocol event log.
// Parser / runLoop write. TUI reads snapshots. No naked vector across threads.

#include <mutex>
#include <utility>
#include <vector>

#include "../protocol/events.hpp"

namespace cortex::mk3 {

struct ProtocolLog {
    void clear() {
        std::lock_guard<std::mutex> g(mu_);
        events_.clear();
    }

    void push(ProtocolEvent e) {
        std::lock_guard<std::mutex> g(mu_);
        events_.push_back(std::move(e));
    }

    size_t size() const {
        std::lock_guard<std::mutex> g(mu_);
        return events_.size();
    }

    std::vector<ProtocolEvent> snapshot() const {
        std::lock_guard<std::mutex> g(mu_);
        return events_;
    }

    template <typename F>
    void mutate(F&& f) {
        std::lock_guard<std::mutex> g(mu_);
        f(events_);
    }

    template <typename F>
    void read(F&& f) const {
        std::lock_guard<std::mutex> g(mu_);
        f(events_);
    }

private:
    mutable std::mutex mu_;
    std::vector<ProtocolEvent> events_;
};

}  // namespace cortex::mk3
