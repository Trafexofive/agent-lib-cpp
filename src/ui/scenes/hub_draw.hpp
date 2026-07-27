#pragma once
// Hub dashboard draw panels — out-of-line MainScene methods (hub peel).
// Included at the bottom of main_scene.hpp after MainScene is complete.

#include <algorithm>
#include <string>
#include <vector>

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
    // ── Hero identity ────────────────────────────────────────────
    const std::string name = activeName();
    const std::string engine =
        nonempty(model_->agentProvider, nonempty(cfg_.provider, "?")) + "/" +
        nonempty(model_->agentModel, nonempty(cfg_.model, "?"));
    const bool live = model_->running;
    const char* pulse = live ? "● LIVE" : "○ READY";

    surface.text({frame.x, frame.y}, inkcell::text::truncate(name, frame.w / 2), theme::bright());
    surface.text({frame.x + inkcell::text::display_width(name) + 2, frame.y},
                 pulse, live ? theme::green() : theme::muted());
    if (frame.h > 1)
        surface.text({frame.x, frame.y + 1},
                     inkcell::text::truncate(engine, frame.w), theme::italic_dim());

    int y = frame.y + 3;

    // ── KPI strip (real counts only) ─────────────────────────────
    int agents = 0, tools = 0, feeds = 0, other = 0;
    for (const auto& m : model_->dashboard.manifests) {
        if (m.kind == "agent") ++agents;
        else if (m.kind == "tool") ++tools;
        else if (m.kind == "feed") ++feeds;
        else ++other;
    }
    int sessN = static_cast<int>(model_->dashboard.sessions.size());
    int toolN = model_->rootAgent ? static_cast<int>(model_->rootAgent->toolNames().size()) : 0;
    int subN = model_->rootAgent ? static_cast<int>(model_->rootAgent->subAgentNames().size()) : 0;

    struct Kpi { const char* lab; std::string val; inkcell::Style st; };
    std::vector<Kpi> kpis = {
        {"REGISTRY", std::to_string(static_cast<int>(model_->dashboard.manifests.size())), theme::cyan()},
        {"AGENTS", std::to_string(agents), theme::bright()},
        {"SESSIONS", std::to_string(sessN), theme::text()},
        {"TOOLS", std::to_string(toolN), theme::text()},
        {"SUBS", std::to_string(subN), theme::muted()},
    };
    int cols = std::min(static_cast<int>(kpis.size()), std::max(3, frame.w / 18));
    int gap = 1;
    int tileW = std::max(12, (frame.w - gap * (cols - 1)) / cols);
    int tileH = 3;
    if (y + tileH < frame.bottom()) {
        for (int i = 0; i < cols; ++i) {
            inkcell::Rect t{frame.x + i * (tileW + gap), y, tileW, tileH};
            surface.fill(t, " ", theme::panel_2());
            // top accent freckle
            surface.text({t.x + 1, t.y}, "▀",
                         (i == 0 ? theme::cyan() : theme::dim()).with_bg(theme::panel_2().bg));
            surface.text({t.x + 2, t.y}, inkcell::text::truncate(kpis[static_cast<size_t>(i)].lab, tileW - 3),
                         theme::dim().with_bg(theme::panel_2().bg));
            auto vs = kpis[static_cast<size_t>(i)].st;
            vs.bg = theme::panel_2().bg;
            vs.bold = true;
            surface.text({t.x + 2, t.y + 1},
                         inkcell::text::truncate(kpis[static_cast<size_t>(i)].val, tileW - 3), vs);
        }
        y += tileH + 2;
    }

    // ── Two-column: runtime | loadout ────────────────────────────
    int colW = frame.w >= 70 ? (frame.w - 3) / 2 : frame.w;
    int leftX = frame.x;
    int rightX = frame.w >= 70 ? frame.x + colW + 3 : frame.x;
    int yL = y, yR = y;

    auto head = [&](int x, int& yy, const char* t, inkcell::Style st) {
        if (yy >= frame.bottom()) return;
        surface.text({x, yy++}, t, st);
    };
    auto row = [&](int x, int& yy, int w, const char* k, const std::string& v) {
        if (yy >= frame.bottom()) return;
        components::fieldLine(surface, x, yy++, w, k, v);
    };

    head(leftX, yL, "RUNTIME", theme::cyan_soft());
    row(leftX, yL, colW, "manifest",
        activeManifest().empty() ? "—" : basename(activeManifest()));
    row(leftX, yL, colW, "session",
        model_->activeSessionId.empty() ? "—" : suffix(model_->activeSessionId));
    row(leftX, yL, colW, "harness", basename(cfg_.harnessPath));
    row(leftX, yL, colW, "system", basename(cfg_.systemPromptPath));
    row(leftX, yL, colW, "persona", basename(cfg_.personaPath));
    row(leftX, yL, colW, "turn", live ? "running" : "idle");

    if (frame.w >= 70) {
        head(rightX, yR, "LOADOUT", theme::amber_soft());
        auto joinN = [&](const std::vector<std::string>& names, int maxN) {
            std::string j;
            int n = 0;
            for (const auto& nm : names) {
                if (n >= maxN) {
                    j += " · +";
                    j += std::to_string(static_cast<int>(names.size()) - maxN);
                    break;
                }
                if (!j.empty()) j += " · ";
                j += nm;
                ++n;
            }
            return j.empty() ? std::string("—") : j;
        };
        if (model_->rootAgent) {
            row(rightX, yR, colW, "tools", joinN(model_->rootAgent->toolNames(), 6));
            row(rightX, yR, colW, "agents", joinN(model_->rootAgent->subAgentNames(), 5));
            row(rightX, yR, colW, "feeds", joinN(model_->rootAgent->feedNames(), 4));
        } else {
            row(rightX, yR, colW, "tools", "—");
            row(rightX, yR, colW, "agents", "—");
            row(rightX, yR, colW, "feeds", "—");
        }
        row(rightX, yR, colW, "registry", std::to_string(agents) + "a · " +
                                              std::to_string(tools) + "t · " +
                                              std::to_string(feeds) + "f");
        row(rightX, yR, colW, "field",
            gfx::fieldEnabled() ? std::string(gfx::activeFieldName()) : std::string("off"));
    }

    y = std::max(yL, yR) + 1;

    // ── Recent sessions (actionable) ─────────────────────────────
    if (y + 2 < frame.bottom() && !model_->dashboard.sessions.empty()) {
        surface.text({frame.x, y++}, "RECENT", theme::violet());
        int shown = 0;
        for (const auto& s : model_->dashboard.sessions) {
            if (shown >= 4 || y >= frame.bottom() - 1) break;
            bool cur = !model_->activeSessionId.empty() && s.id == model_->activeSessionId;
            std::string line = std::string(cur ? "▸ " : "  ") +
                               (s.agentName.empty() ? s.id : s.agentName);
            line += "  ·  " + std::to_string(s.turnCount) + "t";
            if (!s.updated.empty()) line += "  ·  " + s.updated;
            surface.text({frame.x, y++}, inkcell::text::truncate(line, frame.w),
                         cur ? theme::bright() : theme::muted());
            ++shown;
        }
    }

    if (!model_->launchError.empty() && y < frame.bottom()) {
        surface.text({frame.x, y},
                     inkcell::text::truncate("⚠  " + model_->launchError, frame.w),
                     theme::red());
    }

    // Footer action — one line, no key encyclopedia
    if (frame.bottom() - 1 > y)
        surface.text({frame.x, frame.bottom() - 1},
                     "enter open chat  ·  a registry  ·  s sessions",
                     theme::italic_dim());
}

