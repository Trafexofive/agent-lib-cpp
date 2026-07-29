#pragma once
// Persist hub/chat UI prefs.
// Path: $XDG_CONFIG_HOME/cortex-mk3/ui.json  or  ~/.config/cortex-mk3/ui.json

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "src/ui/gfx/field_raster.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui {

// Chat-side prefs live on ShellModel — keep a shadow copy so load works
// before the model exists, then apply.
struct UiPrefState {
    bool showThoughts = true;
    bool truncateBodies = true;
    bool showRaw = false;
    // Chat-side field underlay. Inherits shared gfx::activeFieldIndex; this
    // is the on/off gate for the chat surface specifically.
    bool chatFieldEnabled = false;
    // Hub chrome: zen hides the floating nav pill until nav keys are used.
    bool zenMode = false;
    // Master switch — off removes the pill entirely (stage fills the bottom).
    bool navPillEnabled = true;
    // How long the pill stays visible after nav activity in zen mode (ms).
    // 0 = never auto-hide (always up while enabled).
    int navPillHideMs = 3000;
    // Default CWD applied on session create/resume. Empty = process startup CWD.
    // Inline edit via Settings · CWD (e to edit, ←→ to cycle HOME/process CWD).
    std::string sessionCwd;
    // When ON, the persisted sessionCwd is honored at app launch (the process
    // chdirs to it before the hub renders). When OFF (default), the launch
    // dir is used regardless of what was persisted — the CWD setting is
    // treated as a per-session hint, not a sticky cross-launch state.
    bool rememberLastCwd = false;
    // When ON, CWD change skips killing the live session. When OFF
    // (default), CWD change tears down the live session and its file.
    bool keepLiveOnCwdChange = false;
};

// Expand ~ to $HOME on the given path. Empty / already-absolute / no-~
// returned unchanged. Invalid $HOME → path returned unchanged.
inline std::string expandHome(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) return path;
    if (path.size() == 1) return std::string(home);
    if (path[1] == '/') return std::string(home) + path.substr(1);
    return path;
}

// Launch-time CWD policy. Reads shadow prefs (already loaded), normalizes the
// model's sessionCwd, and chdirs the process if rememberLastCwd is on.
// Called once after applyUiPrefsToModel in the boot sequence.
inline UiPrefState& uiPrefShadow();  // forward decl (defined below)
template <typename Model>
inline void applyLaunchCwd(Model& model) {
    const auto& s = uiPrefShadow();
    if (s.rememberLastCwd && !model.sessionCwd.empty()) {
        std::string target = expandHome(model.sessionCwd);
        if (!target.empty() && ::chdir(target.c_str()) == 0) {
            char buf[1024] = {0};
            if (::getcwd(buf, sizeof(buf) - 1)) {
                // Re-resolve in case chdir normalized a relative path.
                model.sessionCwd = buf;
            }
        }
    } else {
        // rememberLastCwd off: drop the persisted value so the Settings row
        // and Main/Sessions pages match the live process CWD (launch dir).
        model.sessionCwd.clear();
    }
}

inline UiPrefState& uiPrefShadow() {
    static UiPrefState s;
    return s;
}

inline std::string uiPrefsPath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0])
        return std::string(xdg) + "/cortex-mk3/ui.json";
    const char* home = std::getenv("HOME");
    if (home && home[0]) return std::string(home) + "/.config/cortex-mk3/ui.json";
    return "/tmp/cortex-mk3-ui.json";
}

inline void ensureParentDir(const std::string& path) {
    auto slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return;
    std::string dir = path.substr(0, slash);
    std::string acc;
    for (size_t i = 0; i < dir.size(); ++i) {
        char c = dir[i];
        acc.push_back(c);
        if (c == '/' && acc.size() > 1) ::mkdir(acc.c_str(), 0755);
    }
    if (!acc.empty() && acc.back() != '/') ::mkdir(acc.c_str(), 0755);
}

inline std::string jsonGetString(const std::string& body, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t k = body.find(pat);
    if (k == std::string::npos) return {};
    size_t colon = body.find(':', k + pat.size());
    if (colon == std::string::npos) return {};
    size_t q1 = body.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    size_t q2 = body.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return body.substr(q1 + 1, q2 - q1 - 1);
}

inline bool jsonGetBool(const std::string& body, const std::string& key, bool fallback) {
    std::string pat = "\"" + key + "\"";
    size_t k = body.find(pat);
    if (k == std::string::npos) return fallback;
    size_t colon = body.find(':', k + pat.size());
    if (colon == std::string::npos) return fallback;
    size_t t = body.find_first_not_of(" \t\r\n", colon + 1);
    if (t == std::string::npos) return fallback;
    if (body.compare(t, 4, "true") == 0) return true;
    if (body.compare(t, 5, "false") == 0) return false;
    return fallback;
}

inline int jsonGetInt(const std::string& body, const std::string& key, int fallback) {
    std::string pat = "\"" + key + "\"";
    size_t k = body.find(pat);
    if (k == std::string::npos) return fallback;
    size_t colon = body.find(':', k + pat.size());
    if (colon == std::string::npos) return fallback;
    size_t t = body.find_first_not_of(" \t\r\n", colon + 1);
    if (t == std::string::npos) return fallback;
    try {
        size_t end = 0;
        int v = std::stoi(body.substr(t), &end);
        (void)end;
        return v;
    } catch (...) {
        return fallback;
    }
}

