#pragma once
// Persist hub/chat UI prefs.
// Path: $XDG_CONFIG_HOME/cortex-mk3/ui.json  or  ~/.config/cortex-mk3/ui.json

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>

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
};

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
}

// Apply shadow → live model (call after model construct / load).
template <typename Model>
inline void applyUiPrefsToModel(Model& model) {
    const auto& s = uiPrefShadow();
    model.showThoughts = s.showThoughts;
    model.truncateBodies = s.truncateBodies;
    model.showRaw = s.showRaw;
    model.chatFieldEnabled = s.chatFieldEnabled;
}

template <typename Model>
inline void captureUiPrefsFromModel(const Model& model) {
    auto& s = uiPrefShadow();
    s.showThoughts = model.showThoughts;
    s.truncateBodies = model.truncateBodies;
    s.showRaw = model.showRaw;
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
        << "  \"chat_field_enabled\": " << (s.chatFieldEnabled ? "true" : "false") << "\n"
        << "}\n";
}

inline void persistUiPrefs() { saveUiPrefs(); }

template <typename Model>
inline void persistUiPrefs(const Model& model) {
    captureUiPrefsFromModel(model);
    saveUiPrefs();
}

}  // namespace cortex::mk3::ui
