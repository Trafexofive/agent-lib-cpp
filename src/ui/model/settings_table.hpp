#pragma once
// Settings cabinet table — tetris-style grouped list.
// Headers are not focusable. settingsFocus is a ROW index into kSettingsRows.

#include <string>

namespace cortex::mk3::ui::model {

// Stable option ids (nudgeSetting switches on these).
enum class SettingsOpt : int {
    Theme = 0,
    Field,
    Shader,
    Thoughts,
    Truncate,
    InputFmt,
    OutputFmt,
    Raw,
    ChatField,
    FooterPane,   // default chat footer pane
    BodyView,     // stream · compact · canvas (Ctrl-O)
    AutoFollow,   // stick to live edge while streaming
    Zen,
    NavPill,
    PillHide,
    Cwd,
    RememberCwd,
    KeepLive,
    SessionScope,
    DevMode,
    Count
};

enum class SettingsRowKind { Head, Item };

struct SettingsRow {
    SettingsRowKind kind;
    SettingsOpt opt;      // ignored for Head
    const char* head;     // section title for Head; null for Item
    const char* label;    // row label for Item
};

// Single scrollable cabinet. Order is visual order.
inline constexpr SettingsRow kSettingsRows[] = {
    {SettingsRowKind::Head, SettingsOpt::Count, "LOOK", nullptr},
    {SettingsRowKind::Item, SettingsOpt::Theme, nullptr, "THEME"},
    {SettingsRowKind::Item, SettingsOpt::Field, nullptr, "HUB FIELD"},
    {SettingsRowKind::Item, SettingsOpt::Shader, nullptr, "FIELD SHADER"},

    {SettingsRowKind::Head, SettingsOpt::Count, "CHAT", nullptr},
    {SettingsRowKind::Item, SettingsOpt::Thoughts, nullptr, "THOUGHTS"},
    {SettingsRowKind::Item, SettingsOpt::Truncate, nullptr, "SHORT CARDS"},
    {SettingsRowKind::Item, SettingsOpt::InputFmt, nullptr, "ACTION BODY"},
    {SettingsRowKind::Item, SettingsOpt::OutputFmt, nullptr, "RESULT BODY"},
    {SettingsRowKind::Item, SettingsOpt::Raw, nullptr, "RAW STREAM"},
    {SettingsRowKind::Item, SettingsOpt::ChatField, nullptr, "CHAT FIELD BG"},
    {SettingsRowKind::Item, SettingsOpt::FooterPane, nullptr, "FOOTER PANE"},
    {SettingsRowKind::Item, SettingsOpt::BodyView, nullptr, "BODY VIEW"},
    {SettingsRowKind::Item, SettingsOpt::AutoFollow, nullptr, "LIVE FOLLOW"},

    {SettingsRowKind::Head, SettingsOpt::Count, "CHROME", nullptr},
    {SettingsRowKind::Item, SettingsOpt::Zen, nullptr, "ZEN MODE"},
    {SettingsRowKind::Item, SettingsOpt::NavPill, nullptr, "NAV PILL"},
    {SettingsRowKind::Item, SettingsOpt::PillHide, nullptr, "PILL AUTO-HIDE"},

    {SettingsRowKind::Head, SettingsOpt::Count, "SESSION", nullptr},
    {SettingsRowKind::Item, SettingsOpt::Cwd, nullptr, "CWD"},
    {SettingsRowKind::Item, SettingsOpt::RememberCwd, nullptr, "REMEMBER CWD"},
    {SettingsRowKind::Item, SettingsOpt::KeepLive, nullptr, "KEEP LIVE"},
    {SettingsRowKind::Item, SettingsOpt::SessionScope, nullptr, "SESSION SCOPE"},

    {SettingsRowKind::Head, SettingsOpt::Count, "DEV", nullptr},
    {SettingsRowKind::Item, SettingsOpt::DevMode, nullptr, "DEV MODE"},
};

inline constexpr int kSettingsRowN =
    static_cast<int>(sizeof(kSettingsRows) / sizeof(kSettingsRows[0]));

inline bool settingsRowFocusable(int i) {
    return i >= 0 && i < kSettingsRowN && kSettingsRows[i].kind == SettingsRowKind::Item;
}

inline int settingsFirstFocus() {
    for (int i = 0; i < kSettingsRowN; ++i)
        if (settingsRowFocusable(i)) return i;
    return 0;
}

// Step to next/prev focusable row (wraps).
inline int settingsStep(int cur, int dir) {
    int i = cur;
    for (int n = 0; n < kSettingsRowN; ++n) {
        i = (i + dir + kSettingsRowN) % kSettingsRowN;
        if (settingsRowFocusable(i)) return i;
    }
    return cur;
}

inline SettingsOpt settingsOptAt(int row) {
    if (!settingsRowFocusable(row)) return SettingsOpt::Theme;
    return kSettingsRows[row].opt;
}

// Whether value is a carousel (left/right) vs toggle (enter/left/right same).
inline bool settingsIsCarousel(SettingsOpt o) {
    switch (o) {
        case SettingsOpt::Theme:
        case SettingsOpt::Shader:
        case SettingsOpt::InputFmt:
        case SettingsOpt::OutputFmt:
        case SettingsOpt::PillHide:
        case SettingsOpt::Cwd:
        case SettingsOpt::SessionScope:
        case SettingsOpt::FooterPane:
        case SettingsOpt::BodyView:
            return true;
        default:
            return false;
    }
}

inline const char* settingsBindHint(SettingsOpt o) {
    if (o == SettingsOpt::Cwd) return "e edit";
    if (settingsIsCarousel(o)) return "← →";
    return "enter";
}

}  // namespace cortex::mk3::ui::model
