#pragma once
// Hub dashboard draw panels — out-of-line MainScene methods (hub peel).
// Included at the bottom of main_scene.hpp after MainScene is complete.

#include "src/ui/model/settings_table.hpp"
#include "src/ui/chat/chat_footer.hpp"
#include <algorithm>
#include <string>
#include <unistd.h>
#include <vector>

#include "src/ui/components/workflow_stage.hpp"

namespace cortex::mk3::ui::scenes {

inline void MainScene::drawAppBar(inkcell::Surface& surface, inkcell::Rect page, layout::DensityTier tier) const {
    auto bar = theme::panel_2();
    // Two rows only — no hairline under bar; wallpaper/ripple owns the gap.
    surface.fill({page.x, page.y, page.w, 2}, " ", bar);

    // Brand: CORTEX bold + MK3 cyan — contiguous "CORTEX MK3" for scan/tests
    auto brand = theme::bright().with_bg(bar.bg);
    auto mk = theme::cyan().with_bg(bar.bg);
    surface.text({page.x, page.y}, "CORTEX ", brand);
    surface.text({page.x + 7, page.y}, "MK3", mk);
    auto sec = theme::italic_accent().with_bg(bar.bg);
    surface.text({page.x + 10, page.y},
                 "  ·  " + std::string(model::dashboardSectionName(model_->dashboard.section)),
                 sec);

    std::string right = activeName() + "  ·  " + nonempty(cfg_.provider, model_->agentProvider) +
                        "/" + nonempty(cfg_.model, model_->agentModel) + "  ·  " +
                        layout::densityName(tier);
    // Prefer live identity
    right = activeName() + "  ·  " + nonempty(model_->agentProvider, nonempty(cfg_.provider, "?")) +
            "/" + nonempty(model_->agentModel, nonempty(cfg_.model, "?")) + "  ·  " +
            layout::densityName(tier);
    int rw = inkcell::text::display_width(right);
    surface.text({std::max(page.x, page.right() - rw), page.y}, right,
                 theme::dim().with_bg(bar.bg));

    std::string sub;
    if (!model_->launchError.empty()) {
        sub = "error  " + model_->launchError;
    } else if (model_->dashboard.searchMode || !model_->dashboard.searchQuery.empty()) {
        sub = "/" + model_->dashboard.searchQuery +
              (model_->dashboard.searchMode ? "█" : "") + "   esc clears";
    } else if (!model_->dashboard.notice.empty()) {
        sub = model_->dashboard.notice;
    } else {
        sub = "session " + suffix(model_->activeSessionId) + "   registry " +
              std::to_string(model_->dashboard.manifests.size()) + "   " +
              (model_->running ? "● live" : "○ idle");
    }
    surface.text({page.x, page.y + 1}, inkcell::text::truncate(sub, page.w),
                 (!model_->launchError.empty() ? theme::red() : theme::dim()).with_bg(bar.bg));
}

inline void MainScene::drawContent(inkcell::Surface& surface, inkcell::Rect frame) const {
    switch (model_->dashboard.section) {
        case model::DashboardSection::Home: drawHome(surface, frame); break;
        case model::DashboardSection::Sessions: drawSessions(surface, frame); break;
        case model::DashboardSection::Manifests: drawManifests(surface, frame); break;
        case model::DashboardSection::Tools: drawTools(surface, frame); break;
        case model::DashboardSection::Relics: drawRelics(surface, frame); break;
        case model::DashboardSection::Workflows: drawWorkflows(surface, frame); break;
        case model::DashboardSection::Settings: drawSettings(surface, frame); break;
    }
}

inline void MainScene::sectionHead(inkcell::Surface& surface, inkcell::Rect frame, const std::string& title,
                 const std::string& subtitle) const {
    // No ─ rules — title + italic subtitle only; field/theme is the chrome.
    surface.text({frame.x, frame.y}, title, theme::bright());
    if (!subtitle.empty())
        surface.text({frame.x, frame.y + 1}, inkcell::text::truncate(subtitle, frame.w),
                     theme::italic_dim());
}

inline void MainScene::metricTile(inkcell::Surface& surface, inkcell::Rect r, const std::string& label,
                const std::string& value, inkcell::Style valueSt) const {
    // Borderless tile — no ─ box chrome
    surface.fill(r, " ", theme::panel_2());
    surface.text({r.x + 2, r.y + 1}, inkcell::text::truncate(label, r.w - 4), theme::dim());
    surface.text({r.x + 2, r.y + 2}, inkcell::text::truncate(value, r.w - 4), valueSt);
}

inline void MainScene::drawHome(inkcell::Surface& surface, inkcell::Rect frame) const {
    const auto& dash = model_->dashboard;
    const std::string name = activeName();
    const std::string engine =
        nonempty(model_->agentProvider, nonempty(cfg_.provider, "?")) + "/" +
        nonempty(model_->agentModel, nonempty(cfg_.model, "?"));
    const bool live = model_->running;
    const bool contentFocus = dash.focus == model::DashboardFocus::Content;

    surface.text({frame.x, frame.y}, inkcell::text::truncate(name, std::max(8, frame.w / 2)),
                 theme::bright());
    surface.text({frame.x + inkcell::text::display_width(name) + 2, frame.y},
                 live ? "● LIVE" : "○ READY", live ? theme::green() : theme::muted());
    if (frame.h > 1)
        surface.text({frame.x, frame.y + 1}, inkcell::text::truncate(engine, frame.w),
                     theme::italic_dim());

    int y = frame.y + 3;
    if (y < frame.bottom()) {
        std::string now = "NOW  ";
        if (live) {
            now += model_->status.empty() ? "running" : model_->status;
            if (model_->pendingOps > 0)
                now += "  ·  " + std::to_string(model_->pendingOps) + " open";
            if (!model_->activeSessionId.empty())
                now += "  ·  " + suffix(model_->activeSessionId);
        } else if (!model_->activeSessionId.empty()) {
            now += "idle  ·  " + suffix(model_->activeSessionId);
        } else {
            now += "idle  ·  no session";
        }
        surface.text({frame.x, y++}, inkcell::text::truncate(now, frame.w),
                     live ? theme::cyan() : theme::muted());
    }

    ++y;
    static const char* kActs[] = {"OPEN", "NEW", "SESS", "REG"};
    int tileW = std::max(10, (frame.w - 3) / 4);
    if (y + 2 < frame.bottom()) {
        for (int i = 0; i < 4; ++i) {
            bool on = contentFocus && dash.homeCursor == i;
            inkcell::Rect t{frame.x + i * (tileW + 1), y, tileW, 2};
            auto bg = on ? theme::panel_3() : theme::panel_2();
            surface.fill(t, " ", bg);
            auto st = (on ? theme::bright() : theme::dim()).with_bg(bg.bg);
            st.bold = on;
            surface.text({t.x + 1, t.y}, on ? "▌" : " ", st);
            surface.text({t.x + 2, t.y}, kActs[i], st);
            const char* sub = i == 0 ? "chat" : i == 1 ? "session" : i == 2 ? "list" : "agents";
            surface.text({t.x + 2, t.y + 1}, sub, theme::muted().with_bg(bg.bg));
        }
        y += 3;
    }

    std::vector<int> recentIdx;
    recentIdx.reserve(6);
    {
        const std::string& liveId = model_->activeSessionId;
        if (!liveId.empty()) {
            for (int i = 0; i < static_cast<int>(dash.sessions.size()); ++i)
                if (dash.sessions[static_cast<size_t>(i)].id == liveId) {
                    recentIdx.push_back(i);
                    break;
                }
        }
        for (int i = 0; i < static_cast<int>(dash.sessions.size()) &&
                         static_cast<int>(recentIdx.size()) < 6;
             ++i) {
            if (!liveId.empty() && dash.sessions[static_cast<size_t>(i)].id == liveId)
                continue;
            recentIdx.push_back(i);
        }
    }

    if (y < frame.bottom()) {
        surface.text({frame.x, y++}, "RECENT", theme::violet());
        if (recentIdx.empty()) {
            surface.text({frame.x, y++}, "  n  new session", theme::muted());
        } else {
            for (int r = 0; r < static_cast<int>(recentIdx.size()) && y < frame.bottom() - 2; ++r) {
                const auto& s = dash.sessions[static_cast<size_t>(recentIdx[static_cast<size_t>(r)])];
                bool on = contentFocus && dash.homeCursor == model::DashboardState::kHomeActionN + r;
                bool cur = !model_->activeSessionId.empty() && s.id == model_->activeSessionId;
                auto bg = on ? theme::panel_3() : theme::panel_bg();
                surface.fill({frame.x, y, frame.w, 1}, " ", bg);
                std::string mark = on ? "▸ " : (cur ? "● " : "  ");
                std::string lab = !s.title.empty() ? s.title
                                                   : (s.agentName.empty() ? suffix(s.id) : s.agentName);
                std::string line = mark + lab + "  ·  " + nonempty(s.agentName, "?") + "  ·  " +
                                   std::to_string(s.turnCount) + "t  ·  " +
                                   model::relativeTimeAgo(s.updated);
                if (cur && live) line += "  LIVE";
                auto st = (on ? theme::bright() : (cur ? theme::text() : theme::muted())).with_bg(bg.bg);
                if (on || cur) st.bold = true;
                surface.text({frame.x, y++}, inkcell::text::truncate(line, frame.w), st);
            }
        }
    }

    y = std::max(y + 1, frame.bottom() - 3);
    if (y < frame.bottom()) {
        char buf[1024] = {0};
        std::string cwd = (::getcwd(buf, sizeof(buf) - 1)) ? buf : ".";
        std::string strip = basename(activeManifest());
        strip += "  ·  ";
        strip += cwd;
        int toolN = model_->rootAgent ? static_cast<int>(model_->rootAgent->toolNames().size()) : 0;
        int subN = model_->rootAgent ? static_cast<int>(model_->rootAgent->subAgentNames().size()) : 0;
        strip += "  ·  ";
        strip += std::to_string(toolN) + " tools  ·  " + std::to_string(subN) + " children";
        surface.text({frame.x, y++}, inkcell::text::truncate(strip, frame.w), theme::dim());
    }
    if (!model_->launchError.empty() && y < frame.bottom()) {
        surface.text({frame.x, y++},
                     inkcell::text::truncate("⚠  " + model_->launchError, frame.w), theme::red());
    }
    if (frame.bottom() - 1 >= frame.y)
        surface.text({frame.x, frame.bottom() - 1},
                     "j/k move  ·  ↵ act  ·  n new  ·  / sessions",
                     theme::italic_dim());
}

inline void MainScene::drawSessions(inkcell::Surface& surface, inkcell::Rect frame) const {
    const auto& dash = model_->dashboard;
    const int totalSess = static_cast<int>(dash.sessions.size());
    const bool liveTurn = model_->running;

    surface.text({frame.x, frame.y}, "SESSIONS", theme::bright());
    std::string stat = "  " + std::to_string(totalSess) + " disk";
    if (!model_->activeSessionId.empty()) stat += liveTurn ? "  ·  1 live" : "  ·  1 open";
    if (dash.searchMode || !dash.searchQuery.empty())
        stat += "  ·  /" + dash.searchQuery;
    surface.text({frame.x + 9, frame.y}, inkcell::text::truncate(stat, frame.w - 9),
                 theme::italic_dim());

    int y = frame.y + 2;
    if (dash.sessions.empty()) {
        surface.text({frame.x, y++}, "Nothing yet.", theme::muted());
        surface.text({frame.x, y++}, "n  new session", theme::text());
        return;
    }

    const bool wide = frame.w >= 72;
    const int previewH = (frame.h >= 16 && wide) ? 3 : 0;
    int listBottom = frame.bottom() - 1 - previewH;

    if (wide && y < listBottom) {
        auto head = theme::dim();
        surface.text({frame.x, y}, "  STATE", head);
        surface.text({frame.x + 10, y}, "AGENT", head);
        surface.text({frame.x + 24, y}, "TITLE", head);
        surface.text({frame.x + frame.w - 22, y}, "TURNS", head);
        surface.text({frame.x + frame.w - 12, y}, "UPDATED", head);
        ++y;
    }

    int visible = std::max(1, listBottom - y);
    int start = std::max(0, std::min(dash.sessionIndex - visible / 2,
                                     static_cast<int>(dash.sessions.size()) - visible));
    if (start < 0) start = 0;

    const auto* sel = dash.selectedSession();
    for (int i = start; i < static_cast<int>(dash.sessions.size()) && y < listBottom; ++i) {
        const auto& s = dash.sessions[static_cast<size_t>(i)];
        bool selected = i == dash.sessionIndex;
        bool active = !model_->activeSessionId.empty() && s.id == model_->activeSessionId;
        auto rowBg = selected ? theme::panel_3() : active ? theme::panel_2() : theme::panel_bg();
        surface.fill({frame.x, y, frame.w, 1}, " ", rowBg);
        if (selected)
            surface.text({frame.x, y}, "▌", theme::cyan().with_bg(rowBg.bg));

        std::string badge = "○";
        if (active && liveTurn) badge = "●";
        else if (active) badge = "◉";
        else if (s.hasUiTimeline) badge = "·";

        std::string label = !s.title.empty() ? s.title : suffix(s.id);
        std::string when = model::relativeTimeAgo(s.updated);
        std::string turns = std::to_string(s.turnCount) + "t";
        auto st = (selected ? theme::bright() : theme::text()).with_bg(rowBg.bg);
        if (active) st.bold = true;

        if (wide) {
            auto bst = (active ? theme::green() : theme::dim()).with_bg(rowBg.bg);
            surface.text({frame.x + 2, y}, badge, bst);
            surface.text({frame.x + 10, y},
                         inkcell::text::truncate(nonempty(s.agentName, "?"), 12), st);
            surface.text({frame.x + 24, y},
                         inkcell::text::truncate(label, std::max(8, frame.w - 48)), st);
            surface.text({frame.x + frame.w - 22, y},
                         inkcell::text::truncate(turns, 6), st);
            surface.text({frame.x + frame.w - 12, y},
                         inkcell::text::truncate(when, 11), theme::dim().with_bg(rowBg.bg));
        } else {
            std::string one = std::string(selected ? "▸ " : "  ") + badge + " " + label + "  " +
                              nonempty(s.agentName, "?") + "  " + turns + "  " + when;
            surface.text({frame.x + 1, y}, inkcell::text::truncate(one, frame.w - 2), st);
        }
        ++y;
    }

    if (previewH > 0 && sel) {
        int py = frame.bottom() - 1 - previewH;
        surface.text({frame.x, py}, inkcell::text::truncate("─ preview", frame.w), theme::dim());
        std::string p1 = nonempty(sel->title, suffix(sel->id)) + "  ·  " +
                         nonempty(sel->agentName, "?") + "  ·  " +
                         nonempty(sel->model, "");
        surface.text({frame.x, py + 1}, inkcell::text::truncate(p1, frame.w), theme::text());
        std::string p2 = suffix(sel->id) + "  ·  " + std::to_string(sel->turnCount) + " turns  ·  " +
                         model::relativeTimeAgo(sel->updated);
        if (sel->hasUiTimeline) p2 += "  ·  ui timeline";
        surface.text({frame.x, py + 2}, inkcell::text::truncate(p2, frame.w), theme::muted());
    }

    surface.text({frame.x, frame.bottom() - 1},
                 inkcell::text::truncate("j/k  ·  ↵ open  ·  n new  ·  d del  ·  x kill  ·  / find",
                                         frame.w),
                 theme::italic_dim());
}

inline void MainScene::drawManifests(inkcell::Surface& surface, inkcell::Rect frame) const {
    const auto& dash = model_->dashboard;
    auto L = layout::manifestLayoutFor(frame.w);

    // Per-kind legend. Lists the binds that affect the focused manifest kind
    // (so the operator never has to guess which key is live for what type).
    // Falls back to the global legend when no selection is focused.
    std::string legend;
    const auto* focused = dash.selectedManifest();
    auto fmt = [](const std::string& k, const char* acts) { return k + " " + acts; };
    if (!focused) {
        legend = "↵ run/launch · 1-9 kind · f facet · z canvas · tab focus · / search · R reload";
    } else if (focused->kind == "agent") {
        legend = fmt("agent", "· ↵ launch · c clone");
    } else if (focused->kind == "workflow") {
        legend = fmt("workflow", "· ↵ open workflow page");
    } else if (focused->kind == "tool") {
        legend = fmt("tool", "· ↵ open tool page");
    } else if (focused->kind == "relic") {
        legend = fmt("relic", "· ↵ open relic page");
    } else if (focused->kind == "feed") {
        legend = fmt("feed", "· ↵ inspect");
    } else if (focused->kind == "harness" || focused->kind == "prompt" || focused->kind == "skill") {
        legend = fmt(focused->kind, "· ↵ inspect");
    } else {
        legend = fmt(focused->kind, "· ↵ inspect");
    }
    sectionHead(surface, frame, "Manifests", legend);

    int y = frame.y + 4;

    // Operable kind chips with indices: [1 agent 12] ...
    std::map<std::string, int> kindCounts;
    // Count from unfiltered would need cache; approximate from current view + labels
    auto allForCount = catalog::discoverManifests(dash.manifestDir);
    for (const auto& m : allForCount) kindCounts[m.kind]++;

    std::vector<components::Chip> kindChips;
    kindChips.push_back({"0", "0:all", static_cast<int>(allForCount.size()),
                         dash.manifestFilter.empty()});
    const auto& facets = model::DashboardState::kindFacets();
    for (size_t i = 1; i < facets.size() && i <= 9; ++i) {
        const std::string& k = facets[i];
        int c = kindCounts.count(k) ? kindCounts[k] : 0;
        kindChips.push_back({std::to_string(i), std::to_string(i) + ":" + k, c,
                             dash.manifestFilter == k});
    }
    int after = y;
    components::drawChipStrip(surface, {frame.x, y, frame.w, 2}, kindChips, &after);
    y = after;

    // Category strip (secondary) — only when useful
    std::map<std::string, int> catCounts;
    for (const auto& m : dash.manifests) catCounts[m.category]++;
    if (catCounts.size() > 1) {
        std::vector<components::Chip> catChips;
        for (const auto& kv : catCounts)
            catChips.push_back({kv.first, kv.first, kv.second, dash.tagFilter == kv.first});
        components::drawChipStrip(surface, {frame.x, y, frame.w, 2}, catChips, &after);
        y = after + 1;
    } else {
        ++y;
    }

    if (dash.manifests.empty()) {
        bool filtered = !dash.manifestFilter.empty() || !dash.tagFilter.empty() ||
                        !dash.searchQuery.empty();
        surface.text({frame.x, y++},
                     filtered ? "No matches — Esc clears filters." : assets::MANIFESTS_EMPTY,
                     theme::amber());
        return;
    }

    // Grouped list
    struct Row {
        bool header = false;
        std::string text;
        int idx = -1;
    };
    std::vector<Row> rows;
    std::string last;
    for (int i = 0; i < static_cast<int>(dash.manifests.size()); ++i) {
        const auto& m = dash.manifests[static_cast<size_t>(i)];
        if (m.category != last) {
            last = m.category;
            rows.push_back({true,
                            "▸ " + m.category + "  (" + std::to_string(catCounts[m.category]) + ")",
                            -1});
        }
        rows.push_back({false, "", i});
    }
    int selRow = 0;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        if (!rows[static_cast<size_t>(i)].header &&
            rows[static_cast<size_t>(i)].idx == dash.manifestIndex) {
            selRow = i;
            break;
        }

    int listW = L.listW;
    int listBottom = frame.bottom();
    // On narrow layouts reserve a floating swipe card strip at the bottom.
    const bool floatingCard = !L.showDetail && frame.h >= 14;
    if (floatingCard) listBottom = frame.bottom() - std::min(12, frame.h / 3);

    int visible = std::max(1, listBottom - y);
    int start = std::max(0, std::min(selRow - visible / 3,
                                     std::max(0, static_cast<int>(rows.size()) - visible)));
    const std::string am = activeManifest();
    const std::string an = activeName();

    const float cardT = dash.cardAnimT();
    const bool swiping = dash.cardAnimating();
    const int nudge = swiping ? components::listNudgeX(dash.cardAnimDir, cardT) : 0;

    for (int ri = start; ri < static_cast<int>(rows.size()) && y < listBottom; ++ri) {
        const auto& row = rows[static_cast<size_t>(ri)];
        if (row.header) {
            surface.text({frame.x, y++}, inkcell::text::truncate(row.text, listW),
                         theme::amber_soft());
            continue;
        }
        const auto& m = dash.manifests[static_cast<size_t>(row.idx)];
        bool selected = row.idx == dash.manifestIndex;
        bool wasPrev = swiping && row.idx == dash.cardPrevIndex;
        bool active =
            m.kind == "agent" && ((!an.empty() && m.name == an) || (!am.empty() && m.path == am));

        int rowX = frame.x + (selected ? nudge : (wasPrev ? -nudge / 2 : 0));
        int rowW = std::max(8, listW - (rowX - frame.x));
        components::drawCardRow(surface, {rowX, y, rowW, 1}, selected, active);
        components::kindChip(surface, rowX + 2, y, m.kind, selected);

        std::string name = std::string(active ? "● " : "  ") + m.name;
        if (m.launchable && m.kind == "agent") name += selected ? "  ↵ launch" : "";
        if (m.kind == "workflow") {
            name += selected ? "  ↵ page" : "  ▷";
            auto live = model_->workflowRun.snapshot();
            if (live.live && (live.path == m.path || live.name == m.name))
                name += "  ●";
        }
        int nameCol = rowX + 8;
        int nameBudget = rowW - 10;
        if (L.showTagColumn && L.tagColMax > 0) {
            nameBudget = std::max(10, rowW - 10 - L.tagColMax);
            components::drawTagChips(surface, nameCol + nameBudget + 1, y, L.tagColMax, m.tags,
                                     4);
        }
        auto nameSt = selected ? theme::bright() : theme::text();
        if (wasPrev) nameSt = theme::italic_dim();
        surface.text({nameCol, y}, inkcell::text::truncate(name, nameBudget), nameSt);
        ++y;
    }

    // ── Detail / floating card with curved swipe ─────────────────
    inkcell::Rect det;
    if (L.showDetail) {
        det = {frame.x + L.detailX, frame.y + 4, L.detailW, frame.h - 5};
    } else if (floatingCard) {
        int ch = frame.bottom() - listBottom;
        det = {frame.x, listBottom, frame.w, ch};
    } else {
        return;
    }

    // Card well — always a field card. Workflow canvas lives on Workflows page.
    surface.fill(det, " ", theme::panel_bg());

    auto paintManifestBody = [&](inkcell::Surface& s, inkcell::Rect inner, float alpha,
                                 const catalog::ManifestEntry& m) {
        bool ghost = alpha < 0.55f;
        auto titleSt = ghost ? theme::muted() : theme::bright();
        auto kindSt = ghost ? theme::dim() : theme::kindAccent(m.kind, true);
        auto bodySt = ghost ? theme::italic_dim() : theme::text();
        int dy = inner.y;
        int ix = inner.x;
        int iw = inner.w;
        if (dy >= inner.bottom()) return;

        // Title row: name + version chip
        std::string title = m.name;
        if (!m.version.empty()) title += "  v" + m.version;
        s.text({ix, dy++}, inkcell::text::truncate(title, iw), titleSt);
        if (dy >= inner.bottom()) return;

        // Kind · category · flags
        std::string meta = std::string(assets::kindLabel(m.kind)) + " · " + m.category;
        if (m.nested) meta += " · nested";
        if (m.builtin) meta += " · builtin";
        if (m.launchable) meta += " · launchable";
        s.text({ix, dy++}, inkcell::text::truncate(meta, iw), kindSt);

        if (!m.summary.empty() && dy < inner.bottom() - 10) {
            for (const auto& line : chat::wrapWordsLossless(m.summary, iw)) {
                if (dy >= inner.bottom() - 10) break;
                s.text({ix, dy++}, line, bodySt);
            }
        }
        if (dy < inner.bottom()) ++dy;

        // Shared identity fields — always useful.
        if (dy < inner.bottom())
            components::fieldLine(s, ix, dy++, iw, "kind", m.kind);
        if (dy < inner.bottom() && !m.version.empty())
            components::fieldLine(s, ix, dy++, iw, "version", m.version);
        if (dy < inner.bottom())
            components::fieldLine(s, ix, dy++, iw, "category", m.category);

        // ── Per-kind card data (operator-visible, not path soup) ──
        if (m.kind == "agent") {
            if (dy < inner.bottom() && (!m.provider.empty() || !m.model.empty()))
                components::fieldLine(s, ix, dy++, iw, "engine",
                                      nonempty(m.provider, "?") + "/" +
                                          nonempty(m.model, "?"));
        } else if (m.kind == "tool") {
            if (dy < inner.bottom() && !m.runtime.empty())
                components::fieldLine(s, ix, dy++, iw, "runtime", m.runtime);
            if (dy < inner.bottom() && !m.entrypoint.empty())
                components::fieldLine(s, ix, dy++, iw, "entry", m.entrypoint);
            // First line of PE description if richer than summary
            if (dy < inner.bottom() && !m.description.empty() &&
                m.description != m.summary) {
                std::string pe = m.description;
                auto nl = pe.find('\n');
                if (nl != std::string::npos) pe = pe.substr(0, nl);
                if (pe.size() > 90) pe = pe.substr(0, 87) + "…";
                if (dy < inner.bottom()) ++dy;
                if (dy < inner.bottom())
                    s.text({ix, dy++}, inkcell::text::truncate(pe, iw), bodySt);
            }
        } else if (m.kind == "skill") {
            // Frontmatter is the product surface for skills — show it.
            if (dy < inner.bottom() && !m.summary.empty()) {
                // summary already painted above when non-empty; frontmatter extras:
            }
            for (const auto& meta : m.extraMeta) {
                if (dy >= inner.bottom() - 4) break;
                auto colon = meta.find(':');
                if (colon != std::string::npos)
                    components::fieldLine(s, ix, dy++, iw, meta.substr(0, colon),
                                          meta.substr(colon + 1));
                else if (dy < inner.bottom())
                    s.text({ix, dy++}, inkcell::text::truncate(meta, iw), bodySt);
            }
            if (m.extraMeta.empty() && dy < inner.bottom())
                components::fieldLine(s, ix, dy++, iw, "module",
                                      m.relPath.empty() ? m.name : m.relPath);
        } else if (m.kind == "relic") {
            // Prefer catalog-parsed endpoints; fall back to last inspect cache.
            std::vector<std::string> eps = m.endpoints;
            if (eps.empty() && dash.relicRun.relicName == m.name)
                eps = dash.relicRun.endpoints;
            if (dy < inner.bottom()) {
                char buf[24];
                std::snprintf(buf, sizeof(buf), "%zu", eps.size());
                components::fieldLine(s, ix, dy++, iw, "endpoints", buf);
            }
            for (size_t i = 0; i < eps.size() && dy < inner.bottom() - 3; ++i) {
                s.text({ix, dy++},
                       inkcell::text::truncate(std::string("  · ") + eps[i], iw),
                       bodySt);
            }
            if (eps.empty() && dy < inner.bottom())
                s.text({ix, dy++}, "no endpoints in manifest", theme::italic_dim());
        } else if (m.kind == "workflow") {
            // Card stays lean — full canvas lives on the Workflows page / ↵ open.
            if (dy < inner.bottom() && !m.summary.empty()) {
                /* summary already shown */
            }
            if (dy < inner.bottom())
                components::fieldLine(s, ix, dy++, iw, "open",
                                      "↵ workflow page · run from there");
        } else if (m.kind == "feed") {
            if (dy < inner.bottom() && !m.runtime.empty())
                components::fieldLine(s, ix, dy++, iw, "runtime", m.runtime);
            if (dy < inner.bottom() && !m.entrypoint.empty())
                components::fieldLine(s, ix, dy++, iw, "entry", m.entrypoint);
        } else if (m.kind == "harness" || m.kind == "prompt") {
            if (dy < inner.bottom())
                components::fieldLine(s, ix, dy++, iw, "module",
                                      m.relPath.empty() ? m.name : m.relPath);
        }

        if (dy < inner.bottom())
            components::fieldLine(s, ix, dy++, iw, "source", nonempty(m.source, "—"));
        if (dy < inner.bottom() && !m.relPath.empty())
            components::fieldLine(s, ix, dy++, iw, "rel", m.relPath);
        // Full path only if room — often noise on short cards
        if (dy < inner.bottom() - 3 && !m.path.empty())
            components::fieldLine(s, ix, dy++, iw, "path", m.path);

        if (dy < inner.bottom()) {
            std::string flags;
            if (m.launchable) flags += "launch ";
            if (m.nested) flags += "nested ";
            if (m.builtin) flags += "builtin ";
            if (flags.empty()) flags = "—";
            components::fieldLine(s, ix, dy++, iw, "flags", flags);
        }

        if (dy < inner.bottom()) {
            ++dy;
            if (dy < inner.bottom()) {
                auto tagHead = ghost ? theme::dim() : theme::violet_soft();
                s.text({ix, dy++}, "TAGS", tagHead);
            }
            std::string all;
            for (const auto& t : m.tags) {
                if (!all.empty()) all += "  ";
                all += "#" + t;
            }
            auto tagSt = ghost ? theme::italic_dim() : theme::italic();
            tagSt.fg = theme::violet_soft().fg;
            for (const auto& line : chat::wrapWordsLossless(all.empty() ? "—" : all, iw)) {
                if (dy >= inner.bottom() - 2) break;
                s.text({ix, dy++}, line, tagSt);
            }
        }
        if (m.launchable && m.kind == "agent" && dy < inner.bottom()) {
            bool isLive =
                (!am.empty() && m.path == am) || (!an.empty() && m.name == an);
            if (dy < inner.bottom()) ++dy;
            if (dy < inner.bottom())
                s.text({ix, dy++},
                       isLive ? "LIVE · enter opens chat" : "ENTER LAUNCHES",
                       ghost ? theme::green_soft() : theme::green());
        }

        if (m.kind == "tool" && !ghost && dy < inner.bottom()) {
            if (dy < inner.bottom()) ++dy;
            if (dy < inner.bottom())
                s.text({ix, dy++}, inkcell::text::truncate("↵ open tool page", iw),
                       theme::green_soft());
        }
        if (m.kind == "relic" && !ghost && dy < inner.bottom()) {
            if (dy < inner.bottom()) ++dy;
            if (dy < inner.bottom())
                s.text({ix, dy++}, inkcell::text::truncate("↵ open relic page", iw),
                       theme::green_soft());
        }
        if (m.kind == "workflow" && !ghost && dy < inner.bottom()) {
            if (dy < inner.bottom()) ++dy;
            if (dy < inner.bottom())
                s.text({ix, dy++},
                       inkcell::text::truncate("↵ open workflow page", iw),
                       theme::green_soft());
        }
    };

    const auto* sel = dash.selectedManifest();
    if (!sel) return;

    const int cardW = det.w;
    const int cardH = det.h;
    // Rest pose fills the well
    auto restPose = components::CardPose{det.x, det.y, 1.f};

    if (swiping && dash.cardPrevIndex >= 0 &&
        dash.cardPrevIndex < static_cast<int>(dash.manifests.size())) {
        const auto& prev = dash.manifests[static_cast<size_t>(dash.cardPrevIndex)];
        auto outP = components::outgoingPose(det, dash.cardAnimDir, cardT);
        auto inP = components::incomingPose(det, dash.cardAnimDir, cardT);

        // Outgoing first (under), then incoming on top
        components::drawSwipedCard(
            surface, det, outP, cardW, cardH, 0.8f,
            [&](inkcell::Surface& s, inkcell::Rect inner, float a) {
                paintManifestBody(s, inner, a, prev);
            });
        components::drawSwipedCard(
            surface, det, inP, cardW, cardH, 0.15f,
            [&](inkcell::Surface& s, inkcell::Rect inner, float a) {
                paintManifestBody(s, inner, a, *sel);
            });
    } else {
        components::drawSwipedCard(
            surface, det, restPose, cardW, cardH, 0.15f,
            [&](inkcell::Surface& s, inkcell::Rect inner, float a) {
                paintManifestBody(s, inner, a, *sel);
            });
    }
}

// AAA options: j/k focus · h/l or enter cycle · no redundant dumps
// Shared list+card registry for Tools / Relics pill pages.
inline void MainScene::drawKindRegistry(inkcell::Surface& surface, inkcell::Rect frame,
                                        const char* kind, const char* title,
                                        const char* legend) const {
    auto& dash = model_->dashboard;
    sectionHead(surface, frame, title, legend);

    int y = frame.y + 4;
    // Ensure facet matches kind so dash.manifests is the right list.
    // (select path sets filter; redraw is read-only — filter is set on enter.)
    std::vector<const catalog::ManifestEntry*> items;
    for (const auto& m : dash.manifests)
        if (m.kind == kind) items.push_back(&m);
    if (items.empty()) {
        // Full discovery fallback if facet cache empty
        auto all = catalog::discoverManifests(dash.manifestDir);
        for (const auto& m : all)
            if (m.kind == kind) items.push_back(&m);
    }

    if (items.empty()) {
        surface.text({frame.x, y},
                     std::string("No ") + kind + " manifests found.", theme::amber());
        return;
    }

    int sel = std::max(0, std::min(dash.manifestIndex, (int)items.size() - 1));
    // Prefer path match against selectedManifest when kinds align
    if (const auto* cur = dash.selectedManifest()) {
        if (cur->kind == kind) {
            for (int i = 0; i < (int)items.size(); ++i)
                if (items[static_cast<size_t>(i)]->path == cur->path) {
                    sel = i;
                    break;
                }
        }
    }

    auto L = layout::manifestLayoutFor(frame.w);
    int listW = L.listW;
    int listBottom = frame.bottom();
    const bool floatingCard = !L.showDetail && frame.h >= 14;
    if (floatingCard) listBottom = frame.bottom() - std::min(12, frame.h / 3);

    int visible = std::max(1, listBottom - y);
    int start = std::max(0, std::min(sel - visible / 3,
                                     std::max(0, (int)items.size() - visible)));

    for (int i = start; i < (int)items.size() && y < listBottom; ++i) {
        const auto& m = *items[static_cast<size_t>(i)];
        bool selected = (i == sel);
        components::drawCardRow(surface, {frame.x, y, listW, 1}, selected, false);
        components::kindChip(surface, frame.x + 2, y, m.kind, selected);
        std::string name = std::string(selected ? "› " : "  ") + m.name;
        if (selected) name += "  ↵ open";
        // Meta chips on the right of the name when room
        std::string extra;
        if (m.kind == "tool" && !m.runtime.empty()) extra = m.runtime;
        if (m.kind == "relic" && !m.endpoints.empty())
            extra = std::to_string(m.endpoints.size()) + " ep";
        int nameCol = frame.x + 8;
        int nameBudget = listW - 10;
        if (!extra.empty() && nameBudget > 20) {
            int ew = inkcell::text::display_width(extra) + 1;
            nameBudget = std::max(8, nameBudget - ew);
            surface.text({nameCol + nameBudget + 1, y},
                         inkcell::text::truncate(extra, ew), theme::dim());
        }
        surface.text({nameCol, y},
                     inkcell::text::truncate(name, nameBudget),
                     selected ? theme::bright() : theme::text());
        ++y;
    }

    // Detail card — reuse paint path via a temporary Manifests-style body.
    inkcell::Rect det;
    if (L.showDetail)
        det = {frame.x + L.detailX, frame.y + 4, L.detailW, frame.h - 5};
    else if (floatingCard)
        det = {frame.x, listBottom, frame.w, frame.bottom() - listBottom};
    else
        return;

    surface.fill(det, " ", theme::panel_bg());
    if (sel < 0 || sel >= (int)items.size()) return;
    const auto& m = *items[static_cast<size_t>(sel)];

    int dy = det.y + 1;
    int ix = det.x + 1;
    int iw = det.w - 2;
    if (iw < 8 || dy >= det.bottom()) return;

    std::string ttl = m.name;
    if (!m.version.empty()) ttl += "  v" + m.version;
    surface.text({ix, dy++}, inkcell::text::truncate(ttl, iw), theme::bright());
    if (dy >= det.bottom()) return;

    std::string meta = std::string(assets::kindLabel(m.kind)) + " · " + m.category;
    if (m.builtin) meta += " · builtin";
    surface.text({ix, dy++}, inkcell::text::truncate(meta, iw), theme::kindAccent(m.kind, true));

    if (!m.summary.empty() && dy < det.bottom() - 6) {
        for (const auto& line : chat::wrapWordsLossless(m.summary, iw)) {
            if (dy >= det.bottom() - 6) break;
            surface.text({ix, dy++}, line, theme::text());
        }
    }
    if (dy < det.bottom()) ++dy;

    if (m.kind == "tool") {
        if (dy < det.bottom() && !m.runtime.empty())
            components::fieldLine(surface, ix, dy++, iw, "runtime", m.runtime);
        if (dy < det.bottom() && !m.entrypoint.empty())
            components::fieldLine(surface, ix, dy++, iw, "entry", m.entrypoint);
        if (dy < det.bottom() && !m.description.empty() && m.description != m.summary) {
            std::string pe = m.description;
            auto nl = pe.find('\n');
            if (nl != std::string::npos) pe = pe.substr(0, nl);
            if (pe.size() > 100) pe = pe.substr(0, 97) + "…";
            if (dy < det.bottom())
                surface.text({ix, dy++}, inkcell::text::truncate(pe, iw), theme::text());
        }
    } else if (m.kind == "relic") {
        if (dy < det.bottom()) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%zu", m.endpoints.size());
            components::fieldLine(surface, ix, dy++, iw, "endpoints", buf);
        }
        for (size_t i = 0; i < m.endpoints.size() && dy < det.bottom() - 2; ++i)
            surface.text({ix, dy++},
                         inkcell::text::truncate(std::string("  · ") + m.endpoints[i], iw),
                         theme::text());
        if (m.endpoints.empty() && dy < det.bottom())
            surface.text({ix, dy++}, "no endpoints in manifest", theme::italic_dim());
    }