inline void MainScene::drawSessions(inkcell::Surface& surface, inkcell::Rect frame) const {
    // Vet-fix smarter sessions page: LIVE chip on the active session,
    // column-rich row, active-delete guard refused via notice (not a
    // hint-only silent failure), empty-state with shortcut, scoped
    // header KPI. Same AAA grammar as Home.
    const auto& dash = model_->dashboard;

    // Hero + KPI label
    surface.text({frame.x, frame.y}, "SESSIONS", theme::bright());
    const int totalSess = static_cast<int>(dash.sessions.size());
    const int activeFlag = !model_->activeSessionId.empty() ? 1 : 0;
    std::string stat =
        " " + std::to_string(totalSess) +
        " on disk" + (activeFlag ? "  ·  1 active" : "");
    surface.text({frame.x + 9, frame.y}, inkcell::text::truncate(stat, frame.w - 9),
                 theme::italic_dim());
    if (frame.h > 1) {
        int liveTurn = model_->running ? 1 : 0;
        std::string meta = std::string("enter resume · n new · d delete · e export · x kill-live · / search") +
                            (liveTurn ? std::string("  ·  ● live") : std::string("  ·  ○ idle"));
        surface.text({frame.x, frame.y + 1},
                     inkcell::text::truncate(meta, frame.w), theme::italic_dim());
    }

    int y = frame.y + 3;
    if (dash.sessions.empty()) {
        inkcell::Style tip = theme::muted();
        surface.text({frame.x, y++}, "Nothing yet.", tip);
        surface.text({frame.x, y++}, "Press  n  to create and open chat.", theme::text());
        surface.text({frame.x, y}, "Press  e  on a later session to export as .json.", theme::dim());
        return;
    }

    // Visible column headers
    if (frame.w >= 64) {
        int rowW = frame.w;
        int idsX = frame.x;
        int liveX = frame.x + 4;
        int agentX = frame.x + 12;
        int turnsX = std::max(agentX + 18, frame.x + rowW - 28);
        int updatedX = std::max(turnsX + 6, frame.x + rowW - 14);
        auto head = theme::dim();
        surface.text({idsX, y}, "ID", head);
        surface.text({liveX, y}, "STATE", head);
        surface.text({agentX, y}, "AGENT", head);
        surface.text({turnsX, y}, "TURNS", head);
        surface.text({updatedX, y}, "UPDATED", head);
        ++y;
    }

    int visible = std::max(1, frame.bottom() - y - 1);  // reserve row for footer
    int start = std::max(0, std::min(dash.sessionIndex - visible / 2,
                                     static_cast<int>(dash.sessions.size()) - visible));

    for (int i = start; i < static_cast<int>(dash.sessions.size()) && y < frame.bottom(); ++i) {
        const auto& s = dash.sessions[static_cast<size_t>(i)];
        bool selected = i == dash.sessionIndex;
        bool active = !model_->activeSessionId.empty() &&
                      s.id == model_->activeSessionId;

        auto rowBg = selected ? theme::panel_3()
                      : active   ? theme::panel_2()
                                 : theme::panel_bg();
        surface.fill({frame.x, y, frame.w, 1}, " ", rowBg);
        if (selected)
            surface.text({frame.x, y}, "▌", theme::cyan().with_bg(rowBg.bg));

        // Primary label: title (first prompt / rename) else id suffix.
        std::string idTxt = suffix(s.id);
        std::string label = !s.title.empty() ? s.title : idTxt;
        int labelW = std::max(2, inkcell::text::display_width(label));
        auto idSt = active ? theme::bright() : theme::muted();
        idSt.bg = rowBg.bg;
        surface.text({frame.x + 1, y},
                     inkcell::text::truncate(label, std::max(2, frame.w - 3)),
                     active ? idSt.strong() : idSt);

        if (frame.w >= 64) {
            // STATE + resume quality badge
            auto state = active ? theme::green() : theme::dim();
            state.bg = rowBg.bg;
            state.bold = active;
            std::string badge = active ? "● LIVE" : (s.hasUiTimeline ? "◉ UI" : "◯");
            surface.text({frame.x + labelW + 2, y}, badge, state);

            // AGENT column
            std::string agentTxt = nonempty(s.agentName, "?");
            surface.text({frame.x + labelW + 10, y},
                         inkcell::text::truncate(agentTxt, std::max(8, frame.w - labelW - 32)),
                         (theme::text()).with_bg(rowBg.bg));

            // TURNS
            auto turnSt = theme::text().with_bg(rowBg.bg);
            turnSt.bold = (s.turnCount > 0);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%zut",
                          static_cast<size_t>(s.turnCount));
            surface.text({frame.x + frame.w - 22, y},
                         inkcell::text::truncate(buf, 6), turnSt);

            // UPDATED — age-style short suffix (last 4 of iso or -)
            std::string whenTxt = s.updated.empty()
                                      ? std::string("—")
                                      : s.updated.substr(std::max<size_t>(0, s.updated.size() - 16));
            whenTxt += "Z";
            auto updSt = theme::italic_dim();
            updSt.bg = rowBg.bg;
            surface.text({frame.x + frame.w - 14, y},
                         inkcell::text::truncate(whenTxt, 14), updSt);
        } else {
            std::string oneLine = active ? "● " : (s.hasUiTimeline ? "◉ " : "  ");
            oneLine += label + "  " + nonempty(s.agentName, "?") + "  " +
                       std::to_string(s.turnCount) + "r  ";
            if (!s.updated.empty()) oneLine += s.updated;
            auto st = ((selected ? theme::bright() : theme::text())).with_bg(rowBg.bg);
            if (active) st.bold = true;
            surface.text({frame.x + 1, y},
                         inkcell::text::truncate(oneLine, frame.w - 2), st);
        }
        ++y;
    }

    // Footer row: error context message + J/K hint
    if (y < frame.bottom()) {
        std::string hint =
            "j/k · ↵ resume · n new · f fork · t title · d del · e export · x kill";
        surface.text({frame.x, y}, inkcell::text::truncate(hint, frame.w),
                     theme::italic_dim());
    }
}

