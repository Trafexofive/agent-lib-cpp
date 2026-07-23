// src/ui/chat/notification.hpp — transient operator-facing events.
//
// A Notification sits above the composer as a stack of small badges. Distinct
// from chat-body rows (Log/Error persist; Notifications are dismissible).
// Stability rule: the same notification `id` collapses into one banner with
// ticking counters — a 3-attempt retry reads as one badge, not three.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "inkcell/style.hpp"

namespace cortex::mk3::ui::chat {

struct Notification {
    std::string id;          // dedupe key; "" means "always fresh"
    std::string source;      // short tag ("empty-response", "curl-28", "rate-limit")
    std::string severity;    // "info" | "warn" | "error"
    std::string title;       // bold one-liner
    std::string detail;      // optional secondary line
    int attempt = 0;
    int maxAttempts = 0;
    int64_t lifetimeMs = 0;  // 0 = sticky until dismissed
    int64_t createdMs = 0;
};

class NotificationStack {
   public:
    void push(Notification n) {
        n.createdMs = nowMs();
        if (n.id.empty()) {
            items_.push_back(std::move(n));
        } else {
            // Upsert by id — keeps the retry counter ticking on top.
            for (auto& it : items_) {
                if (it.id == n.id) { it = std::move(n); return; }
            }
            items_.push_back(std::move(n));
        }
        pruneExpired();
    }

    void dismissTop() {
        if (!items_.empty()) items_.pop_front();
    }

    void dismissId(const std::string& id) {
        for (auto it = items_.begin(); it != items_.end(); ++it) {
            if (it->id == id) { items_.erase(it); return; }
        }
    }

    void clearAll() { items_.clear(); }

    const Notification* top() const {
        return items_.empty() ? nullptr : &items_.front();
    }

    bool empty() const { return items_.empty(); }
    std::size_t size() const { return items_.size(); }
    const std::deque<Notification>& items() const { return items_; }

    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

   private:
    std::deque<Notification> items_;

    void pruneExpired() {
        // Auto-prune everything except the top so the operator always sees
        // its last state; deeper ones time out cleanly.
        int64_t n = nowMs();
        while (items_.size() > 1) {
            const auto& back = items_.back();
            if (back.lifetimeMs <= 0) break;
            if (n - back.createdMs > back.lifetimeMs) items_.pop_back();
            else break;
        }
    }
};

inline inkcell::Style notificationAccentStyle(const std::string& sev, bool on) {
    auto s = inkcell::Style::normal();
    s.bold = on;
    if (sev == "error") {
        s.fg = inkcell::Color::rgb(255, 107, 122);
        s.dim = !on;
    } else if (sev == "warn") {
        s.fg = inkcell::Color::rgb(245, 180, 90);
        s.dim = !on;
    } else {
        s.fg = inkcell::Color::rgb(140, 200, 220);
        s.dim = !on;
    }
    return s;
}

}  // namespace cortex::mk3::ui::chat
