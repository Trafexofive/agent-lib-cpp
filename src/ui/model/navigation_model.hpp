#pragma once
// View/navigation stack model for the future inkcell app controller.

#include <string>
#include <vector>

#include "src/ui/model/app_context.hpp"
#include "src/ui/model/timeline_model.hpp"

namespace cortex::mk3::ui::model {

struct NavigationStackEntry {
    AppView view = AppView::AgentHistory;
    AgentPath agentPath;
    std::string selectedBlockId;
    int selectedIndex = 0;
    int scrollOffset = 0;
    FocusPane focus = FocusPane::History;
};

struct NavigationState {
    std::vector<NavigationStackEntry> stack;

    bool empty() const { return stack.empty(); }
    const NavigationStackEntry* current() const { return stack.empty() ? nullptr : &stack.back(); }
    NavigationStackEntry* current() { return stack.empty() ? nullptr : &stack.back(); }
};

inline NavigationStackEntry makeEntry(AppView view, FocusPane focus = FocusPane::History) {
    NavigationStackEntry e;
    e.view = view;
    e.focus = focus;
    return e;
}

inline void pushView(NavigationState& state, NavigationStackEntry entry) {
    state.stack.push_back(std::move(entry));
}

inline bool popView(NavigationState& state) {
    if (state.stack.size() <= 1) return false;
    state.stack.pop_back();
    return true;
}

inline void replaceRoot(NavigationState& state, NavigationStackEntry root) {
    state.stack.clear();
    state.stack.push_back(std::move(root));
}

inline bool pushAgentChild(NavigationState& state, const std::string& childName,
                           const std::string& selectedBlockId = {}) {
    if (childName.empty()) return false;
    NavigationStackEntry next;
    if (const auto* cur = state.current()) {
        next = *cur;
        next.agentPath = cur->agentPath.child(childName);
    } else {
        next.agentPath.parts.push_back(childName);
    }
    next.view = AppView::AgentHistory;
    next.focus = FocusPane::History;
    next.selectedBlockId = selectedBlockId;
    next.selectedIndex = 0;
    next.scrollOffset = 0;
    state.stack.push_back(std::move(next));
    return true;
}

inline bool atRootAgent(const NavigationState& state) {
    const auto* cur = state.current();
    return !cur || cur->agentPath.root();
}

inline std::string currentBreadcrumb(const NavigationState& state, const std::string& rootName = "root") {
    const auto* cur = state.current();
    if (!cur) return rootName;
    return cur->agentPath.label(rootName);
}

}  // namespace cortex::mk3::ui::model
