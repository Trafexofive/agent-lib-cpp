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
        {"nav.tools", "Tools", "tool registry · open page", "NAV", "t"},
        {"nav.relics", "Relics", "relic registry · open page", "NAV", "l"},
        {"nav.workflows", "Workflows", "canvas · run · stop", "NAV", "w"},
        {"nav.settings", "Settings", "theme · shaders · keys", "NAV", "?"},
        {"act.shader", "Next shader", "cycle field background", "ACTION", "S"},
        {"act.shader_off", "Shader off", "solid theme background", "ACTION", "B"},
        {"nav.chat", "Open chat", "switch to agent scene", "NAV", "c"},
        {"act.refresh", "Refresh hub", "reload sessions + manifests", "ACTION", "R"},
        {"act.theme", "Toggle theme", "graphite ↔ neon", "ACTION", "T"},
        {"act.launch", "Launch / run selected", "agent hot-swap · workflow run", "ACTION", "↵"},
        {"act.wf_run", "Run workflow", "execute selected workflow manifest", "ACTION", "↵"},
        {"act.wf_stop", "Stop workflow", "cancel live workflow run", "ACTION", "Esc"},
        {"act.wf_resume", "Resume last run", "re-run last workflow path", "ACTION", "r"},
        {"nav.wf_facet", "Workflows facet", "filter manifests to workflows", "NAV", "4"},
        {"act.wf_canvas", "Toggle workflow canvas", "expand infinite canvas stage", "ACTION", "f"},
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
        {"chat.scroll_down", "Fine scroll down", "transcript +1 line", "CHAT", "^J"},
        {"chat.scroll_up", "Fine scroll up", "transcript -1 line", "CHAT", "^K"},
        {"chat.clear", "Clear transcript", "wipe current view", "CHAT", "/clear"},
        {"chat.stop", "Stop agent loop", "kill switch", "CHAT", "^X"},
        {"chat.replay", "Replay first prompt", "wipe + re-run first YOU", "CHAT", "/replay"},
        {"chat.collapse", "Collapse block", "toggle selected body", "CHAT", "z"},
        {"chat.bang", "Shell from composer", "!cmd run · !!cmd insert", "PROMPT", "!"},
        {"chat.at", "Path complete", "@path + Tab", "PROMPT", "@"},
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

