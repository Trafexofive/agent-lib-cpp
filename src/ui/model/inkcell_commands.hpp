#pragma once
// Bridge: Cortex command inventory → inkcell CommandRegistry (F8 dogfood).
// One registry source for help overlay, future KeyHints, and App::commands().

#include <algorithm>
#include <string>
#include <vector>

#include "inkcell/command.hpp"
#include "inkcell/widgets/key_hints.hpp"
#include "src/ui/components/cmd_palette.hpp"

namespace cortex::mk3::ui {

inline inkcell::Command toInkcellCommand(const components::CmdItem& item) {
    inkcell::Command c;
    c.id = item.id;
    c.title = item.label.empty() ? item.id : item.label;
    c.category = item.group.empty() ? "general" : item.group;
    c.description = item.hint;
    c.default_key = item.keys;
    c.visible = true;
    c.enabled = true;
    if (!item.group.empty()) c.tags.push_back(item.group);
    return c;
}

inline inkcell::CommandRegistry registryFromCmdItems(const std::vector<components::CmdItem>& items) {
    inkcell::CommandRegistry reg;
    for (const auto& item : items) reg.add(toInkcellCommand(item));
    return reg;
}

inline inkcell::CommandRegistry chatCommandRegistry() {
    return registryFromCmdItems(components::chatCommands());
}

inline inkcell::CommandRegistry hubCommandRegistry() {
    return registryFromCmdItems(components::hubCommands());
}

// Build inkcell KeyHints from registry entries that have a default_key.
// maxHints caps density for narrow footers. optional category filter (empty = all).
inline inkcell::widgets::KeyHints keyHintsFromRegistry(const inkcell::CommandRegistry& reg,
                                                       int maxHints = 8,
                                                       const std::string& category = {}) {
    inkcell::widgets::KeyHints hints;
    hints.separator(" · ");
    int n = 0;
    for (const auto& c : reg.all()) {
        if (!c.visible || !c.enabled) continue;
        if (c.default_key.empty()) continue;
        if (!category.empty() && c.category != category) continue;
        hints.hint(c.default_key, c.title);
        if (++n >= maxHints) break;
    }
    return hints;
}

// Prefer NAV/ACTION for hub chrome; falls back to full registry.
inline inkcell::widgets::KeyHints hubChromeKeyHints(int maxHints = 6) {
    auto reg = hubCommandRegistry();
    auto nav = keyHintsFromRegistry(reg, maxHints, "NAV");
    // If NAV alone is thin, mix ACTION.
    int count = 0;
    for (const auto& c : reg.all())
        if (c.category == "NAV" && c.visible && c.enabled && !c.default_key.empty()) ++count;
    if (count >= 3) return nav;
    return keyHintsFromRegistry(reg, maxHints);
}

// Categories in stable display order for help overlay.
inline std::vector<std::string> registryCategoryOrder(const inkcell::CommandRegistry& reg) {
    std::vector<std::string> order;
    auto push = [&](const std::string& cat) {
        if (std::find(order.begin(), order.end(), cat) == order.end()) order.push_back(cat);
    };
    // Preferred Cortex groups first
    for (const char* pref : {"NAV", "CHAT", "ACTION", "SLASH", "PROMPT", "SYSTEM", "general"})
        push(pref);
    for (const auto& c : reg.all()) push(c.category);
    // Drop empties that never appeared
    std::vector<std::string> out;
    for (const auto& cat : order) {
        if (!reg.by_category(cat).empty()) out.push_back(cat);
    }
    return out;
}

}  // namespace cortex::mk3::ui
