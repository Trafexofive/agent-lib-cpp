#pragma once
// Animated command palette — Ctrl-P / leader-leader (space×2).
// Faux transparency via scrim dither + elevated modal.
// Open: scale+fade from center; close: reverse; rows stagger in.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "inkcell/surface.hpp"
#include "inkcell/text.hpp"
#include "src/ui/chat/chat_command_catalog.hpp"
#include "src/ui/gfx/blit.hpp"
#include "src/ui/gfx/field_raster.hpp"
#include "src/ui/gfx/shaders_dedsec.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui::components {

struct CmdItem {
    std::string id;     // action key
    std::string label;
    std::string hint;   // right side / subtitle
    std::string group;  // NAV · CHAT · AGENT · SYSTEM
    std::string keys;   // shortcut hint
};

inline float cpClamp01(float t) { return t < 0.f ? 0.f : (t > 1.f ? 1.f : t); }

inline float cpSmoother(float t) {
    t = cpClamp01(t);
    return t * t * t * (t * (t * 6.f - 15.f) + 10.f);
}

inline float cpEaseOutBack(float t) {
    t = cpClamp01(t);
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.f;
    float u = t - 1.f;
    return 1.f + c3 * u * u * u + c1 * u * u;
}

inline float cpEaseInCubic(float t) {
    t = cpClamp01(t);
    return t * t * t;
}

struct CmdPalette {
    bool open = false;
    bool closing = false;
    int64_t animStartMs = 0;
    static constexpr int openMs = 280;
    static constexpr int closeMs = 200;
    static constexpr int leaderGapMs = 420;

    std::string query;
    int index = 0;
    std::vector<CmdItem> all;
    std::vector<CmdItem> filtered;

    // Leader (space×2) detector
    int64_t lastSpaceMs = 0;

    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    float animT() const {
        if (animStartMs <= 0) return open && !closing ? 1.f : 0.f;
        int dur = closing ? closeMs : openMs;
        float t = static_cast<float>(nowMs() - animStartMs) / static_cast<float>(dur);
        return cpClamp01(t);
    }

    // 0 = fully hidden, 1 = fully shown (accounts for close reverse)
    float visibility() const {
        float t = animT();
        if (!open && !closing) return 0.f;
        if (closing) return 1.f - cpEaseInCubic(t);
        return cpSmoother(t);
    }

    bool active() const { return open || closing; }
    bool animating() const {
        if (!active()) return false;
        return animT() < 1.f;
    }

    void finishCloseIfNeeded() {
        if (closing && animT() >= 1.f) {
            closing = false;
            open = false;
            query.clear();
            index = 0;
            filtered.clear();
        }
    }

    void show(std::vector<CmdItem> items) {
        all = std::move(items);
        query.clear();
        index = 0;
        refilter();
        open = true;
        closing = false;
        animStartMs = nowMs();
    }

    void requestClose() {
        if (!open || closing) return;
        closing = true;
        animStartMs = nowMs();
    }

    void toggle(std::vector<CmdItem> items) {
        if (open && !closing) requestClose();
        else show(std::move(items));
    }

    // Returns true if space×2 leader fired (caller should open palette).
    bool noteSpace() {
        int64_t n = nowMs();
        if (lastSpaceMs > 0 && (n - lastSpaceMs) <= leaderGapMs) {
            lastSpaceMs = 0;
            return true;
        }
        lastSpaceMs = n;
        return false;
    }

    void clearLeader() { lastSpaceMs = 0; }

    void refilter() {
        filtered.clear();
        std::string q = query;
        for (char& c : q) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const auto& it : all) {
            if (q.empty()) {
                filtered.push_back(it);
                continue;
            }
            std::string hay = it.label + " " + it.hint + " " + it.id + " " + it.group;
            for (char& c : hay) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            // fuzzy subsequence OR substring
            if (hay.find(q) != std::string::npos) {
                filtered.push_back(it);
                continue;
            }
            size_t qi = 0;
            for (char c : hay) {
                if (qi < q.size() && c == q[qi]) ++qi;
            }
            if (qi == q.size()) filtered.push_back(it);
        }
        if (index >= static_cast<int>(filtered.size()))
            index = std::max(0, static_cast<int>(filtered.size()) - 1);
        if (filtered.empty()) index = 0;
    }

    void move(int delta) {
        if (filtered.empty()) return;
        int n = static_cast<int>(filtered.size());
        index = (index + delta) % n;
        if (index < 0) index += n;
    }

    const CmdItem* selected() const {
        if (index < 0 || index >= static_cast<int>(filtered.size())) return nullptr;
        return &filtered[static_cast<size_t>(index)];
    }

    void onQueryChar(char ch) {
        if (ch >= 32) {
            query.push_back(ch);
            refilter();
            index = 0;
        }
    }

    void onBackspace() {
        if (!query.empty()) {
            query.pop_back();
            refilter();
            index = 0;
        }
    }
};

