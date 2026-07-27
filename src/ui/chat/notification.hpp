// src/ui/chat/notification.hpp — transient operator-facing feedback.
//
// Chat: one elevated line above the composer (same language as the footer).
// Hub: notice already lives in the app-bar subtitle — no second surface.
// Same `id` collapses; auto-dismiss; Esc dismisses top.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>

#include "inkcell/style.hpp"
#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::chat {

inline constexpr int64_t kDefaultToastLifetimeMs = 4000;

struct Notification {
    std::string id;        // dedupe key; "" = always fresh
    std::string source;    // short tag
    std::string severity;  // "info" | "warn" | "error"
    std::string title;     // one-liner
    std::string detail;    // unused in paint (kept for model/debug)
    int attempt = 0;
    int maxAttempts = 0;
    int64_t lifetimeMs = kDefaultToastLifetimeMs;  // 0 = sticky until Esc
    int64_t createdMs = 0;
};

class NotificationStack {
   public:
    void push(Notification n) {
        n.createdMs = nowMs();
        if (n.lifetimeMs < 0) n.lifetimeMs = kDefaultToastLifetimeMs;
        if (!n.id.empty()) {
            for (auto& it : items_) {
                if (it.id == n.id) {
                    it = std::move(n);
                    pruneExpired();
                    return;
                }
            }
        }
        items_.push_back(std::move(n));
        while (items_.size() > 4) items_.pop_front();
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
        const int64_t n = nowMs();
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

// Rows the chat layout should reserve above the status/composer block.
inline int notificationReserveRows(const NotificationStack& stack) {
    return stack.empty() ? 0 : 1;
}

// One-line elevated strip — same language as the chat footer, not a card.
// Place immediately above the status line.
inline void drawNotificationStrip(inkcell::Surface& surface, inkcell::Rect row,
                                  const NotificationStack& stack) {
    if (stack.empty() || row.w < 8 || row.h < 1) return;
    const Notification* n = stack.top();
    if (!n) return;

    auto bg = theme::footer_bg();
    surface.fill(row, " ", bg);

    inkcell::Style accent = theme::footer_accent_idle();
    inkcell::Style textSt = theme::footer_bright();
    if (n->severity == "error") {
        accent = theme::red().with_bg(bg.bg);
        accent.bold = true;
        textSt = theme::red().with_bg(bg.bg);
    } else if (n->severity == "warn") {
        accent = theme::footer_warn();
        accent.bold = true;
        textSt = theme::footer_warn();
        textSt.bold = true;
    }
    surface.text({row.x, row.y}, "▌", accent);

    // source · title  a/m  +N
    std::string body;
    body.reserve(96);
    if (!n->source.empty()) {
        body += n->source;
        body += "  ·  ";
    }
    body += n->title;
    if (n->maxAttempts > 0) {
        body += "  ";
        body += std::to_string(std::max(1, n->attempt));
        body += "/";
        body += std::to_string(n->maxAttempts);
    }
    if (stack.size() > 1) {
        body += "  +";
        body += std::to_string(static_cast<int>(stack.size()) - 1);
    }

    surface.text({row.x + 2, row.y},
                 inkcell::text::truncate(body, std::max(1, row.w - 3)), textSt);
}

// Back-compat name used by agent_scene (single strip, ignore popT / multi-card).
inline int drawNotificationToasts(inkcell::Surface& surface, inkcell::Rect area,
                                  const NotificationStack& stack, float /*popT*/ = 1.f) {
    if (stack.empty() || area.h < 1) return 0;
    // Prefer bottom of the reserved area (just above composer).
    inkcell::Rect row{area.x, area.bottom() - 1, area.w, 1};
    drawNotificationStrip(surface, row, stack);
    return 1;
}

// Hub must NOT draw a second toast — app bar already shows dash.notice.
// Kept as no-op so call sites compile until removed.
inline void drawNoticeToast(inkcell::Surface&, inkcell::Rect, const std::string&) {}

}  // namespace cortex::mk3::ui::chat
