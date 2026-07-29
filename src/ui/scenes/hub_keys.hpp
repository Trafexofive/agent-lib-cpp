#pragma once
// Hub keyboard routing — out-of-line MainScene::on_key.

#include <sys/stat.h>
#include <unistd.h>

#include "inkcell/key.hpp"

namespace cortex::mk3::ui::scenes {

inline bool MainScene::on_key(const inkcell::KeyEvent& event) {
    using inkcell::KeyCode;
    auto& dash = model_->dashboard;

    // Palette has priority when open / closing input sink
    if (model_->cmdPalette.open && !model_->cmdPalette.closing) {
        std::string action;
        if (components::handleCmdPaletteKey(model_->cmdPalette, event, &action)) {
            if (!action.empty()) runPaletteAction(action);
            return true;
        }
    }

    // Ctrl-P opens palette
    if (event.code == KeyCode::Character && event.ctrl() &&
        (event.ch == 'p' || event.ch == 'P')) {
        model_->cmdPalette.toggle(components::hubCommands());
        return true;
    }

    // ── Search mode ──────────────────────────────────────────────
    if (dash.searchMode) {
        if (event.code == KeyCode::Escape) {
            dash.searchMode = false;
            dash.searchQuery.clear();
            dash.refreshManifests();
            bumpNotice();
            return true;
        }
        if (event.code == KeyCode::Enter) {
            dash.searchMode = false;
            bumpNotice();
            return true;
        }
        if (event.code == KeyCode::Backspace) {
            if (!dash.searchQuery.empty()) dash.searchQuery.pop_back();
            dash.refreshManifests();
            bumpNotice();
            return true;
        }
        if (event.code == KeyCode::Character && !event.ctrl() && event.ch >= 32) {
            dash.searchQuery.push_back(static_cast<char>(event.ch));
            dash.refreshManifests();
            bumpNotice();
            return true;
        }
        return true;
    }

    // ── CWD inline edit mode (Settings · CWD · e) ─────────────────
    // Mirrors searchMode: char buffer, backspace, enter commits (~ expanded),
    // esc cancels. Only active when settings page is on the CWD row.
    if (dash.cwdEditMode) {
        if (event.code == KeyCode::Escape) {
            dash.cwdEditMode = false;
            dash.cwdEditBuffer.clear();
            bumpNotice();
            return true;
        }
        if (event.code == KeyCode::Enter) {
            dash.cwdEditMode = false;
            std::string raw = dash.cwdEditBuffer;
            dash.cwdEditBuffer.clear();
            // Empty buffer clears the setting (process CWD).
            if (raw.empty()) {
                model_->sessionCwd.clear();
                dash.flashNotice("cwd · cleared (process default)");
            } else {
                std::string resolved = expandHome(raw);
                // Validate before committing — operator can fix typos without
                // a surprise chdir to a non-existent path.
                struct stat st {};
                if (::stat(resolved.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                    model_->sessionCwd = resolved;
                    dash.flashNotice("cwd · " + resolved);
                } else {
                    dash.flashNotice("cwd · invalid: " + resolved);
                }
            }
            persistUiPrefs(*model_);
            return true;
        }
        if (event.code == KeyCode::Backspace) {
            if (!dash.cwdEditBuffer.empty()) dash.cwdEditBuffer.pop_back();
            return true;
        }
        if (event.code == KeyCode::Character && !event.ctrl() && event.ch >= 32) {
            dash.cwdEditBuffer.push_back(static_cast<char>(event.ch));
            return true;
        }
        return true;
    }

    // Tab toggles dock vs content focus
    if (event.code == KeyCode::Tab) {
        if (dash.section == model::DashboardSection::Manifests && workflowSelectionActive()) {
            dash.wfCanvasFocus = !dash.wfCanvasFocus;
            dash.focus = model::DashboardFocus::Content;
            dash.flashNotice(dash.wfCanvasFocus ? "canvas focus · hjkl pan · [] node"
                                                : "list focus");
            return true;
        }
        dash.toggleFocus();
        return true;
    }

    // ── ctrl-j = prev · ctrl-k = next ────────────────────────────
    if (event.ctrl() && event.code == KeyCode::Character) {
        if (event.ch == 'j' || event.ch == 'J') {
            dash.moveNavigation(-1);
            bumpNotice();
            return true;
        }
        if (event.ch == 'k' || event.ch == 'K') {
            dash.moveNavigation(1);
            bumpNotice();
            return true;
        }
    }

    // When dock focused, plain j/k also cycle (no chord needed)
    if (dash.focus == model::DashboardFocus::Dock && !event.ctrl() &&
        event.code == KeyCode::Character) {
        if (event.ch == 'j' || event.ch == 'J') {
            dash.moveNavigation(-1);
            bumpNotice();
            return true;
        }
        if (event.ch == 'k' || event.ch == 'K') {
            dash.moveNavigation(1);
            bumpNotice();
            return true;
        }
    }

    if (event.code == KeyCode::Escape) {
        if (model_->workflowRun.isLive() || model_->workflowRun.isActive()) {
            model_->pendingStopWorkflow = true;
            model_->workflowRun.requestCancel();
            dash.flashNotice("stopping workflow…");
            return true;
        }
        if (dash.wfCanvasExpanded) {
            dash.wfCanvasExpanded = false;
            dash.flashNotice("canvas docked");
            return true;
        }
        if (dash.wfCanvasFocus) {
            dash.wfCanvasFocus = false;
            dash.flashNotice("list focus");
            return true;
        }
        if (dash.focus == model::DashboardFocus::Dock) {
            dash.focus = model::DashboardFocus::Content;
            return true;
        }
        if (!dash.searchQuery.empty()) {
            dash.searchQuery.clear();
            dash.refreshManifests();
            dash.flashNotice("search cleared");
            return true;
        }
        if (!dash.tagFilter.empty()) {
            dash.tagFilter.clear();
            dash.refreshManifests();
            dash.flashNotice("tag cleared");
            return true;
        }
        if (!dash.manifestFilter.empty()) {
            dash.manifestFilter.clear();
            dash.refreshManifests();
            dash.flashNotice("kind cleared");
            return true;
        }
        return true;
    }

    // Content list nav / settings option nav
    if (dash.focus == model::DashboardFocus::Content && !event.ctrl()) {
        const bool up = event.code == KeyCode::ArrowUp ||
                        (event.code == KeyCode::Character && (event.ch == 'k' || event.ch == 'K'));
        const bool down = event.code == KeyCode::ArrowDown ||
                          (event.code == KeyCode::Character && (event.ch == 'j' || event.ch == 'J'));
        const bool left = event.code == KeyCode::ArrowLeft ||
                          (event.code == KeyCode::Character && (event.ch == 'h' || event.ch == 'H'));
        const bool right = event.code == KeyCode::ArrowRight ||
                           (event.code == KeyCode::Character && (event.ch == 'l' || event.ch == 'L'));

        // Infinite canvas owns hjkl when focused (workflow selected).
        if (dash.section == model::DashboardSection::Manifests && dash.wfCanvasFocus &&
            workflowSelectionActive()) {
            const float pan = event.shift() ? 8.f : 3.f;
            if (left) {
                dash.wfCamX -= pan;
                return true;
            }
            if (right) {
                dash.wfCamX += pan;
                return true;
            }
            if (up) {
                dash.wfCamY -= pan;
                return true;
            }
            if (down) {
                dash.wfCamY += pan;
                return true;
            }
            if (event.code == KeyCode::Character && (event.ch == '[' || event.ch == 'p')) {
                dash.wfFocusNode = std::max(0, dash.wfFocusNode - 1);
                return true;
            }
            if (event.code == KeyCode::Character && (event.ch == ']' || event.ch == 'n')) {
                ++dash.wfFocusNode;
                return true;
            }
        }

        if (dash.section == model::DashboardSection::Settings) {
            if (up) {
                dash.settingsFocus =
                    (dash.settingsFocus + model::DashboardState::settingsOptionCount - 1) %
                    model::DashboardState::settingsOptionCount;
                return true;
            }
            if (down) {
                dash.settingsFocus =
                    (dash.settingsFocus + 1) % model::DashboardState::settingsOptionCount;
                return true;
            }
            if (left) {
                nudgeSetting(-1);
                return true;
            }
            if (right || event.code == KeyCode::Enter) {
                nudgeSetting(+1);
                return true;
            }
        } else {
            if (up) {
                if (dash.section == model::DashboardSection::Sessions) dash.moveSession(-1);
                else if (dash.section == model::DashboardSection::Manifests)
                    dash.moveManifest(-1);
                return true;
            }
            if (down) {
                if (dash.section == model::DashboardSection::Sessions) dash.moveSession(1);
                else if (dash.section == model::DashboardSection::Manifests)
                    dash.moveManifest(1);
                return true;
            }
        }
    }

    if (event.code == KeyCode::Enter && dash.section != model::DashboardSection::Settings) {
        activate();
        return true;
    }

    if (event.code == KeyCode::Character && !event.ctrl()) {
        // Leader-leader: space space → palette (not in search)
        if (event.ch == ' ') {
            if (model_->cmdPalette.noteSpace()) {
                model_->cmdPalette.show(components::hubCommands());
                return true;
            }
            // single space swallowed on hub (no type-ahead surface)
            return true;
        }
        model_->cmdPalette.clearLeader();

        // Settings hot-numbers: 0 field off · 1-7 pick shader (no on-screen list)
        if (dash.section == model::DashboardSection::Settings && event.ch >= '0' &&
            event.ch <= '9') {
            int d = event.ch - '0';
            if (d == 0) {
                gfx::setFieldEnabled(false);
                dash.settingsFocus = 1;
            } else if (d >= 1 && d <= gfx::fieldCount()) {
                gfx::setFieldEnabled(true);
                gfx::setFieldIndex(d - 1);
                dash.settingsFocus = 2;
            }
            persistUiPrefs(*model_);
            return true;
        }
        // Kind digits on Manifests — operable facets
        if (dash.section == model::DashboardSection::Manifests && event.ch >= '0' &&
            event.ch <= '9') {
            if (dash.applyKindDigit(event.ch - '0')) {
                bumpNotice();
                return true;
            }
        }
        switch (event.ch) {
            case '/':
                if (dash.section == model::DashboardSection::Manifests) {
                    dash.searchMode = true;
                    dash.focus = model::DashboardFocus::Content;
                    dash.notice = "search: "; dash.noticeExpireAtMs = 0;
                    return true;
                }
                break;
            case 'c':
            case 'C':
                // Chat route always — canvas center is '.' (no clash).
                model_->requestRoute(PendingRoute::Agent);
                return true;
            case '.':
                if (dash.section == model::DashboardSection::Manifests &&
                    workflowSelectionActive()) {
                    dash.wfCamX = 1e9f;  // sentinel → reframe on next draw
                    dash.flashNotice("center");
                    return true;
                }
                break;
            case 'o':
            case 'O':
            case 'g':
            case 'G':
                dash.select(model::DashboardSection::Home);
                model_->launchError.clear();
                bumpNotice();
                return true;
            case 's':
            case 'S':
                // On Settings: S cycles shader. Elsewhere: Sessions jump.
                if (dash.section == model::DashboardSection::Settings) {
                    dash.settingsFocus = 2;
                    if (!gfx::fieldEnabled()) gfx::setFieldEnabled(true);
                    else gfx::cycleField(1);
                    persistUiPrefs(*model_);
                    return true;
                }
                dash.select(model::DashboardSection::Sessions);
                model_->launchError.clear();
                bumpNotice();
                return true;
            case 'a':
            case 'A':
            case 'm':
            case 'M':
                dash.select(model::DashboardSection::Manifests);
                dash.refreshManifests();
                model_->launchError.clear();
                bumpNotice();
                return true;
            case 'f':
            case 'F':
                if (dash.section == model::DashboardSection::Sessions) {
                    forkSelectedSession();
                    return true;
                }
                if (dash.section == model::DashboardSection::Manifests) {
                    if (event.ch == 'F' && workflowSelectionActive()) {
                        dash.wfCanvasExpanded = !dash.wfCanvasExpanded;
                        dash.wfCanvasFocus = dash.wfCanvasExpanded;
                        dash.flashNotice(dash.wfCanvasExpanded ? "canvas expanded"
                                                              : "canvas docked");
                        return true;
                    }
                    dash.cycleManifestFilter();
                    bumpNotice();
                }
                return true;
            case 't':
                if (dash.section == model::DashboardSection::Sessions) {
                    retitleSelectedSession();
                    return true;
                }
                if (dash.section == model::DashboardSection::Manifests) {
                    dash.cycleTagFilter();
                    bumpNotice();
                }
                return true;
            case '?':
                dash.select(model::DashboardSection::Settings);
                model_->launchError.clear();
                bumpNotice();
                return true;
            case 'b':
            case 'B':  // toggle field on/off (global)
                gfx::toggleFieldEnabled();
                if (dash.section == model::DashboardSection::Settings) dash.settingsFocus = 1;
                persistUiPrefs(*model_);
                return true;
            case 'n':
            case 'N':
                if (dash.section == model::DashboardSection::Sessions ||
                    dash.section == model::DashboardSection::Home)
                    createSession();
                return true;
            case 'd':
            case 'D':
                if (dash.section == model::DashboardSection::Sessions) deleteSelectedSession();
                return true;
            case 'e':
            case 'E':
                if (dash.section == model::DashboardSection::Settings &&
                    dash.settingsFocus == 10) {
                    dash.cwdEditMode = true;
                    dash.cwdEditBuffer = model_->sessionCwd;
                    bumpNotice();
                    return true;
                }
                if (dash.section == model::DashboardSection::Sessions)
                    exportSelectedSession();
                return true;
            case 'k':  // alias — lowercase k for quick kill on Sessions
            case 'K':
                if (dash.section == model::DashboardSection::Sessions) killLiveSession();
                return true;
            case 'y':
            case 'Y':
                if (!dash.yankBuffer.empty()) {
                    dash.flashNotice("yank: " + dash.yankBuffer);
                }
                return true;
            case 'r':
                if (dash.section == model::DashboardSection::Manifests) {
                    resumeLastWorkflow();
                    return true;
                }
                break;
            case 'x':
                if (dash.section == model::DashboardSection::Sessions) {
                    // 'x' shortcut — kill live session, mirroring
                    // Ctrl-X semantics. Doesn't conflict with workflow
                    // cancel because we're on the Sessions page.
                    killLiveSession();
                    return true;
                }
                if (model_->workflowRun.isLive() || model_->workflowRun.isActive()) {
                    model_->pendingStopWorkflow = true;
                    model_->workflowRun.requestCancel();
                    dash.flashNotice("stopping workflow…");
                    return true;
                }
                break;
            case 'X':
                if (dash.section == model::DashboardSection::Sessions) killLiveSession();
                return true;
                break;
            case 'z':
            case 'Z':
                // Manifests + workflow selection: Z expands canvas (legacy).
                // Elsewhere / Settings: Z toggles zen (pill auto-hide).
                if (dash.section == model::DashboardSection::Manifests &&
                    workflowSelectionActive()) {
                    dash.wfCanvasExpanded = !dash.wfCanvasExpanded;
                    dash.wfCanvasFocus = true;
                    dash.flashNotice(dash.wfCanvasExpanded ? "infinite canvas"
                                                           : "canvas docked");
                    return true;
                }
                model_->zenMode = !model_->zenMode;
                dash.bumpNavActivity();
                if (dash.section == model::DashboardSection::Settings)
                    dash.settingsFocus = 7;
                dash.flashNotice(model_->zenMode ? "zen on · pill auto-hides"
                                                 : "zen off · pill always up");
                persistUiPrefs(*model_);
                return true;
            case 'R':
                dash.refreshAll();
                dash.flashNotice("refreshed");
                return true;
            case 'T':
                theme::toggle();
                if (dash.section == model::DashboardSection::Settings) dash.settingsFocus = 0;
                persistUiPrefs(*model_);
                return true;
            case 'q':
            case 'Q':
                model_->requestRoute(PendingRoute::Quit);
                return true;
        }
    }
    return false;
}


}  // namespace cortex::mk3::ui::scenes