// Build hub-scoped commands.
inline std::vector<CmdItem> hubCommands() {
    return {
        {"nav.home", "Home", "operator overview", "NAV", "g"},
        {"nav.sessions", "Sessions", "resume · create · delete", "NAV", "s"},
        {"nav.manifests", "Manifests", "registry · launch agents", "NAV", "a"},
        {"nav.settings", "Settings", "theme · shaders · keys", "NAV", "?"},
        {"act.shader", "Next shader", "cycle field background", "ACTION", "S"},
        {"act.shader_off", "Shader off", "solid theme background", "ACTION", "B"},
        {"nav.chat", "Open chat", "switch to agent scene", "NAV", "c"},
        {"act.refresh", "Refresh hub", "reload sessions + manifests", "ACTION", "R"},
        {"act.theme", "Toggle theme", "graphite ↔ neon", "ACTION", "T"},
        {"act.launch", "Launch selected agent", "hot-swap from manifests selection", "ACTION", "↵"},
        {"sys.quit", "Quit", "exit cortex", "SYSTEM", "q"},
    };
}

// Build chat-scoped commands (slash + navigation).
inline std::vector<CmdItem> chatCommands() {
    std::vector<CmdItem> out = {
        {"nav.main", "Dashboard", "back to hub", "NAV", "m"},
        {"chat.thoughts", "Toggle thoughts", "show/hide model thoughts", "CHAT", "^T"},
        {"chat.truncate", "Toggle truncate", "cap long bodies", "CHAT", "^O"},
        {"chat.raw", "Toggle raw", "protocol dump view", "CHAT", "^R"},
        {"chat.clear", "Clear transcript", "wipe current view", "CHAT", "/clear"},
        {"chat.stop", "Stop agent loop", "kill switch", "CHAT", "^X"},
        {"act.theme", "Toggle theme", "graphite ↔ neon", "ACTION", "T"},
        {"sys.quit", "Quit", "exit cortex", "SYSTEM", "q"},
    };
    // Slash builtins as palette entries
    static const char* slashes[] = {
        "/help", "/commands", "/manifests", "/sessions", "/prompts",
        "/dump-prompt", "/cp-all", "/stop", "/quit",
    };
    for (const char* s : slashes) {
        CmdItem it;
        it.id = std::string("slash:") + s;
        it.label = s;
        it.hint = "slash command";
        it.group = "SLASH";
        it.keys = s;
        out.push_back(it);
    }
    for (const auto& d : chat::discoverDynamicChatCommands()) {
        CmdItem it;
        it.id = "slash:" + d.name;
        it.label = d.name;
        it.hint = d.description.empty() ? d.kind : d.description;
        it.group = "PROMPT";
        it.keys = d.name;
        out.push_back(it);
    }
    return out;
}

// Palette scrim = field plasma (half-block) + light darken. Not GLSL — real samples.
inline void drawScrim(inkcell::Surface& s, inkcell::Rect page, float vis) {
    if (vis <= 0.01f) return;
    gfx::drawFieldBg(s, page, gfx::themeVariantIndex(), gfx::nowSeconds());
    // DedSec grit only when field is on (otherwise solid theme already drawn)
    if (gfx::fieldEnabled()) {
        const auto& grit = gfx::bakeDedSecScrim(page.w, page.h, gfx::themeVariantIndex(),
                                               gfx::nowSeconds());
        gfx::blit(s, grit, page.x, page.y, gfx::BlitMode::Transparent, page);
    }
    if (vis < 0.9f) {
        auto veil = inkcell::Style::normal()
                        .with_bg(theme::color(inkcell::Color::rgb(0, 0, 0), inkcell::Color::rgb(0, 0, 0)))
                        .with_fg(theme::color(inkcell::Color::rgb(0, 0, 0), inkcell::Color::rgb(0, 0, 0)));
        int skip = vis > 0.55f ? 2 : 1;
        for (int y = page.y; y < page.bottom(); ++y)
            for (int x = page.x; x < page.right(); ++x)
                if (((x + y) % (skip + 1)) == 0) s.fill({x, y, 1, 1}, " ", veil);
    }
}

