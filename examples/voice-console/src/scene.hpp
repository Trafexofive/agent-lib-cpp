#pragma once
// Voice console scene — a crafted inkcell TUI for the voice pipeline.
//
//   header    : ● live/offline  ·  app title  ·  engines (voice·whisper·llm)
//   pipeline  : wake → listen → stt → llm → tts  (✓ done · ⟳ current · · pending)
//   level     : live mic VU meter
//   body      : CONVERSATION (the product)  |  HARNESS (stream detail)
//   prompt    : manual harness run box
//   footer    : key hints + backend status
//
// Data: polls voice.py's --state-out JSON every ~200ms; spawns headless
// `cortex-mk3 run -p … --ephemeral --no-ansi` on demand (fork/exec, no shell).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "inkcell/inkcell.hpp"
#include "model.hpp"

namespace vc {

namespace {

constexpr const char* kStages[] = {"wake", "listen", "stt", "llm", "tts"};
constexpr int kStageCount = 5;

inline inkcell::Style role(const inkcell::Theme& t, inkcell::Role r) {
    return t.style(r);
}

// Greedy word wrap at `width`.
inline std::vector<std::string> wrap(const std::string& in, int width) {
    std::vector<std::string> out;
    if (width < 1)
        width = 1;
    std::string cur;
    const char* p = in.c_str();
    while (*p) {
        const char* sp = p;
        while (*sp && *sp != ' ')
            ++sp;
        std::string word(p, (size_t)(sp - p));
        if (!cur.empty() && (int)(cur.size() + 1 + word.size()) > width) {
            out.push_back(cur);
            cur.clear();
        }
        if (!cur.empty())
            cur += ' ';
        cur += word;
        p = *sp ? sp + 1 : sp;
        if ((int)cur.size() > width) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    if (out.empty())
        out.push_back("");
    return out;
}

}  // namespace

class VoiceConsoleScene : public inkcell::Scene {
   public:
    explicit VoiceConsoleScene(const AppConfig& cfg) : cfg_(cfg) {
        if (const char* e = std::getenv("CORTEX_BIN"))
            cfg_.bin = e;
        if (const char* e = std::getenv("VOICE_MANIFEST"))
            cfg_.manifest = e;
        if (const char* e = std::getenv("VOICE_STATE"))
            cfg_.state_path = e;
    }

    void on_enter() override { load_state(); }

    bool on_key(const inkcell::KeyEvent& e) override {
        // Global bindings from the design spec (§8): q / Ctrl+C quit, Esc
        // cancels. The prompt box is the only focus; it must not swallow
        // these when idle.
        if (e.code == inkcell::KeyCode::Character) {
            if (e.text == "q" && prompt_.empty())
                return false;  // idle → let keymap quit
            prompt_ += e.text;
            return true;
        }
        if (e.code == inkcell::KeyCode::Backspace) {
            if (!prompt_.empty()) {
                prompt_.pop_back();
                while (!prompt_.empty() &&
                       ((unsigned char)prompt_.back() & 0xC0) == 0x80)
                    prompt_.pop_back();
            }
            return true;
        }
        if (e.code == inkcell::KeyCode::Enter) {
            submit();
            return true;
        }
        if (e.code == inkcell::KeyCode::Escape) {
            if (!prompt_.empty()) {
                prompt_.clear();  // first Esc clears the prompt
                return true;
            }
            return false;  // idle → keymap Esc → app.quit
        }
        return false;  // everything else → keymap / engine (q, ctrl-c, ?)
    }

    void update(inkcell::Tick t, inkcell::Action) override {
        poll_ms_ += t.delta_ms;
        if (poll_ms_ >= 150) {
            poll_ms_ = 0;
            load_state();
        }
    }