// Draw palette — AAA options modal (same language as Settings).
inline bool drawCmdPalette(inkcell::Surface& s, inkcell::Rect page, CmdPalette& pal) {
    pal.finishCloseIfNeeded();
    if (!pal.active()) return false;

    float vis = pal.visibility();
    if (vis <= 0.01f) return pal.animating();

    drawScrim(s, page, vis);

    float scaleT = pal.closing ? cpSmoother(vis) : cpEaseOutBack(std::min(1.f, vis * 1.05f));
    scaleT = cpClamp01(scaleT);

    int targetW = std::min(page.w - 8, std::max(52, page.w * 4 / 7));
    int targetH = std::min(page.h - 8, std::max(14, page.h * 3 / 5));
    int boxW = std::max(28, static_cast<int>(std::lround(targetW * (0.78f + 0.22f * scaleT))));
    int boxH = std::max(10, static_cast<int>(std::lround(targetH * (0.62f + 0.38f * scaleT))));

    int rise = static_cast<int>(std::lround((1.f - vis) * 5.f));
    int boxX = page.x + (page.w - boxW) / 2;
    int boxY = page.y + (page.h - boxH) / 3 + rise;
    inkcell::Rect box{boxX, boxY, boxW, boxH};

    // Depth shadow (no ─ chrome)
    if (vis > 0.25f) {
        auto sh = inkcell::Style::normal()
                      .with_bg(inkcell::Color::rgb(0, 0, 0))
                      .with_fg(inkcell::Color::rgb(0, 0, 0));
        s.fill({box.x + 2, box.y + 1, box.w, box.h}, " ", sh);
    }

    auto panel = theme::panel_2();
    panel.bg = theme::color(inkcell::Color::rgb(26, 26, 32), inkcell::Color::rgb(12, 18, 30));
    s.fill(box, " ", panel);

    // Top freckle accent (▀ not ─)
    auto ac = theme::cyan().with_bg(panel.bg);
    for (int x = box.x + 2; x < box.right() - 2; ++x) {
        float u = static_cast<float>(x - box.x) / static_cast<float>(std::max(1, box.w));
        ac.fg = theme::color(
            inkcell::Color::rgb(static_cast<uint8_t>(90 + 50 * u), static_cast<uint8_t>(150 - 10 * u),
                                static_cast<uint8_t>(170)),
            inkcell::Color::rgb(static_cast<uint8_t>(80 + 90 * u), static_cast<uint8_t>(210),
                                255));
        s.text({x, box.y}, "▀", ac);
    }

    int y = box.y + 1;
    s.text({box.x + 2, y}, "COMMANDS", theme::bright().with_bg(panel.bg));
    s.text({box.x + 12, y},
           inkcell::text::truncate(std::to_string(pal.filtered.size()) + " / " +
                                       std::to_string(pal.all.size()),
                                   12),
           theme::italic_dim().with_bg(panel.bg));
    ++y;

    // Search rail
    bool blink = (static_cast<int>(CmdPalette::nowMs() / 530) % 2) == 0;
    std::string qline = "  ⌕  " + pal.query + (blink ? "▌" : " ");
    auto qbg = theme::panel_3();
    s.fill({box.x + 1, y, box.w - 2, 1}, " ", qbg);
    s.text({box.x + 1, y}, inkcell::text::truncate(qline, box.w - 2),
           theme::cyan().with_bg(qbg.bg));
    ++y;
    ++y;  // air

    int listTop = y;
    int listBot = box.bottom() - 2;
    int visible = std::max(1, listBot - listTop);

    // Count only item rows for scroll (headers still drawn)
    int start = 0;
    if (pal.index > 0) {
        // keep selection near top-third
        start = std::max(0, pal.index - visible / 3);
    }

    float rowGate = pal.closing ? 1.f : cpSmoother(std::min(1.f, (vis - 0.12f) / 0.88f));
    std::string lastGroup;
    int drawn = 0;

    for (int i = start; i < static_cast<int>(pal.filtered.size()) && drawn < visible; ++i) {
        const auto& it = pal.filtered[static_cast<size_t>(i)];
        if (it.group != lastGroup) {
            if (drawn >= visible) break;
            lastGroup = it.group;
            if (rowGate > 0.2f) {
                auto gs = theme::violet_soft().with_bg(panel.bg);
                gs.italic = true;
                s.text({box.x + 2, listTop + drawn},
                       inkcell::text::truncate(it.group, box.w - 4), gs);
            }
            ++drawn;
            if (drawn >= visible) break;
        }

        float local = cpClamp01(rowGate * 1.15f - 0.035f * static_cast<float>(drawn));
        if (local < 0.04f) {
            ++drawn;
            continue;
        }

        bool sel = (i == pal.index);
        int rowY = listTop + drawn;
        int slide = static_cast<int>(std::lround((1.f - local) * 2.f));
        auto rowBg = sel ? theme::panel_3() : panel;
        s.fill({box.x + 1, rowY, box.w - 2, 1}, " ", rowBg);
        if (sel) s.text({box.x + 1, rowY}, "▌", theme::cyan().with_bg(rowBg.bg));

        auto lab = (sel ? theme::bright() : theme::text()).with_bg(rowBg.bg);
        if (sel) lab.bold = true;
        if (!sel && local < 0.65f) lab.dim = true;
        int lx = box.x + 3 + slide;
        s.text({lx, rowY}, inkcell::text::truncate(it.label, box.w / 2), lab);

        // Right: bind only (hint is redundant with label for most) — show hint if no keys
        std::string right = !it.keys.empty() ? it.keys : it.hint;
        auto rs = (sel ? theme::italic_accent() : theme::italic_dim()).with_bg(rowBg.bg);
        int rw = inkcell::text::display_width(right);
        s.text({std::max(lx + 8, box.right() - 2 - rw), rowY},
               inkcell::text::truncate(right, box.w / 3), rs);
        ++drawn;
    }

    if (pal.filtered.empty()) {
        s.text({box.x + 3, listTop}, "no matches", theme::amber().with_bg(panel.bg));
    } else if (const auto* sel = pal.selected()) {
        // Detail strip — one line of context for selection (no key dump)
        s.text({box.x + 2, box.bottom() - 1},
               inkcell::text::truncate(std::string("▸ ") + sel->label +
                                           (sel->hint.empty() ? "" : "  —  " + sel->hint),
                                       box.w - 4),
               theme::italic_dim().with_bg(panel.bg));
    }

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
