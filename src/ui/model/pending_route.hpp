#pragma once
// Typed deferred scene/app route (F6). Replaces stringly pendingRoute.

#include <string>

namespace cortex::mk3::ui {

enum class PendingRoute {
    None = 0,
    Agent,  // open chat / agent scene
    Tool,   // open dedicated tool scene (full UX: input form + streaming output + history)
    Main,   // dashboard / hub
    Quit,   // app.quit
};

inline const char* pendingRouteAction(PendingRoute r) {
    switch (r) {
        case PendingRoute::Agent:
            return "scene.agent";
        case PendingRoute::Tool:
            return "scene.tool";
        case PendingRoute::Main:
            return "scene.main";
        case PendingRoute::Quit:
            return "app.quit";
        case PendingRoute::None:
            break;
    }
    return "";
}

inline bool pendingRoutePersistBefore(PendingRoute r) {
    return r == PendingRoute::Main || r == PendingRoute::Quit;
}

// Test/debug only.
inline const char* pendingRouteName(PendingRoute r) {
    switch (r) {
        case PendingRoute::Agent:
            return "agent";
        case PendingRoute::Tool:
            return "tool";
        case PendingRoute::Main:
            return "main";
        case PendingRoute::Quit:
            return "quit";
        case PendingRoute::None:
            return "";
    }
    return "";
}

inline PendingRoute pendingRouteFromName(const std::string& s) {
    if (s == "agent") return PendingRoute::Agent;
    if (s == "tool") return PendingRoute::Tool;
    if (s == "main") return PendingRoute::Main;
    if (s == "quit") return PendingRoute::Quit;
    return PendingRoute::None;
}

}  // namespace cortex::mk3::ui
