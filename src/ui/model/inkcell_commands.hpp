#pragma once
// Bridge: Cortex command inventory → inkcell CommandRegistry (F8 dogfood).
// One registry source for help overlay, future KeyHints, and App::commands().

#include <algorithm>
#include <string>
#include <vector>

#include "inkcell/command.hpp"
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