inline void MainScene::drawManifests(inkcell::Surface& surface, inkcell::Rect frame) const {
    const auto& dash = model_->dashboard;
    auto L = layout::manifestLayoutFor(frame.w);

    sectionHead(surface, frame, "Manifests",
                "↵ run/launch · 1-9 kind · f facet · z canvas · tab focus · / search");

    int y = frame.y + 4;

    // Expanded infinite canvas owns the whole stage.
    if (dash.wfCanvasExpanded) {
        const auto* sel = dash.selectedManifest();
        if (sel && sel->kind == "workflow") {
            drawWorkflowStage(surface, {frame.x, y, frame.w, frame.bottom() - y}, *sel,
                              gfx::nowSeconds());
            return;
        }
    }

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
            name += selected ? "  ↵ run" : "  ▷";
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

    // Card well — fill only for non-workflow; canvas wants field void.
    const auto* selPeek = dash.selectedManifest();
    const bool wfDetail = selPeek && selPeek->kind == "workflow";
    if (!wfDetail) surface.fill(det, " ", theme::panel_bg());

    auto paintManifestBody = [&](inkcell::Surface& s, inkcell::Rect inner, float alpha,
                                 const catalog::ManifestEntry& m) {
        // Workflows get infinite canvas + rail — not the generic field card.
        if (m.kind == "workflow" && alpha >= 0.55f) {
            drawWorkflowStage(s, inner, m, gfx::nowSeconds());
            return;
        }

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

        if (dy < inner.bottom())
            components::fieldLine(s, ix, dy++, iw, "kind", m.kind);
        if (dy < inner.bottom() && !m.version.empty())
            components::fieldLine(s, ix, dy++, iw, "version", m.version);
        if (dy < inner.bottom())
            components::fieldLine(s, ix, dy++, iw, "category", m.category);
        if (dy < inner.bottom() && (!m.provider.empty() || !m.model.empty()))
            components::fieldLine(s, ix, dy++, iw, "engine",
                                  nonempty(m.provider, "?") + "/" + nonempty(m.model, "?"));
        if (dy < inner.bottom())
            components::fieldLine(s, ix, dy++, iw, "source", nonempty(m.source, "—"));
        if (dy < inner.bottom())
            components::fieldLine(s, ix, dy++, iw, "rel",
                                  m.relPath.empty() ? "—" : m.relPath);
        if (dy < inner.bottom())
            components::fieldLine(s, ix, dy++, iw, "path",
                                  m.path.empty() ? "—" : m.path);
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
inline void MainScene::drawSettings(inkcell::Surface& surface, inkcell::Rect frame) const {
    // Title plate
    surface.text({frame.x, frame.y}, "SETTINGS", theme::bright());
    surface.text({frame.x + 10, frame.y}, "OPTIONS",
                 theme::italic_dim());

    int y = frame.y + 2;
    // Live preview strip — what the field actually is right now
    int previewH = std::min(5, std::max(3, frame.h / 6));
    if (y + previewH + 8 < frame.bottom()) {
        inkcell::Rect prev{frame.x, y, frame.w, previewH};
        if (gfx::fieldEnabled()) {
            gfx::drawFieldBg(surface, prev, gfx::themeVariantIndex(), gfx::nowSeconds());
        } else {
            surface.fill(prev, " ", theme::base_bg());
        }
        // Overlay label on preview
        std::string tag = gfx::fieldEnabled()
                              ? std::string("FIELD  ·  ") + gfx::activeFieldName()
                              : std::string("FIELD  ·  OFF");
        auto tagSt = theme::bright();
        tagSt.bg = inkcell::Color::rgb(0, 0, 0);
        surface.text({prev.x + 2, prev.y + previewH / 2},
                     inkcell::text::truncate(tag, prev.w - 4), tagSt);
        y = prev.bottom() + 1;
    }

    // Section labels
    auto section = [&](const char* name) {
        if (y >= frame.bottom()) return;
        surface.text({frame.x, y++}, name, theme::violet());
    };

    // Game-style option row:  [▸] LABEL ……… ◂ VALUE ▸   bind
    auto option = [&](int idx, const char* label, const std::string& value, const char* bind,
                      bool carousel) {
        if (y >= frame.bottom()) return;
        bool foc = (dashFocus() == idx);
        auto rowBg = foc ? theme::panel_3() : theme::panel_2();
        surface.fill({frame.x, y, frame.w, 1}, " ", rowBg);
        if (foc)
            surface.text({frame.x, y}, "▌",
                         theme::cyan().with_bg(rowBg.bg));

        auto labSt = (foc ? theme::bright() : theme::muted()).with_bg(rowBg.bg);
        surface.text({frame.x + 2, y}, inkcell::text::truncate(label, 16), labSt);

        // Value — centered carousel or toggle glyph
        std::string val = value;
        if (carousel) val = "◂  " + value + "  ▸";
        int vw = inkcell::text::display_width(val);
        int vx = frame.x + std::max(20, (frame.w - vw) / 2);
        auto valSt = (foc ? theme::bright() : theme::text()).with_bg(rowBg.bg);
        if (foc) valSt.bold = true;
        surface.text({vx, y}, inkcell::text::truncate(val, frame.w - 24), valSt);

        // Bind chip right
        auto bindSt = theme::italic_accent().with_bg(rowBg.bg);
        int bw = inkcell::text::display_width(bind);
        surface.text({frame.right() - bw - 1, y}, bind, bindSt);
        ++y;
    };

    section("DISPLAY");
    option(0, "THEME", upperCopy(theme::name()), "T / ←→", true);
    option(1, "FIELD", gfx::fieldEnabled() ? "ON" : "OFF", "B", false);
    option(2, "SHADER",
           gfx::fieldEnabled() ? upperCopy(gfx::activeFieldName()) : std::string("—"),
           "S / ←→", true);

    if (y < frame.bottom()) ++y;
    section("CHAT");
    option(3, "THOUGHTS", model_->showThoughts ? "ON" : "OFF", "^T", false);
    option(4, "TRUNCATE", model_->truncateBodies ? "ON" : "OFF", "^O", false);
    option(5, "RAW STREAM", model_->showRaw ? "ON" : "OFF", "^R", false);
    option(6, "CHAT FIELD",
           model_->chatFieldEnabled
               ? (gfx::fieldEnabled() ? std::string("ON  · ")
                                          + gfx::activeFieldName()
                                        : std::string("ON  ·  hub off"))
               : "OFF",
           "B / S", true);

    // Single footer — path only, no key encyclopedia
    if (y + 1 < frame.bottom()) {
        y = frame.bottom() - 1;
        surface.text({frame.x, y},
                     inkcell::text::truncate(
                         std::string("j/k select  ·  h/l or enter cycle  ·  ") + uiPrefsPath(),
                         frame.w),
                     theme::italic_dim());
    }
}

inline void MainScene::drawWorkflowStage(inkcell::Surface& surface, inkcell::Rect frame,
                       const catalog::ManifestEntry& m, float tsec) const {
    if (frame.w < 12 || frame.h < 6) return;

    auto& engine = workflows::WorkflowEngine::instance();
    auto& loaded = engine.load(m.path);
    if (!loaded.isValid()) {
        surface.text({frame.x, frame.y}, "failed to load workflow", theme::red());
        surface.text({frame.x, frame.y + 1},
                     inkcell::text::truncate(m.path, frame.w), theme::dim());
        return;
    }
    const auto& mf = loaded.manifest();
    auto graph = components::buildCanvasGraph(mf);

    auto run = model_->workflowRun.snapshot();
    const bool liveHere =
        (run.live || model::runStatusActive(run.status) ||
         run.status == model::RunStatus::Succeeded ||
         run.status == model::RunStatus::Failed ||
         run.status == model::RunStatus::Cancelled) &&
        (!run.path.empty() ? run.path == m.path : run.name == m.name);
    if (liveHere) components::applyRunStatusToGraph(graph, run);

    // Header strip (2 rows)
    std::string title = m.name;
    if (!m.version.empty()) title += "  v" + m.version;
    surface.text({frame.x, frame.y}, inkcell::text::truncate(title, frame.w), theme::bright());

    model::WorkflowTopology topo;
    model::countTopo(mf.steps, topo);
    std::string meta = components::topologyLine(topo);
    if (!m.summary.empty()) meta += "  ·  " + m.summary;
    if (liveHere) {
        meta = std::string(model::runStatusLabel(run.status)) + "  ·  " +
               components::formatRunElapsed(run.elapsedMs) + "  ·  " + meta;
    }
    surface.text({frame.x, frame.y + 1}, inkcell::text::truncate(meta, frame.w),
                 liveHere ? components::runStatusChipStyle(run.status) : theme::italic_dim());

    int bodyTop = frame.y + 3;
    int eventH = liveHere ? std::min(5, std::max(2, frame.h / 6)) : 0;
    int canvasH = std::max(4, frame.bottom() - bodyTop - eventH - (liveHere ? 0 : 1));
    inkcell::Rect canvas{frame.x, bodyTop, frame.w, canvasH};

    // Camera — settle path changes / center sentinel
    auto& dashMut = const_cast<model::DashboardState&>(model_->dashboard);
    components::CanvasCamera cam;
    cam.x = dashMut.wfCamX;
    cam.y = dashMut.wfCamY;
    bool needFrame = (dashMut.wfCanvasPath != m.path) || cam.x > 1e8f;
    if (needFrame) {
        if (!graph.nodes.empty() && dashMut.wfFocusNode >= 0 &&
            dashMut.wfFocusNode < static_cast<int>(graph.nodes.size()) && cam.x > 1e8f) {
            components::cameraCenterNode(
                cam, graph.nodes[static_cast<size_t>(dashMut.wfFocusNode)], canvas.w,
                canvas.h);
        } else {
            components::cameraFrameGraph(cam, graph, canvas.w, canvas.h);
        }
        dashMut.wfCamX = cam.x;
        dashMut.wfCamY = cam.y;
        dashMut.wfCanvasPath = m.path;
        if (dashMut.wfFocusNode >= static_cast<int>(graph.nodes.size()))
            dashMut.wfFocusNode = 0;
    }
    if (dashMut.wfFocusNode < 0) dashMut.wfFocusNode = 0;
    if (!graph.nodes.empty()) {
        int n = static_cast<int>(graph.nodes.size());
        dashMut.wfFocusNode = dashMut.wfFocusNode % n;
        if (dashMut.wfFocusNode < 0) dashMut.wfFocusNode += n;
    }

    components::CanvasDrawOpts opt;
    opt.selected = dashMut.wfFocusNode;
    opt.tSec = tsec;
    opt.showChrome = true;
    if (liveHere && run.currentIdx >= 0 &&
        run.currentIdx < static_cast<int>(run.steps.size()))
        opt.currentId = run.steps[static_cast<size_t>(run.currentIdx)].id;
    opt.statusLine = dashMut.wfCanvasFocus
                         ? "hjkl pan · [] node · . center · z expand · ↵ run · Esc stop"
                         : "tab canvas · z expand · ↵ run";
    if (liveHere && !run.lastError.empty() &&
        (run.status == model::RunStatus::Failed ||
         run.status == model::RunStatus::Cancelled))
        opt.statusLine = run.lastError;

    components::drawWorkflowCanvas(surface, canvas, graph, cam, opt);

    // Live event strip under canvas
    if (liveHere && eventH > 0) {
        inkcell::Rect strip{frame.x, canvas.bottom(), frame.w, frame.bottom() - canvas.bottom()};
        if (strip.h >= 2) {
            components::hairline(surface, strip.x, strip.y, strip.w, theme::dim());
            int ey = strip.y + 1;
            int n = static_cast<int>(run.events.size());
            int vis = std::max(1, strip.bottom() - ey);
            int start = n > vis ? n - vis : 0;
            for (int i = start; i < n && ey < strip.bottom(); ++i) {
                const auto& ev = run.events[static_cast<size_t>(i)];
                auto st = theme::dim();
                if (ev.kind.find("fail") != std::string::npos)
                    st = theme::red();
                else if (ev.kind == "step.ok" || ev.kind == "done")
                    st = theme::green_soft();
                else if (ev.kind == "step.enter" || ev.kind == "hitl")
                    st = theme::cyan();
                else if (ev.kind == "checkpoint" || ev.kind == "emit")
                    st = theme::amber_soft();
                std::string line = ev.kind;
                if (!ev.text.empty()) line += "  " + ev.text;
                surface.text({strip.x, ey++},
                             inkcell::text::truncate(line, strip.w), st);
            }
        }
    }
}


}  // namespace cortex::mk3::ui::scenes