inline void loadUiPrefs() {
    std::ifstream in(uiPrefsPath());
    if (!in) return;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string body = ss.str();
    if (body.empty()) return;

    std::string th = jsonGetString(body, "theme");
    if (th == "neon") theme::set(theme::Variant::Neon);
    else if (th == "graphite") theme::set(theme::Variant::Graphite);

    gfx::setFieldEnabled(jsonGetBool(body, "shader_enabled", true));
    std::string sh = jsonGetString(body, "shader");
    if (!sh.empty()) {
        const auto& all = gfx::fieldShaders();
        for (int i = 0; i < static_cast<int>(all.size()); ++i) {
            if (sh == all[static_cast<size_t>(i)].id) {
                gfx::setFieldIndex(i);
                break;
            }
        }
    }

    auto& shad = uiPrefShadow();
    shad.showThoughts = jsonGetBool(body, "show_thoughts", true);
    shad.truncateBodies = jsonGetBool(body, "truncate_bodies", true);
    shad.showRaw = jsonGetBool(body, "show_raw", false);
    shad.chatFieldEnabled = jsonGetBool(body, "chat_field_enabled", false);
    shad.zenMode = jsonGetBool(body, "zen_mode", false);
    shad.navPillEnabled = jsonGetBool(body, "nav_pill_enabled", true);
    shad.navPillHideMs = jsonGetInt(body, "nav_pill_hide_ms", 3000);
    // Clamp known carousel values.
    if (shad.navPillHideMs < 0) shad.navPillHideMs = 0;
    if (shad.navPillHideMs > 60000) shad.navPillHideMs = 60000;
    shad.sessionCwd = jsonGetString(body, "session_cwd");
    shad.rememberLastCwd = jsonGetBool(body, "remember_last_cwd", false);
    shad.keepLiveOnCwdChange = jsonGetBool(body, "keep_live_on_cwd_change", false);
}

// Apply shadow → live model (call after model construct / load).
template <typename Model>
inline void applyUiPrefsToModel(Model& model) {
    const auto& s = uiPrefShadow();
    model.showThoughts = s.showThoughts;
    model.truncateBodies = s.truncateBodies;
    model.showRaw = s.showRaw;
    model.chatFieldEnabled = s.chatFieldEnabled;
    model.zenMode = s.zenMode;
    model.navPillEnabled = s.navPillEnabled;
    model.navPillHideMs = s.navPillHideMs;
    model.sessionCwd = s.sessionCwd;
    model.rememberLastCwd = s.rememberLastCwd;
    model.keepLiveOnCwdChange = s.keepLiveOnCwdChange;
}

template <typename Model>
inline void captureUiPrefsFromModel(const Model& model) {
    auto& s = uiPrefShadow();
    s.showThoughts = model.showThoughts;
    s.truncateBodies = model.truncateBodies;
    s.showRaw = model.showRaw;
    s.chatFieldEnabled = model.chatFieldEnabled;
    s.zenMode = model.zenMode;
    s.navPillEnabled = model.navPillEnabled;
    s.navPillHideMs = model.navPillHideMs;
    s.sessionCwd = model.sessionCwd;
    s.rememberLastCwd = model.rememberLastCwd;
    s.keepLiveOnCwdChange = model.keepLiveOnCwdChange;
}

inline void saveUiPrefs() {
    std::string path = uiPrefsPath();
    ensureParentDir(path);
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    const auto& s = uiPrefShadow();
    out << "{\n"
        << "  \"theme\": \"" << theme::name() << "\",\n"
        << "  \"shader\": \"" << gfx::activeFieldId() << "\",\n"
        << "  \"shader_enabled\": " << (gfx::fieldEnabled() ? "true" : "false") << ",\n"
        << "  \"show_thoughts\": " << (s.showThoughts ? "true" : "false") << ",\n"
        << "  \"truncate_bodies\": " << (s.truncateBodies ? "true" : "false") << ",\n"
        << "  \"show_raw\": " << (s.showRaw ? "true" : "false") << ",\n"
        << "  \"chat_field_enabled\": " << (s.chatFieldEnabled ? "true" : "false") << ",\n"
        << "  \"zen_mode\": " << (s.zenMode ? "true" : "false") << ",\n"
        << "  \"nav_pill_enabled\": " << (s.navPillEnabled ? "true" : "false") << ",\n"
        << "  \"nav_pill_hide_ms\": " << s.navPillHideMs << ",\n"
        << "  \"session_cwd\": \"" << s.sessionCwd << "\",\n"
        << "  \"remember_last_cwd\": " << (s.rememberLastCwd ? "true" : "false") << ",\n"
        << "  \"keep_live_on_cwd_change\": " << (s.keepLiveOnCwdChange ? "true" : "false") << "\n"
        << "}\n";
}

inline void persistUiPrefs() { saveUiPrefs(); }

template <typename Model>
inline void persistUiPrefs(const Model& model) {
    captureUiPrefsFromModel(model);
    saveUiPrefs();
}

}  // namespace cortex::mk3::ui
