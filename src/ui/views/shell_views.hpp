#pragma once

#include <algorithm>
#include <string>

#include "inkcell/text.hpp"
#include "src/ui/layout/sbtui_layout.hpp"
#include "src/ui/model/inkcell_app_model.hpp"

namespace cortex::mk3::ui::views {

inline void topbar(inkcell::Surface& surface, const InkcellAppConfig& cfg, const ShellModel& model,
                   const std::string& page_name) {
    using namespace inkcell;
    Rect p = layout::page(surface);
    surface.text({p.x, p.y}, "CORTEX MK3", theme::cyan());
    surface.text({p.x, p.y + 1}, "sovereign protocol-native agent control plane", theme::dim());

    std::string mode = "mode: " + page_name;
    surface.text({std::max(p.x, p.right() - static_cast<int>(mode.size())), p.y}, mode, theme::bright());

    int chip_y = p.y + 3;
    layout::chip(surface, {p.x, chip_y}, model.running ? "● live" : model.done ? "✓ done" : "○ idle",
                 model.failed ? theme::red() : model.running ? theme::green() : theme::dim());
    int x = p.x + 11;
    std::string provider = nonempty(cfg.provider, "provider") + "/" + nonempty(cfg.model, "default");
    layout::chip(surface, {x, chip_y}, provider, theme::dim());
    x += static_cast<int>(provider.size()) + 4;
    layout::chip(surface, {x, chip_y}, cfg.ephemeral ? "ephemeral" : "session", theme::dim());
    layout::section_rule(surface, {p.x, p.y + 5}, p.w, "navigation");
}

inline void footer(inkcell::Surface& surface, const ShellModel& model, const std::string& hints) {
    using namespace inkcell;
    Rect p = layout::page(surface);
    std::string right = "pending " + std::to_string(model.pendingOps) + " · wakes " + std::to_string(model.wakeCount);
    surface.text({p.x, p.bottom() - 1}, text::truncate(hints, std::max(0, p.w - static_cast<int>(right.size()) - 2)),
                 theme::dim());
    surface.text({std::max(p.x, p.right() - static_cast<int>(right.size())), p.bottom() - 1}, right, theme::dim());
}

inline void nav(inkcell::Surface& surface, inkcell::Rect r, const std::string& current) {
    layout::flat_panel(surface, r, theme::panel_bg());
    struct Row {
        const char* key;
        const char* label;
        const char* page;
    } rows[] = {
        {"1", "Agent", "Agent"},
        {"2", "Dashboard", "Dashboard"},
        {"3", "Inspector", "Inspector"},
        {"?", "Help", "Help"},
    };
    int y = r.y + 1;
    for (const auto& row : rows) {
        bool sel = current == row.page;
        if (sel)
            layout::selected_row(surface, {r.x, y, r.w, 1}, std::string(row.key) + "  " + row.label, true);
        else
            surface.text({r.x + 1, y},
                         inkcell::text::truncate(std::string("  ") + row.key + "  " + row.label, r.w - 2),
                         theme::text());
        ++y;
    }
}

inline void state_block(inkcell::Surface& surface, inkcell::Rect r, PageState state, const std::string& noun,
                        const ShellModel& model) {
    layout::flat_panel(surface, r, theme::panel_2());
    if (state == PageState::Loading) {
        surface.text({r.x + 2, r.y + 1}, "Loading " + noun + "…", theme::amber());
        surface.text({r.x + 2, r.y + 3}, "Prior context remains visible when available.", theme::dim());
    } else if (state == PageState::Empty) {
        surface.text({r.x + 2, r.y + 1}, "Nothing here yet.", theme::dim());
        surface.text({r.x + 2, r.y + 3}, "Type a prompt and press Enter to send.", theme::text());
    } else if (state == PageState::Error) {
        surface.text({r.x + 2, r.y + 1}, "✗ Error", theme::red());
        surface.text({r.x + 2, r.y + 3}, inkcell::text::truncate(model.status, r.w - 4), theme::text());
    }
}

inline void metric(inkcell::Surface& surface, inkcell::Rect r, const std::string& label, const std::string& value,
                   inkcell::Style st) {
    layout::flat_panel(surface, r, theme::panel_2());
    surface.text({r.x + 1, r.y}, label, theme::dim());
    surface.text({r.x + 1, r.y + 2}, inkcell::text::truncate(value, r.w - 2), st);
}

}  // namespace cortex::mk3::ui::views