// Draw palette; returns true if still animating (caller may force tick redraw).
inline bool drawCmdPalette(inkcell::Surface& s, inkcell::Rect page, CmdPalette& pal) {
    pal.finishCloseIfNeeded();
    if (!pal.active()) return false;

    float vis = pal.visibility();
    if (vis <= 0.01f) return pal.animating();

    drawScrim(s, page, vis);

    // Modal geometry — springs open with slight overshoot on open
    float scaleT = pal.closing ? cpSmoother(vis) : cpEaseOutBack(std::min(1.f, vis * 1.05f));
    scaleT = cpClamp01(scaleT);

    int targetW = std::min(page.w - 6, std::max(48, page.w * 5 / 8));
    int targetH = std::min(page.h - 6, std::max(12, page.h * 2 / 3));
    int boxW = std::max(24, static_cast<int>(std::lround(targetW * (0.72f + 0.28f * scaleT))));
    int boxH = std::max(8, static_cast<int>(std::lround(targetH * (0.55f + 0.45f * scaleT))));

    // Rise from slightly below as it fades in
    int baseY = page.y + (page.h - boxH) / 3;
    int rise = static_cast<int>(std::lround((1.f - vis) * 4.f));
    int boxX = page.x + (page.w - boxW) / 2;
    int boxY = baseY + rise;
    inkcell::Rect box{boxX, boxY, boxW, boxH};

    // Shadow
    auto sh = inkcell::Style::normal()
                  .with_bg(theme::color(inkcell::Color::rgb(0, 0, 0), inkcell::Color::rgb(0, 0, 0)))
                  .with_fg(theme::color(inkcell::Color::rgb(0, 0, 0), inkcell::Color::rgb(0, 0, 0)));
    if (vis > 0.3f)
        s.fill({box.x + 2, box.y + 1, box.w, box.h}, " ", sh);

    // Frosted panel
    auto panel = theme::panel_2();
    panel.bg = theme::color(inkcell::Color::rgb(28, 28, 34), inkcell::Color::rgb(14, 20, 34));
    s.fill(box, " ", panel);

    auto borderFg = theme::color(inkcell::Color::rgb(90, 140, 155), inkcell::Color::rgb(70, 180, 220));
    if (vis < 0.7f)
        borderFg = theme::color(inkcell::Color::rgb(60, 70, 80), inkcell::Color::rgb(40, 70, 100));
    s.box(box, inkcell::BorderStyle::Rounded, panel.with_fg(borderFg));

    // Top accent gradient line
    auto ac = theme::cyan().with_bg(panel.bg);
    for (int x = box.x + 2; x < box.right() - 2; ++x) {
        float u = static_cast<float>(x - box.x) / static_cast<float>(box.w);
        ac.fg = theme::color(
            inkcell::Color::rgb(static_cast<uint8_t>(100 + 40 * u),
                                static_cast<uint8_t>(160 - 20 * u),
                                static_cast<uint8_t>(180 - 30 * u)),
            inkcell::Color::rgb(static_cast<uint8_t>(90 + 80 * u),
                                static_cast<uint8_t>(220 - 40 * u),
                                255));
        s.text({x, box.y + 1}, "─", ac);
    }

    // Title + query
    int y = box.y + 2;
    auto title = theme::bright().with_bg(panel.bg);
    auto muted = theme::italic_dim().with_bg(panel.bg);
    s.text({box.x + 2, y}, "command palette", title);
    s.text({box.x + 20, y}, "ctrl-p · space space", muted);
    ++y;

    // Query field
    std::string qline =
        "> " + pal.query + (static_cast<int>(CmdPalette::nowMs() / 530) % 2 == 0 ? "█" : " ");
    auto qst = theme::cyan().with_bg(panel.bg);
    s.fill({box.x + 1, y, box.w - 2, 1}, " ", theme::panel_3());
    s.text({box.x + 2, y}, inkcell::text::truncate(qline, box.w - 4), qst);
    ++y;
    // hairline under query
    s.hline({box.x + 2, y}, box.w - 4, "─", theme::dim().with_bg(panel.bg));
    ++y;

    int listTop = y;
    int listBot = box.bottom() - 2;
    int visible = std::max(1, listBot - listTop);
    int start = 0;
    if (pal.index >= visible) start = pal.index - visible + 1;
    if (start < 0) start = 0;

    // Stagger: rows appear with delay based on open vis
    float rowGate = pal.closing ? 1.f : cpSmoother(std::min(1.f, (vis - 0.15f) / 0.85f));

    std::string lastGroup;
    int drawn = 0;
    for (int i = start; i < static_cast<int>(pal.filtered.size()) && drawn < visible; ++i) {
        const auto& it = pal.filtered[static_cast<size_t>(i)];
        // Group headers consume a row
        if (it.group != lastGroup) {
            if (drawn >= visible) break;
            lastGroup = it.group;
            float gAppear = rowGate;
            if (gAppear > 0.2f) {
                auto gs = theme::violet_soft().with_bg(panel.bg);
                gs.italic = true;
                if (gAppear < 0.6f) gs.dim = true;
                s.text({box.x + 2, listTop + drawn},
                       inkcell::text::truncate(it.group, box.w - 4), gs);
            }
            ++drawn;
            if (drawn >= visible) break;
        }

        // Per-row stagger
        float local = cpClamp01(rowGate * 1.2f - 0.04f * static_cast<float>(drawn));
        if (local < 0.05f) {
            ++drawn;
            continue;
        }

        bool sel = (i == pal.index);
        int rowY = listTop + drawn;
        int rowX = box.x + 1 + static_cast<int>(std::lround((1.f - local) * 3.f));  // slide in from left
        int rowW = box.w - 2 - (rowX - (box.x + 1));

        auto rowBg = sel ? theme::panel_3() : panel;
        s.fill({box.x + 1, rowY, box.w - 2, 1}, " ", rowBg);
        if (sel) {
            s.text({box.x + 1, rowY}, "▌", theme::cyan().with_bg(rowBg.bg));
            // soft trailing glow on selection
            s.text({box.right() - 2, rowY}, "·", theme::cyan_soft().with_bg(rowBg.bg));
        }

        auto lab = (sel ? theme::bright() : theme::text()).with_bg(rowBg.bg);
        if (!sel && local < 0.7f) lab.dim = true;
        if (sel) lab.bold = true;
        s.text({rowX + 1, rowY}, inkcell::text::truncate(it.label, std::max(8, rowW / 2)), lab);

        std::string right = it.hint;
        if (!it.keys.empty()) right = it.keys + "  " + it.hint;
        auto rs = (sel ? theme::italic_accent() : theme::italic_dim()).with_bg(rowBg.bg);
        int rw = inkcell::text::display_width(right);
        int rx = std::max(rowX + 4, box.right() - 2 - rw);
        s.text({rx, rowY}, inkcell::text::truncate(right, box.right() - 2 - rx), rs);
        ++drawn;
    }

    if (pal.filtered.empty()) {
        s.text({box.x + 3, listTop}, "no matches", theme::amber().with_bg(panel.bg));
    }

    // Footer
    auto foot = theme::italic_dim().with_bg(panel.bg);
    s.text({box.x + 2, box.bottom() - 1},
           inkcell::text::truncate(
               std::to_string(pal.filtered.size()) + "  ·  ↑↓/jk  ·  enter run  ·  esc close",
               box.w - 4),
           foot);

    return pal.animating();
}

