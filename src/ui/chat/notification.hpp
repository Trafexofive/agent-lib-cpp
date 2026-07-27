// src/ui/chat/notification.hpp — transient operator-facing pop toasts.
//
// A Notification is a dismissible badge stack (chat) / top toast (hub). Distinct
// from chat-body rows (Log/Error persist). Same `id` collapses into one banner
// with ticking counters — a 3-attempt retry reads as one badge, not three.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "inkcell/style.hpp"
#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::chat {

// Default auto-dismiss for non-sticky toasts (operator can dismiss early with Esc).
inline constexpr int64_t kDefaultToastLifetimeMs = 4500;

struct Notification {
    std::string id;          // dedupe key; "" means "always fresh"
    std::string source;      // short tag ("empty-response", "curl-28", "rate-limit")
    std::string severity;    // "info" | "warn" | "error"
    std::string title;       // bold one-liner
    std::string detail;      // optional secondary line
    int attempt = 0;
    int maxAttempts = 0;
    int64_t lifetimeMs = kDefaultToastLifetimeMs;  // 0 = sticky until dismissed
    int64_t createdMs = 0;
};

class NotificationStack {
   public:
    void push(Notification n) {
        n.createdMs = nowMs();
        if (n.lifetimeMs < 0) n.lifetimeMs = kDefaultToastLifetimeMs;
        if (n.id.empty()) {
            items_.push_back(std::move(n));
        } else {
            // Upsert by id — keeps the retry counter ticking on top.
            for (auto& it : items_) {
                if (it.id == n.id) {
                    it = std::move(n);
                    pruneExpired();
                    return;
                }
            }
            items_.push_back(std::move(n));
        }
        // Cap stack depth so a retry storm cannot bury the chrome.
        while (items_.size() > 5) items_.pop_front();
        pruneExpired();
    }

    void dismissTop() {
        if (!items_.empty()) items_.pop_front();
    }

    void dismissId(const std::string& id) {
        for (auto it = items_.begin(); it != items_.end(); ++it) {
            if (it->id == id) {
                items_.erase(it);
                return;
            }
        }
    }

    void clearAll() { items_.clear(); }

    // Call each frame so lifetime-expired toasts leave without key input.
    void tick() { pruneExpired(); }

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
        int64_t n = nowMs();
        // Expire from front (oldest/top first) so auto-dismiss works for sticky-looking retries.
        while (!items_.empty()) {
            const auto& front = items_.front();
            if (front.lifetimeMs <= 0) break;
            if (n - front.createdMs > front.lifetimeMs) items_.pop_front();
            else break;
        }
        // Also drop expired deeper items.
        for (auto it = items_.begin(); it != items_.end();) {
            if (it->lifetimeMs > 0 && n - it->createdMs > it->lifetimeMs)
                it = items_.erase(it);
            else
                ++it;
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

inline inkcell::Color toastBgFor(const std::string& sev) {
    if (sev == "error") return inkcell::Color::rgb(48, 18, 24);
    if (sev == "warn") return inkcell::Color::rgb(42, 30, 14);
    return inkcell::Color::rgb(14, 24, 36);
}

inline inkcell::Color toastBorderFor(const std::string& sev) {
    if (sev == "error") return inkcell::Color::rgb(255, 107, 122);
    if (sev == "warn") return inkcell::Color::rgb(245, 180, 90);
    return inkcell::Color::rgb(90, 200, 220);
}

// Pop toast stack: top-right floating cards. Returns rows used.
// `popT` in [0,1] slides the stack in from above (0 = hidden, 1 = seated).
inline int drawNotificationToasts(inkcell::Surface& surface, inkcell::Rect area,
                                  const NotificationStack& stack, float popT = 1.f) {
    if (stack.empty() || area.w < 20 || area.h < 2) return 0;
    popT = std::max(0.f, std::min(1.f, popT));
    if (popT <= 0.01f) return 0;

    const int maxCards = std::min(3, static_cast<int>(stack.size()));
    const int cardW = std::min(area.w - 2, std::max(28, area.w * 2 / 5));
    const int cardX = area.right() - cardW - 1;
    int y = area.y;
    // Slide-in offset from top (cells).
    const int slide = static_cast<int>(std::lround((1.f - popT) * 3.f));
    y += slide;

    int drawn = 0;
    // Newest at top of visual stack = front of deque.
    for (std::size_t i = 0; i < stack.items().size() && drawn < maxCards; ++i) {
        const auto& n = stack.items()[i];
        const bool hasDetail = !n.detail.empty() || (n.maxAttempts > 0);
        const int cardH = hasDetail ? 3 : 2;
        if (y + cardH > area.bottom()) break;

        inkcell::Rect box{cardX, y, cardW, cardH};
        const auto bg = toastBgFor(n.severity);
        const auto border = toastBorderFor(n.severity);
        auto shell = inkcell::Style::normal().with_bg(bg).with_fg(border);
        surface.fill(box, " ", shell);
        surface.box(box, inkcell::BorderStyle::Rounded, shell);

        // Severity bar on left interior
        for (int row = box.y + 1; row < box.bottom() - 1; ++row)
            surface.put({box.x + 1, row}, "▌", shell.with_fg(border));

        std::string head;
        if (n.severity == "error") head = "ERR";
        else if (n.severity == "warn") head = "WRN";
        else head = "INF";
        if (!n.source.empty()) head += " · " + n.source;

        auto headSt = inkcell::Style::normal().with_bg(bg).with_fg(border);
        headSt.bold = true;
        surface.text({box.x + 3, box.y},
                     inkcell::text::truncate(head, cardW - 5), headSt);

        std::string title = n.title;
        if (n.maxAttempts > 0) {
            title += "  " + std::to_string(std::max(1, n.attempt)) + "/" +
                     std::to_string(n.maxAttempts);
        }
        auto titleSt = theme::bright().with_bg(bg);
        titleSt.bold = true;
        surface.text({box.x + 3, box.y + 1},
                     inkcell::text::truncate(title, cardW - 5), titleSt);

        if (hasDetail && cardH >= 3) {
            std::string det = n.detail.empty() ? std::string("esc dismiss") : n.detail;
            surface.text({box.x + 3, box.y + 2},
                         inkcell::text::truncate(det, cardW - 5),
                         theme::dim().with_bg(bg));
        }

        y += cardH;
        // Tiny gap between stacked cards
        if (drawn + 1 < maxCards) ++y;
        ++drawn;
    }
    return y - area.y - slide;
}

// Simple hub notice → toast (one-shot string, info severity).
inline void drawNoticeToast(inkcell::Surface& surface, inkcell::Rect area,
                            const std::string& notice) {
    if (notice.empty() || area.w < 16) return;
    NotificationStack tmp;
    Notification n;
    n.severity = "info";
    n.source = "hub";
    n.title = notice;
    n.lifetimeMs = 0;
    tmp.push(std::move(n));
    drawNotificationToasts(surface, area, tmp, 1.f);
}

}  // namespace cortex::mk3::ui::chat