    if (dy < det.bottom() && !m.relPath.empty())
        components::fieldLine(surface, ix, dy++, iw, "rel", m.relPath);

    if (dy < det.bottom()) {
        ++dy;
        if (dy < det.bottom()) {
            std::string all;
            for (const auto& t : m.tags) {
                if (!all.empty()) all += "  ";
                all += "#" + t;
            }
            surface.text({ix, dy++}, "TAGS", theme::violet_soft());
            for (const auto& line : chat::wrapWordsLossless(all.empty() ? "—" : all, iw)) {
                if (dy >= det.bottom() - 1) break;
                auto tagSt = theme::italic();
                tagSt.fg = theme::violet_soft().fg;
                surface.text({ix, dy++}, line, tagSt);
            }
        }
    }
    if (dy < det.bottom()) {
        ++dy;
        if (dy < det.bottom())
            surface.text({ix, dy},
                         m.kind == "tool" ? "↵ open tool page" : "↵ open relic page",
                         theme::green_soft());
    }
}

inline void MainScene::drawTools(inkcell::Surface& surface, inkcell::Rect frame) const {
    drawKindRegistry(surface, frame, "tool", "Tools",
                     "j/k select · ↵ open tool page · a all manifests · / search");
}

inline void MainScene::drawRelics(inkcell::Surface& surface, inkcell::Rect frame) const {
    drawKindRegistry(surface, frame, "relic", "Relics",
                     "j/k select · ↵ open relic page · a all manifests");
}