// Key handler — returns true if consumed. On Enter success, sets *outId.
inline bool handleCmdPaletteKey(CmdPalette& pal, const inkcell::KeyEvent& event,
                                std::string* outId) {
    using inkcell::KeyCode;
    if (!pal.open || pal.closing) return false;

    if (event.code == KeyCode::Escape ||
        (event.code == KeyCode::Character && event.ctrl() &&
         (event.ch == 'p' || event.ch == 'P' || event.ch == 'g' || event.ch == 'G'))) {
        pal.requestClose();
        return true;
    }
    if (event.code == KeyCode::ArrowUp ||
        (event.code == KeyCode::Character && !event.ctrl() && (event.ch == 'k' || event.ch == 'K'))) {
        pal.move(-1);
        return true;
    }
    if (event.code == KeyCode::ArrowDown ||
        (event.code == KeyCode::Character && !event.ctrl() && (event.ch == 'j' || event.ch == 'J'))) {
        pal.move(1);
        return true;
    }
    if (event.code == KeyCode::Enter) {
        if (const auto* it = pal.selected()) {
            if (outId) *outId = it->id;
            pal.requestClose();
        }
        return true;
    }
    if (event.code == KeyCode::Backspace) {
        pal.onBackspace();
        return true;
    }
    if (event.code == KeyCode::Character && !event.ctrl() && event.ch >= 32 && event.ch != 127) {
        // Don't type spaces into query from leader residue — allow space in filter
        pal.onQueryChar(static_cast<char>(event.ch));
        return true;
    }
    return true;  // swallow while open
}

}  // namespace cortex::mk3::ui::components