    void draw(inkcell::Surface& s) const override {
        const int w = s.size().w;
        const int h = s.size().h;
        s.clear(role(theme_, inkcell::Role::Background));

        const bool offline =
            std::chrono::steady_clock::now() - state_seen_ > std::chrono::seconds(6);

        draw_header(s, w, offline);
        draw_pipeline(s, w, offline);
        draw_level(s, w, offline);

        // ── body: conversation (hero) | harness (detail) ──
        const int top = 4;
        const int bot = h - 4;
        if (bot - top >= 2) {
            const int left_w = std::max(34, w * 45 / 100);
            const int mid = 1 + left_w;
            draw_pane(s, {1, top, left_w, bot - top}, "CONVERSATION",
                      conv_lines(), /*left=*/true);
            draw_pane(s, {mid, top, w - mid - 1, bot - top}, "HARNESS",
                      run_.lines(), /*left=*/false);
        }

        // ── prompt ──
        const int py = h - 3;
        s.box({1, py, w - 2, 2}, inkcell::BorderStyle::Rounded,
              role(theme_, inkcell::Role::BorderFocused));
        s.text({3, py + 1}, ">", role(theme_, inkcell::Role::Accent));
        s.with_clip({3, py + 1, w - 6, 1}, [&] {
            s.text({5, py + 1}, prompt_ + (run_.running() ? "  ⟳" : ""),
                   role(theme_, inkcell::Role::Text));
            if (prompt_.empty() && !run_.running())
                s.text({5, py + 1}, "ask the harness…",
                       role(theme_, inkcell::Role::Ghost));
        });

        // ── footer ──
        s.fill({0, h - 1, w, 1}, ' ', role(theme_, inkcell::Role::Surface));
        const std::string hints = "Enter run   ·   Esc clear   ·   q quit";
        s.text({1, h - 1}, hints, role(theme_, inkcell::Role::TextMuted));
        const std::string status =
            std::string("backend ") + (offline ? "OFFLINE" : "● live");
        const inkcell::Style st = offline ? role(theme_, inkcell::Role::Error)
                                          : role(theme_, inkcell::Role::Success);
        s.text({w - (int)status.size() - 1, h - 1}, status, st);
    }

   private:
    void load_state() {
        VoiceState fresh;
        if (fresh.load(cfg_.state_path)) {
            state_ = fresh;
            state_seen_ = std::chrono::steady_clock::now();
        }
    }

    void submit() {
        std::string p = prompt_;
        while (!p.empty() && p.back() == ' ')
            p.pop_back();
        if (p.empty() || run_.running())
            return;
        run_.start(cfg_, p);
        prompt_.clear();
    }

    // ── header: live dot · title · engines ──
    void draw_header(inkcell::Surface& s, int w, bool offline) const {
        s.fill({0, 0, w, 1}, ' ', role(theme_, inkcell::Role::Surface));
        s.text({1, 0}, offline ? "○" : "●",
               offline ? role(theme_, inkcell::Role::Error)
                       : role(theme_, inkcell::Role::Success));
        s.text({3, 0}, "VOICE", role(theme_, inkcell::Role::Accent));
        const std::string engines = "af_heart · tiny.en · deepseek-v4-flash";
        s.text({w - (int)engines.size() - 1, 0}, engines,
               role(theme_, inkcell::Role::TextMuted));
    }

    // ── pipeline stepper: ✓ done · ⟳ current · · pending ──
    void draw_pipeline(inkcell::Surface& s, int w, bool offline) const {
        const int cur = stage_index();
        int x = 1;
        for (int i = 0; i < kStageCount; ++i) {
            const std::string& name = kStages[i];
            inkcell::Style st;
            std::string prefix;
            std::string label = name;
            if (offline) {
                st = role(theme_, inkcell::Role::TextMuted);
                prefix = "·";
            } else if (i < cur) {
                st = role(theme_, inkcell::Role::Success);
                prefix = "✓";
                auto it = state_.latency.find(name);
                if (it != state_.latency.end())
                    label += " " + std::to_string(it->second) + "ms";
            } else if (i == cur) {
                st = role(theme_, inkcell::Role::Accent);
                prefix = "⟳";
            } else {
                st = role(theme_, inkcell::Role::TextMuted);
                prefix = "·";
            }
            std::string seg = prefix + " " + label;
            s.text({x, 1}, seg, st);
            x += (int)seg.size() + 3;
            if (i + 1 < kStageCount && x < w - 2)
                s.text({x - 3, 1}, "→", role(theme_, inkcell::Role::Ghost));
        }
        if (cur < 0 && !offline) {
            std::string ready = "  ready — say \"" + (state_.wake_word.empty()
                                                          ? std::string("alexa")
                                                          : state_.wake_word) +
                                "\"";
            s.text({x, 1}, ready, role(theme_, inkcell::Role::TextMuted));
        }
    }

    // ── live mic level meter ──
    static std::string fmt_score(float v) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.2f", v);
        return buf;
    }

