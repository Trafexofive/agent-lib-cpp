#pragma once
// Persist hub/chat UI prefs.
// Path: $XDG_CONFIG_HOME/cortex-mk3/ui.json  or  ~/.config/cortex-mk3/ui.json

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "src/ui/chat/chat_footer.hpp"
#include "src/ui/gfx/field_raster.hpp"
#include "src/ui/model/timeline_codec.hpp"
#include "src/ui/theme/cortex_theme.hpp"

namespace cortex::mk3::ui {

// Chat-side prefs live on ShellModel — keep a shadow copy so load works
// before the model exists, then apply.
// Camera snapshot for one workflow manifest (see UiPrefState::wfCams).
struct WfCamPref {
    std::string path;
    int zoomX100 = 100;  // zoom * 100
    int x = 0;
    int y = 0;
    int focus = 0;
};

struct UiPrefState {
    bool showThoughts = true;
    // Default OFF — short cards opt-in (Settings · SHORT CARDS).
    bool truncateBodies = false;
    bool showRaw = false;
    // Action/result body paint: "json" | "yaml" | "raw"
    std::string inputBodyFmt = "json";
    std::string outputBodyFmt = "json";
    // 0=live 1=session 2=engine
    int chatFooterPane = 0;
    int chatBodyMode = 0;  // 0 stream 1 compact
    // Chat-side field underlay. Inherits shared gfx::activeFieldIndex; this
    // is the on/off gate for the chat surface specifically.
    bool chatFieldEnabled = false;
    // Stick transcript to live edge while the agent streams.
    bool autoFollowLive = true;
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
    // When OFF (default), the Sessions page / resume lists only sessions
    // minted under the current CWD (per-project view). When ON, all
    // sessions across projects are shown (recent-select + resume).
    bool globalSessions = false;
    // Operator dev mode (Settings toggle). Gates debug slash commands
    // (/export-dump, /dump-prompt, /prompts) and does not require agent.yml
    // runtime.dev_mode or CORTEX_DEV_MODE env.
    bool uiDevMode = false;
    // Per-workflow canvas camera memory (bounded, most-recent-first). Lets the
    // Workflow page reopen at the zoom/pan you left it at — no more resetting
    // to the overview every time you jump in and out.
    std::vector<WfCamPref> wfCams;
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
    // Heap-allocated on purpose: its destructor is never invoked at process
    // exit, so the atexit prefs flush (which reads this live, incl. the
    // wfCams vector) never touches already-destroyed members.
    static UiPrefState* s = new UiPrefState();
    return *s;
}

// Remember a workflow camera in the in-memory table (cheap; called per draw).
inline void wfCamRemember(const std::string& path, float zoom, float x, float y, int focus) {
    if (path.empty()) return;
    auto& v = uiPrefShadow().wfCams;
    auto it = std::find_if(v.begin(), v.end(),
                           [&](const WfCamPref& c) { return c.path == path; });
    WfCamPref c;
    c.path = path;
    c.zoomX100 = static_cast<int>(zoom * 100.f + 0.5f);
    c.x = static_cast<int>(x);
    c.y = static_cast<int>(y);
    c.focus = focus;
    if (it != v.end()) {
        *it = c;
        std::rotate(v.begin(), it, it + 1);  // most-recent-first
    } else {
        v.insert(v.begin(), c);
        if (v.size() > 16) v.resize(16);
    }
}

// Look up a remembered camera for a workflow path (nullptr if never visited).
inline const WfCamPref* wfCamFind(const std::string& path) {
    if (path.empty()) return nullptr;
    for (const auto& c : uiPrefShadow().wfCams)
        if (c.path == path) return &c;
    return nullptr;
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

inline void saveUiPrefs();  // fwd (defined below)
inline std::string& cachedUiThemeName();
inline std::string& cachedUiShaderId();
inline std::string serializeUiPrefs(const std::string&, const std::string&);
inline void writeUiPrefsFile(const std::string&);

// Flush UI prefs on process exit (normal return or SIGINT) so the workflow
// camera memory survives. Registered once per process, idemPOTENT.
inline void installUiPrefsFlush() {
    static std::once_flag once;
    std::call_once(once, []() {
        // At exit the gfx/anim statics may already be torn down, so build from
        // the cached names + the (heap, never-destroyed) prefs shadow only.
        std::atexit([]() {
            writeUiPrefsFile(serializeUiPrefs(cachedUiThemeName(), cachedUiShaderId()));
        });
    });
}

inline void loadUiPrefs() {
    installUiPrefsFlush();  // ensure prefs (incl. workflow cams) flush on exit
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
    shad.truncateBodies = jsonGetBool(body, "truncate_bodies", false);
    shad.showRaw = jsonGetBool(body, "show_raw", false);
    {
        std::string in = jsonGetString(body, "input_body_fmt");
        if (in.empty()) in = jsonGetString(body, "action_body_mode");
        if (!in.empty()) shad.inputBodyFmt = in;
        std::string out = jsonGetString(body, "output_body_fmt");
        if (!out.empty()) shad.outputBodyFmt = out;
        shad.chatFooterPane = jsonGetInt(body, "chat_footer_pane", 0);
        shad.chatBodyMode = jsonGetInt(body, "chat_body_mode", 0);
    }
    shad.chatFieldEnabled = jsonGetBool(body, "chat_field_enabled", false);
    shad.autoFollowLive = jsonGetBool(body, "auto_follow_live", true);
    shad.zenMode = jsonGetBool(body, "zen_mode", false);
    shad.navPillEnabled = jsonGetBool(body, "nav_pill_enabled", true);
    shad.navPillHideMs = jsonGetInt(body, "nav_pill_hide_ms", 3000);
    // Clamp known carousel values.
    if (shad.navPillHideMs < 0) shad.navPillHideMs = 0;
    if (shad.navPillHideMs > 60000) shad.navPillHideMs = 60000;
    shad.sessionCwd = jsonGetString(body, "session_cwd");
    shad.rememberLastCwd = jsonGetBool(body, "remember_last_cwd", false);
    shad.keepLiveOnCwdChange = jsonGetBool(body, "keep_live_on_cwd_change", false);
    shad.globalSessions = jsonGetBool(body, "global_sessions", false);
    shad.uiDevMode = jsonGetBool(body, "ui_dev_mode", false);
    // wf_cams: entries split by 0x1e, fields by 0x1f (control chars never
    // appear in manifest paths). path|zoomX100|x|y|focus.
    {
        shad.wfCams.clear();
        std::string raw = jsonGetString(body, "wf_cams");
        size_t e0 = 0;
        while (e0 <= raw.size()) {
            size_t e1 = raw.find('\x1e', e0);
            if (e1 == std::string::npos) e1 = raw.size();
            std::string entry = raw.substr(e0, e1 - e0);
            if (!entry.empty()) {
                size_t f0 = 0;
                std::vector<std::string> fields;
                while (f0 <= entry.size()) {
                    size_t f1 = entry.find('\x1f', f0);
                    if (f1 == std::string::npos) f1 = entry.size();
                    fields.push_back(entry.substr(f0, f1 - f0));
                    if (f1 == entry.size()) break;
                    f0 = f1 + 1;
                }
                if (fields.size() >= 5) {
                    try {
                        WfCamPref c;
                        c.path = fields[0];
                        c.zoomX100 = std::stoi(fields[1]);
                        c.x = std::stoi(fields[2]);
                        c.y = std::stoi(fields[3]);
                        c.focus = std::stoi(fields[4]);
                        if (shad.wfCams.size() < 16) shad.wfCams.push_back(c);
                    } catch (...) {}
                }
            }
            if (e1 == raw.size()) break;
            e0 = e1 + 1;
        }
    }
}

// Apply shadow → live model (call after model construct / load).
template <typename Model>
inline void applyUiPrefsToModel(Model& model) {
    const auto& s = uiPrefShadow();
    model.showThoughts = s.showThoughts;
    model.truncateBodies = s.truncateBodies;
    model.showRaw = s.showRaw;
    model.actionBodyMode = bodyRenderModeFromName(s.inputBodyFmt);
    model.resultBodyMode = bodyRenderModeFromName(s.outputBodyFmt);
    {
        int p = s.chatFooterPane;
        if (p < 0) p = 0;
        if (p > 2) p = 2;
        model.chatFooterPane = static_cast<chat::ChatFooterPane>(p);
    }
    {
        int b = s.chatBodyMode;
        if (b != 1) b = 0;  // 0 stream · 1 compact; retired canvas → stream
        model.chatBodyMode = b;
    }
    model.chatFieldEnabled = s.chatFieldEnabled;
    model.autoFollowLive = s.autoFollowLive;
    model.zenMode = s.zenMode;
    model.navPillEnabled = s.navPillEnabled;
    model.navPillHideMs = s.navPillHideMs;
    model.sessionCwd = s.sessionCwd;
    model.rememberLastCwd = s.rememberLastCwd;
    model.keepLiveOnCwdChange = s.keepLiveOnCwdChange;
    model.globalSessions = s.globalSessions;
    model.uiDevMode = s.uiDevMode;
}

template <typename Model>
inline void captureUiPrefsFromModel(const Model& model) {
    auto& s = uiPrefShadow();
    s.showThoughts = model.showThoughts;
    s.truncateBodies = model.truncateBodies;
    s.showRaw = model.showRaw;
    s.inputBodyFmt = bodyRenderModeName(model.actionBodyMode);
    s.outputBodyFmt = bodyRenderModeName(model.resultBodyMode);
    s.chatFooterPane = static_cast<int>(model.chatFooterPane);
    s.chatBodyMode = model.chatBodyMode;
    s.chatFieldEnabled = model.chatFieldEnabled;
    s.autoFollowLive = model.autoFollowLive;
    s.zenMode = model.zenMode;
    s.navPillEnabled = model.navPillEnabled;
    s.navPillHideMs = model.navPillHideMs;
    s.sessionCwd = model.sessionCwd;
    s.rememberLastCwd = model.rememberLastCwd;
    s.keepLiveOnCwdChange = model.keepLiveOnCwdChange;
    s.globalSessions = model.globalSessions;
    s.uiDevMode = model.uiDevMode;
}

// Cached copies of the name-y gfx values, snapshotted during normal saves so
// the atexit writer never touches a torn-down gfx/field static at process end.
inline std::string& cachedUiThemeName() { static std::string s; return s; }
inline std::string& cachedUiShaderId() { static std::string s; return s; }
inline void refreshCachedUiNames() {
    // Snapshot gfx/theme names while their statics are alive (normal saves),
    // so the atexit writer can rebuild prefs without touching them.
    cachedUiThemeName() = theme::name();
    cachedUiShaderId() = gfx::activeFieldId();
}

inline std::string serializeUiPrefs(const std::string& themeName,
                                    const std::string& shaderId) {
    const auto& s = uiPrefShadow();
    std::ostringstream oss;
    oss << "{\n"
        << "  \"ui_schema_version\": 1,\n"
        << "  \"theme\": \"" << themeName << "\",\n"
        << "  \"shader\": \"" << shaderId << "\",\n"
        << "  \"shader_enabled\": " << (gfx::fieldEnabled() ? "true" : "false") << ",\n"
        << "  \"show_thoughts\": " << (s.showThoughts ? "true" : "false") << ",\n"
        << "  \"truncate_bodies\": " << (s.truncateBodies ? "true" : "false") << ",\n"
        << "  \"show_raw\": " << (s.showRaw ? "true" : "false") << ",\n"
        << "  \"input_body_fmt\": \"" << s.inputBodyFmt << "\",\n"
        << "  \"output_body_fmt\": \"" << s.outputBodyFmt << "\",\n"
        << "  \"chat_footer_pane\": " << s.chatFooterPane << ",\n"
        << "  \"chat_body_mode\": " << s.chatBodyMode << ",\n"
        << "  \"chat_field_enabled\": " << (s.chatFieldEnabled ? "true" : "false") << ",\n"
        << "  \"auto_follow_live\": " << (s.autoFollowLive ? "true" : "false") << ",\n"
        << "  \"zen_mode\": " << (s.zenMode ? "true" : "false") << ",\n"
        << "  \"nav_pill_enabled\": " << (s.navPillEnabled ? "true" : "false") << ",\n"
        << "  \"nav_pill_hide_ms\": " << s.navPillHideMs << ",\n"
        << "  \"session_cwd\": \"" << s.sessionCwd << "\",\n"
        << "  \"remember_last_cwd\": " << (s.rememberLastCwd ? "true" : "false") << ",\n"
        << "  \"keep_live_on_cwd_change\": " << (s.keepLiveOnCwdChange ? "true" : "false") << ",\n"
        << "  \"global_sessions\": " << (s.globalSessions ? "true" : "false") << ",\n"
        << "  \"ui_dev_mode\": " << (s.uiDevMode ? "true" : "false") << ",\n"
        << "  \"wf_cams\": \"";
    for (size_t i = 0; i < s.wfCams.size(); ++i) {
        const auto& c = s.wfCams[i];
        if (i) oss << "\x1e";
        oss << c.path << "\x1f" << c.zoomX100 << "\x1f" << c.x << "\x1f" << c.y << "\x1f"
            << c.focus;
    }
    oss << "\"\n"
        << "}\n";
    return oss.str();
}

// Normal save: call gfx/theme (statics alive here), cache names, write.
inline void writeUiPrefsFile(const std::string& body) {
    std::string path = uiPrefsPath();
    ensureParentDir(path);
    // Atomic replace: write tmp beside target then rename — crash mid-write
    // must not leave a half JSON that loads as empty defaults forever.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return;
        out << body;
        out.flush();
        if (!out) {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // Cross-device rename fallback
        std::filesystem::copy_file(
            tmp, path,
            std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmp, ec);
    }
}

inline void saveUiPrefs() {
    refreshCachedUiNames();
    writeUiPrefsFile(serializeUiPrefs(theme::name(), gfx::activeFieldId()));
}

inline void persistUiPrefs() { saveUiPrefs(); }

template <typename Model>
inline void persistUiPrefs(const Model& model) {
    captureUiPrefsFromModel(model);
    saveUiPrefs();
}

}  // namespace cortex::mk3::ui