// Dedicated Workflows hub page — list + live canvas stage (not jammed into cards).
inline void MainScene::drawWorkflows(inkcell::Surface& surface, inkcell::Rect frame) const {
    auto& dash = model_->dashboard;
    auto all = catalog::discoverManifests(dash.manifestDir);
    std::vector<const catalog::ManifestEntry*> wfs;
    for (const auto& m : all)
        if (m.kind == "workflow") wfs.push_back(&m);
    if (wfs.empty())
        for (const auto& m : dash.manifests)
            if (m.kind == "workflow") wfs.push_back(&m);

    auto live = model_->workflowRun.snapshot();
    bool anyLive = live.live;
    sectionHead(surface, frame, "Workflows",
                std::to_string(wfs.size()) + " workflows" +
                    (anyLive ? "  ·  ● " + std::string(model::runStatusLabel(live.status)) : ""));

    if (wfs.empty()) {
        surface.text({frame.x, frame.y + 4}, "No workflows in manifests/.", theme::amber());
        return;
    }

    int y = frame.y + 4;
    int sel = 0;
    const auto* cur = dash.selectedManifest();
    for (int i = 0; i < (int)wfs.size(); ++i)
        if (cur && wfs[(size_t)i]->path == cur->path) {
            sel = i;
            break;
        }

    // List: 2-line chrome rows | stage: canvas, 2-col gap, no divider glyph.
    const int listW = std::min(42, std::max(24, frame.w / 3));
    int listH = std::max(4, frame.bottom() - y - 1);
    int vis = std::max(1, listH / 2);
    int start = std::max(0, std::min(sel - vis / 3, std::max(0, (int)wfs.size() - vis)));

    for (int i = start; i < (int)wfs.size() && (i - start) < vis; ++i) {
        const auto& m = *wfs[(size_t)i];
        bool selected = (i == sel);
        bool liveHere = live.live && (live.path == m.path || live.name == m.name);
        int r0 = y + (i - start) * 2;
        if (r0 + 1 >= frame.bottom()) break;
        components::fillRect(surface, {frame.x, r0, listW, 2},
                             selected ? theme::panel_3() : theme::panel_bg());
        if (selected)
            components::accentBar(surface, frame.x, r0, 2, theme::footer_accent_focus());
        std::string head = (selected ? "› " : "  ") + m.name;
        surface.text({frame.x + 2, r0}, inkcell::text::truncate(head, listW - 6),
                     selected ? theme::bright() : theme::text());
        int steps = 0;
        {
            auto& e = workflows::WorkflowEngine::instance().load(m.path);
            if (e.isValid()) steps = (int)e.manifest().steps.size();
        }
        std::string rr = liveHere ? "● live" : (steps > 0 ? std::to_string(steps) + " steps" : "");
        if (!rr.empty()) {
            int rw = (int)inkcell::text::display_width(rr);
            surface.text({frame.right() - 1 - rw, r0}, rr,
                         liveHere ? theme::green() : theme::dim());
        }
        if (!m.summary.empty())
            surface.text({frame.x + 3, r0 + 1},
                         inkcell::text::truncate(m.summary, listW - 6), theme::italic_dim());
    }

    inkcell::Rect stage{frame.x + listW + 2, y, std::max(12, frame.w - listW - 2),
                        frame.bottom() - y};
    if (stage.w >= 12 && stage.h >= 6 && sel >= 0 && sel < (int)wfs.size())
        drawWorkflowStage(surface, stage, *wfs[(size_t)sel], gfx::nowSeconds());
}
inline void MainScene::drawSettings(inkcell::Surface& surface, inkcell::Rect frame) const {
    using model::kSettingsRows;
    using model::kSettingsRowN;
    using model::SettingsRowKind;
    using model::SettingsOpt;
    using model::settingsOptAt;
    using model::settingsIsCarousel;
    using model::settingsBindHint;
    using model::settingsRowFocusable;

    auto& dash = model_->dashboard;
    if (!settingsRowFocusable(dash.settingsFocus))
        dash.settingsFocus = model::settingsFirstFocus();

    // Title
    surface.text({frame.x, frame.y}, "SETTINGS", theme::bright());
    surface.text({frame.x + 10, frame.y}, "cabinet", theme::italic_dim());

    int y = frame.y + 2;
    // Field preview strip (compact)
    int previewH = std::min(4, std::max(2, frame.h / 8));
    if (y + previewH + 6 < frame.bottom()) {
        inkcell::Rect prev{frame.x, y, frame.w, previewH};
        if (gfx::fieldEnabled()) {
            gfx::drawFieldBg(surface, prev, gfx::themeVariantIndex(), gfx::nowSeconds());
        } else {
            surface.fill(prev, " ", theme::base_bg());
        }
        std::string tag = gfx::fieldEnabled()
                              ? std::string("FIELD  ·  ") + gfx::activeFieldName()
                              : std::string("FIELD  ·  OFF");
        auto tagSt = theme::bright();
        tagSt.bg = inkcell::Color::rgb(0, 0, 0);
        surface.text({prev.x + 2, prev.y + previewH / 2},
                     inkcell::text::truncate(tag, prev.w - 4), tagSt);
        y = prev.bottom() + 1;
    }

    const int listTop = y;
    const int listBot = frame.bottom() - 1;
    const int vis = std::max(1, listBot - listTop);

    // Scroll so focus is visible
    int focus = dash.settingsFocus;
    if (focus >= dash.settingsScroll + vis) dash.settingsScroll = focus - vis + 1;
    if (focus < dash.settingsScroll) dash.settingsScroll = focus;
    if (dash.settingsScroll < 0) dash.settingsScroll = 0;
    if (dash.settingsScroll > std::max(0, kSettingsRowN - vis))
        dash.settingsScroll = std::max(0, kSettingsRowN - vis);

    auto valueFor = [&](SettingsOpt o) -> std::string {
        switch (o) {
            case SettingsOpt::Theme: return upperCopy(theme::name());
            case SettingsOpt::Field: return gfx::fieldEnabled() ? "ON" : "OFF";
            case SettingsOpt::Shader:
                return gfx::fieldEnabled() ? upperCopy(gfx::activeFieldName()) : std::string("—");
            case SettingsOpt::Thoughts: return model_->showThoughts ? "ON" : "OFF";
            case SettingsOpt::Truncate: return model_->truncateBodies ? "ON" : "OFF";
            case SettingsOpt::InputFmt: return upperCopy(bodyRenderModeName(model_->actionBodyMode));
            case SettingsOpt::OutputFmt: return upperCopy(bodyRenderModeName(model_->resultBodyMode));
            case SettingsOpt::Raw: return model_->showRaw ? "ON" : "OFF";
            case SettingsOpt::ChatField:
                return model_->chatFieldEnabled
                           ? (gfx::fieldEnabled() ? std::string("ON · ") + gfx::activeFieldName()
                                                  : std::string("ON · hub off"))
                           : "OFF";
            case SettingsOpt::FooterPane:
                return upperCopy(chat::footerPaneName(
                    static_cast<chat::ChatFooterPane>(model_->chatFooterPane)));
            case SettingsOpt::BodyView: {
                static const char* kN[] = {"STREAM", "COMPACT"};
                int b = model_->chatBodyMode;
                if (b != 1) b = 0;
                return kN[b];
            }
            case SettingsOpt::AutoFollow: return model_->autoFollowLive ? "ON" : "OFF";
            case SettingsOpt::Zen: return model_->zenMode ? "ON" : "OFF";
            case SettingsOpt::NavPill: return model_->navPillEnabled ? "ON" : "OFF";
            case SettingsOpt::PillHide:
                return model_->navPillHideMs <= 0
                           ? "NEVER"
                           : (std::to_string(model_->navPillHideMs / 1000) + "S");
            case SettingsOpt::Cwd: {
                if (model_->dashboard.cwdEditMode)
                    return model_->dashboard.cwdEditBuffer + "█";
                if (!model_->sessionCwd.empty()) return model_->sessionCwd;
                char buf[1024] = {0};
                if (getcwd(buf, sizeof(buf) - 1)) return buf;
                return "—";
            }
            case SettingsOpt::RememberCwd: return model_->rememberLastCwd ? "ON" : "OFF";
            case SettingsOpt::KeepLive: return model_->keepLiveOnCwdChange ? "ON" : "OFF";
            case SettingsOpt::SessionScope: return model_->globalSessions ? "GLOBAL" : "PROJECT";
            case SettingsOpt::DevMode: return model_->uiDevMode ? "ON" : "OFF";
            default: return "?";
        }
    };

    for (int row = 0; row < vis; ++row) {
        int i = dash.settingsScroll + row;
        if (i >= kSettingsRowN) break;
        const auto& sr = kSettingsRows[i];
        inkcell::Rect line{frame.x, listTop + row, frame.w, 1};
        if (sr.kind == SettingsRowKind::Head) {
            auto st = theme::violet();
            st.bold = true;
            surface.fill(line, " ", theme::base_bg());
            surface.text({line.x + 1, line.y}, sr.head ? sr.head : "", st);
            continue;
        }
        bool on = (i == focus);
        auto bg = on ? theme::panel_3() : theme::panel_2();
        surface.fill(line, " ", bg);
        if (on)
            surface.put({line.x, line.y}, "▌", theme::cyan().with_bg(bg.bg));
        auto labSt = (on ? theme::bright() : theme::muted()).with_bg(bg.bg);
        if (on) labSt.bold = true;
        surface.text({line.x + 2, line.y},
                     inkcell::text::truncate(sr.label ? sr.label : "?", 18), labSt);
        std::string val = valueFor(sr.opt);
        if (settingsIsCarousel(sr.opt) && on) val = "◂ " + val + " ▸";
        int vw = inkcell::text::display_width(val);
        auto valSt = (on ? theme::bright() : theme::text()).with_bg(bg.bg);
        if (on) valSt.bold = true;
        surface.text({std::max(line.x + 22, line.right() - vw - 10), line.y},
                     inkcell::text::truncate(val, line.w - 24), valSt);
        auto bindSt = theme::italic_dim().with_bg(bg.bg);
        const char* bh = settingsBindHint(sr.opt);
        int bw = inkcell::text::display_width(bh);
        surface.text({line.right() - bw - 1, line.y}, bh, bindSt);
    }

    if (listBot < frame.bottom()) {
        surface.text({frame.x, listBot},
                     inkcell::text::truncate(
                         std::string("j/k move  ·  h/l or enter cycle  ·  ") + uiPrefsPath(),
                         frame.w),
                     theme::italic_dim());
    }
}


inline void MainScene::drawWorkflowStage(inkcell::Surface& surface, inkcell::Rect frame,
                       const catalog::ManifestEntry& m, float tsec) const {
    components::drawWorkflowDetail(surface, frame, m, *model_, tsec);
}
}  // namespace cortex::mk3::ui::scenes