    void draw_level(inkcell::Surface& s, int w, bool offline) const {
        const int y = 2;
        const int bar_w = std::min(40, w - 12);
        s.text({1, y}, "mic", role(theme_, inkcell::Role::TextMuted));
        const float level = offline ? 0.0f : state_.level;
        const int filled = std::min(bar_w, (int)(level * bar_w + 0.5f));
        inkcell::Style st = role(theme_, inkcell::Role::Success);
        if (level > 0.85f)
            st = role(theme_, inkcell::Role::Error);
        else if (level > 0.6f)
            st = role(theme_, inkcell::Role::Warning);
        s.hline({5, y}, filled, "█", st);
        s.hline({5 + filled, y}, bar_w - filled, "░", role(theme_, inkcell::Role::Ghost));
        s.text({5 + bar_w + 1, y},
               std::to_string((int)(level * 100)) + "%",
               role(theme_, inkcell::Role::TextMuted));
        // wake state — which phrase, and whether we're listening
        if (!offline) {
            std::string wt = "wake: " +
                             (state_.wake_word.empty() ? std::string("alexa") : state_.wake_word);
            inkcell::Style ws = role(theme_, inkcell::Role::TextMuted);
            if (state_.stage == "wake") {
                wt += " · listening…";
                ws = role(theme_, inkcell::Role::Info);
            } else if (state_.stage != "idle") {
                wt += " · active";
                ws = role(theme_, inkcell::Role::Success);
            }
            const int wx = std::max(5 + bar_w + 10, w - (int)wt.size() - 2);
            s.text({wx, y}, wt, ws);
        }
    }

    // ── one titled pane, tail-clipped ──
    void draw_pane(inkcell::Surface& s, inkcell::Rect r, const std::string& title,
                   const std::vector<std::string>& lines, bool /*left*/) const {
        if (r.w < 10 || r.h < 3)
            return;
        s.box(r, inkcell::BorderStyle::Rounded, role(theme_, inkcell::Role::Border));
        s.text({r.x + 2, r.y}, title, role(theme_, inkcell::Role::Accent));
        const int inner_w = r.w - 3;
        const int rows = r.h - 3;
        s.with_clip({r.x + 1, r.y + 2, inner_w, rows}, [&] {
            std::vector<std::string> shown;
            for (const auto& ln : lines)
                for (auto& wl : wrap(ln, inner_w))
                    shown.push_back(wl);
            const size_t start =
                shown.size() > (size_t)rows ? shown.size() - rows : 0;
            int row = r.y + 2;
            for (size_t i = start; i < shown.size() && row < r.y + 2 + rows;
                 ++i, ++row)
                s.text({r.x + 2, row}, shown[i], block_style(shown[i]));
        });
    }

    std::vector<std::string> conv_lines() const {
        std::vector<std::string> out;
        if (!state_.transcript.empty())
            out.push_back("you:   " + state_.transcript);
        if (!state_.response.empty())
            out.push_back("agent: " + state_.response);
        if (out.empty())
            out.push_back("· say \"" + (state_.wake_word.empty() ? std::string("alexa")
                                                                 : state_.wake_word) +
                          "\" to talk");
        return out;
    }

    inkcell::Style block_style(const std::string& line) const {
        if (line.rfind("[response]", 0) == 0)
            return role(theme_, inkcell::Role::Success);
        if (line.rfind("[thought]", 0) == 0)
            return role(theme_, inkcell::Role::TextMuted);
        if (line.rfind("[action", 0) == 0)
            return role(theme_, inkcell::Role::Warning);
        if (line.rfind("[result", 0) == 0)
            return role(theme_, inkcell::Role::Info);
        return role(theme_, inkcell::Role::Text);
    }

    int stage_index() const {
        const std::string& st = state_.stage;
        if (st == "idle")
            return -1;
        for (int i = 0; i < kStageCount; ++i)
            if (st == kStages[i])
                return i;
        return -1;
    }

    AppConfig cfg_;
    inkcell::Theme theme_ = inkcell::Theme::deep_space();
    VoiceState state_;
    HarnessRun run_;
    std::string prompt_;
    int poll_ms_ = 0;
    std::chrono::steady_clock::time_point state_seen_ =
        std::chrono::steady_clock::now();
};

}  // namespace vc
