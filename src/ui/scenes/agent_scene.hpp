#pragma once
// Agent / Chat scene.
// Product chat surface: inkcell-native port of ReplSession composition
// (transcript + status + prompt), not the old experimental card UI.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#include "base_scene.hpp"
#include "src/providers/factory.hpp"
#include "src/session/controller.hpp"
#include "src/session/manager.hpp"
#include "src/ui/chat/block_reader.hpp"
#include "src/ui/chat/chat_commands.hpp"
#include "src/ui/chat/chat_io.hpp"
#include "src/ui/chat/chat_view.hpp"
#include "src/ui/chat/composer_extras.hpp"
#include "src/ui/components/cmd_palette.hpp"
#include "src/ui/model/ui_prefs.hpp"

namespace cortex::mk3::ui::scenes {

class AgentScene final : public BaseScene {
   public:
    using BaseScene::BaseScene;
    std::string name() const override { return "Chat"; }

    void on_enter() override {
        BaseScene::on_enter();
        // One-shot route-in pulse on the status line (via notification stack).
        // Resume / fresh / launch all land here; keep it short, no second surface.
        chat::Notification n;
        n.id = "route-in";
        n.source = "chat";
        n.severity = "info";
        n.lifetimeMs = 2800;
        if (!model_->activeSessionId.empty()) {
            std::string sid = model_->activeSessionId;
            if (sid.size() > 8) sid = sid.substr(sid.size() - 8);
            n.title = "session · " + sid;
        } else if (!model_->agentName.empty()) {
            n.title = "agent · " + model_->agentName;
        } else {
            n.title = "chat ready";
        }
        if (model_->failed) {
            n.severity = "error";
            n.title = "last turn failed";
            n.lifetimeMs = 4500;
        } else if (model_->status == "cancelled") {
            n.severity = "warn";
            n.title = "last turn cancelled";
        }
        model_->notificationStack.push(std::move(n));
        // Idle with no sticky fail → ready label (failed/cancelled stay until next run).
        if (!model_->running && !model_->failed && model_->status != "cancelled" &&
            (model_->status.empty() || model_->status == "idle" || model_->status == "done"))
            model_->status = "ready";
    }

    void on_tick(inkcell::Tick t) override {
        BaseScene::on_tick(t);
        if (!model_->running) return;
        if (!model_->atRoot()) {
            model_->refreshNested();
            return;
        }
        if (model_->pendingOps <= 0) return;
        // Parent still has in-flight children — rebuild so the compact well
        // reflects live act/res without waiting for a parent protocol event.
        using clock = std::chrono::steady_clock;
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                clock::now().time_since_epoch())
                                .count();
        if (now - model_->lastNestedRefreshMs < 200) return;
        model_->lastNestedRefreshMs = now;
        model_->markProjFull();
        model_->rebuildViews();
        if (t.clock) t.clock->mark();
    }

    bool on_key(const inkcell::KeyEvent& event) override {
        using inkcell::KeyCode;

        if (model_->askActive) return handleAskKey(event);

        // Command palette (ctrl-p / space×2 when not typing)
        if (model_->cmdPalette.open && !model_->cmdPalette.closing) {
            std::string action;
            if (components::handleCmdPaletteKey(model_->cmdPalette, event, &action)) {
                if (!model_->cmdPalette.open || model_->cmdPalette.closing)
                    model_->closeModalFocus("palette");
                if (!action.empty()) runPaletteAction(action);
                return true;
            }
        }
        if (event.code == KeyCode::Character && event.ctrl() &&
            (event.ch == 'p' || event.ch == 'P')) {
            model_->cmdPalette.toggle(components::chatCommands());
            if (model_->cmdPalette.open && !model_->cmdPalette.closing)
                model_->openModalFocus("palette");
            else
                model_->closeModalFocus("palette");
            return true;
        }

        // Help-overlay own Esc/?. Always closes first, regardless of focus rung.
        if (model_->helpVisible &&
            (event.code == KeyCode::Escape ||
             (event.code == KeyCode::Character && event.ch == '?'))) {
            model_->helpVisible = false;
            model_->closeModalFocus("help");
            return true;
        }

        // Esc ladder (pi-like): never quit, never route to dashboard, never
        // drill-back. Esc only dismisses overlays / toggles focus rungs.
        // Navigation: `m` → main, Backspace/h → goBack (when not typing).
        if (event.code == KeyCode::Escape) {
            if (model_->blockReader.open) {
                if (model_->blockReader.visual != chat::ReaderVisual::None) {
                    model_->blockReader.visual = chat::ReaderVisual::None;
                    return true;
                }
                chat::closeBlockReader(model_->blockReader);
                return true;
            }
            // Only error/warn/sticky alerts consume Esc. Info pulses (route-in,
            // "no active turn") auto-expire and must not steal focus rungs.
            if (const auto* top = model_->notificationStack.top()) {
                if (top->severity == "error" || top->severity == "warn" ||
                    top->lifetimeMs <= 0) {
                    model_->notificationStack.dismissTop();
                    return true;
                }
            }
            if (model_->cmdPalette.open && !model_->cmdPalette.closing) {
                model_->cmdPalette.requestClose();
                model_->closeModalFocus("palette");
                return true;
            }
            if (model_->composer.focused) {
                // Composer owned focus — drop to timeline without stomp-scrolling.
                model_->focusTimeline();
                return true;
            }
            if (model_->timelineFocus) {
                // Return focus to composer; stay on this scene/agent path.
                model_->focusComposer();
                return true;
            }
            // Idle / no rung — still consume so Esc never becomes quit.
            return true;
        }

        // Block reader owns keys while open (cursor / visual / yank / close).
        if (model_->blockReader.open) {
            auto& br = model_->blockReader;
            const int viewH = std::max(4, model_->transcriptView.viewport_h);
            // Search input mode: type query, Enter run, Esc cancel.
            if (br.searchMode) {
                if (event.code == KeyCode::Escape) {
                    br.searchMode = false;
                    br.searchQuery.clear();
                    return true;
                }
                if (event.code == KeyCode::Enter) {
                    br.lastSearch = br.searchQuery;
                    br.searchMode = false;
                    if (!chat::readerSearchStep(br, +1, viewH) && !br.lastSearch.empty()) {
                        chat::Notification n;
                        n.id = "reader-search";
                        n.source = "reader";
                        n.severity = "warn";
                        n.lifetimeMs = 1800;
                        n.title = "no match: " + br.lastSearch;
                        model_->notificationStack.push(std::move(n));
                    }
                    return true;
                }
                if (event.code == KeyCode::Backspace) {
                    if (!br.searchQuery.empty()) br.searchQuery.pop_back();
                    return true;
                }
                if (event.code == KeyCode::Character && !event.ctrl() && event.ch >= 32 &&
                    event.ch < 127) {
                    br.searchQuery.push_back(static_cast<char>(event.ch));
                    return true;
                }
                return true;
            }
            // Esc: leave visual first, then close.
            if (event.code == KeyCode::Escape) {
                if (br.visual != chat::ReaderVisual::None) {
                    br.visual = chat::ReaderVisual::None;
                    return true;
                }
                chat::closeBlockReader(br);
                return true;
            }
            if (event.code == KeyCode::Backspace) {
                chat::closeBlockReader(br);
                return true;
            }
            if (event.code == KeyCode::Character && event.ch == '/' && !event.ctrl()) {
                br.searchMode = true;
                br.searchQuery.clear();
                br.visual = chat::ReaderVisual::None;
                return true;
            }
            if (event.code == KeyCode::Character && event.ch == 'n' && !event.ctrl() &&
                !event.shift()) {
                if (!chat::readerSearchStep(br, +1, viewH)) {
                    chat::Notification n;
                    n.id = "reader-search";
                    n.source = "reader";
                    n.severity = "warn";
                    n.lifetimeMs = 1500;
                    n.title = br.lastSearch.empty() ? "no search — press /" : "no more matches";
                    model_->notificationStack.push(std::move(n));
                }
                return true;
            }
            if (event.code == KeyCode::Character &&
                (event.ch == 'N' || (event.ch == 'n' && event.shift())) && !event.ctrl()) {
                if (!chat::readerSearchStep(br, -1, viewH)) {
                    chat::Notification n;
                    n.id = "reader-search";
                    n.source = "reader";
                    n.severity = "warn";
                    n.lifetimeMs = 1500;
                    n.title = br.lastSearch.empty() ? "no search — press /" : "no more matches";
                    model_->notificationStack.push(std::move(n));
                }
                return true;
            }
            // Visual mode toggles
            if (event.code == KeyCode::Character && event.ch == 'v' && !event.ctrl() &&
                !event.shift()) {
                if (br.visual == chat::ReaderVisual::Char) {
                    br.visual = chat::ReaderVisual::None;
                } else {
                    br.visual = chat::ReaderVisual::Char;
                    br.selAnchor = br.cursor;
                    br.selAnchorCol = br.cursorCol;
                }
                return true;
            }
            if (event.code == KeyCode::Character &&
                (event.ch == 'V' || (event.ch == 'v' && event.shift())) && !event.ctrl()) {
                if (br.visual == chat::ReaderVisual::Line) {
                    br.visual = chat::ReaderVisual::None;
                } else {
                    br.visual = chat::ReaderVisual::Line;
                    br.selAnchor = br.cursor;
                    br.selAnchorCol = 0;
                }
                return true;
            }
            if (event.code == KeyCode::ArrowUp ||
                (event.code == KeyCode::Character && (event.ch == 'k' || event.ch == 'K') &&
                 !event.ctrl())) {
                chat::moveReaderCursor(br, -1, 0, viewH);
                return true;
            }
            if (event.code == KeyCode::ArrowDown ||
                (event.code == KeyCode::Character && (event.ch == 'j' || event.ch == 'J') &&
                 !event.ctrl())) {
                chat::moveReaderCursor(br, 1, 0, viewH);
                return true;
            }
            if (event.code == KeyCode::ArrowLeft ||
                (event.code == KeyCode::Character && event.ch == 'h' && !event.ctrl() &&
                 !event.shift())) {
                chat::moveReaderCursor(br, 0, -1, viewH);
                return true;
            }
            if (event.code == KeyCode::ArrowRight ||
                (event.code == KeyCode::Character && event.ch == 'l' && !event.ctrl())) {
                chat::moveReaderCursor(br, 0, 1, viewH);
                return true;
            }
            if (event.code == KeyCode::Character && event.ch == 'w' && !event.ctrl()) {
                chat::readerWordForward(br, viewH);
                return true;
            }
            if (event.code == KeyCode::Character && event.ch == 'b' && !event.ctrl()) {
                chat::readerWordBack(br, viewH);
                return true;
            }
            if (event.code == KeyCode::Character && event.ch == 'e' && !event.ctrl()) {
                chat::readerWordEnd(br, viewH);
                return true;
            }
            if (event.code == KeyCode::PageUp ||
                (event.code == KeyCode::Character && event.ch == 'u' && !event.ctrl())) {
                chat::moveReaderCursor(br, -std::max(1, viewH / 2), 0, viewH);
                return true;
            }
            if (event.code == KeyCode::PageDown ||
                (event.code == KeyCode::Character && event.ch == 'd' && !event.ctrl())) {
                chat::moveReaderCursor(br, std::max(1, viewH / 2), 0, viewH);
                return true;
            }
            if (event.code == KeyCode::Home ||
                (event.code == KeyCode::Character && event.ch == 'g' && !event.shift() &&
                 !event.ctrl())) {
                br.cursor = 0;
                br.cursorCol = 0;
                chat::readerEnsureCursorVisible(br, viewH);
                return true;
            }
            if (event.code == KeyCode::End ||
                (event.code == KeyCode::Character && event.ch == 'G' && !event.ctrl())) {
                br.cursor = std::max(0, static_cast<int>(br.lines.size()) - 1);
                br.cursorCol = 0;
                chat::readerEnsureCursorVisible(br, viewH);
                return true;
            }
            if (event.code == KeyCode::Character && (event.ch == 'y' || event.ch == 'Y') &&
                !event.ctrl()) {
                std::string yank = chat::readerYankText(br);
                // yy on no visual with count? single y yanks selection or line.
                // Double-y (yy) yanks whole body when not in visual — use Y for whole.
                if (event.ch == 'Y' && br.visual == chat::ReaderVisual::None)
                    yank = br.body;
                auto copied = chat::copyText(yank, "/tmp/mk3-yank.txt");
                chat::Notification n;
                n.id = "yank";
                n.source = "reader";
                n.lifetimeMs = 2200;
                n.severity = copied.copied ? "info" : "warn";
                n.title = copied.copied
                              ? (std::string("yanked ") + std::to_string(yank.size()) + "B")
                              : ("yank → " + copied.destination);
                model_->notificationStack.push(std::move(n));
                br.visual = chat::ReaderVisual::None;
                return true;
            }
            return true;  // swallow other keys while reader is up
        }

        // Vet-fix: Backspace is NEVER navigation while composer is focused
        // (`i` / interactive type mode). Operator hard rule after repeated
        // UX breakage: in type mode Backspace deletes glyphs only (handled
        // later by composer.handle_key). Navigation Backspace only when
        // composer is unfocused (timeline / selection mode).
        // Drilldown-aware when navigating: pop one level, else main.
        if (event.code == KeyCode::Backspace && !model_->askActive &&
            !model_->helpVisible && !model_->cmdPalette.open &&
            !model_->composer.focused) {
            if (!model_->atRoot()) {
                model_->goBack();
            } else {
                model_->requestRoute(PendingRoute::Main);
            }
            return true;
        }

        if (model_->helpVisible) {
            if (event.code == KeyCode::Escape ||
                (event.code == KeyCode::Character && event.ch == '?')) {
                model_->helpVisible = false;
                model_->closeModalFocus("help");
            }
            return true;
        }

        // Pi UX: 1st Ctrl-C cancels in-flight turn / ask; 2nd (when idle) exits.
        // Engine no longer hard-quits before the scene sees Ctrl-C.
        if (event.code == KeyCode::CtrlC) {
            if (model_->running || model_->askActive) {
                stopAgentLoop("ctrl-c");
            } else {
                model_->requestRoute(PendingRoute::Quit);
            }
            return true;  // always consume — never fall through to engine hard-quit
        }
        // Ctrl-X always means stop the loop (never quit) — explicit kill switch.
        if (event.code == KeyCode::Character && event.ctrl() &&
            (event.ch == 'x' || event.ch == 'X')) {
            stopAgentLoop("ctrl-x");
            return true;
        }
        // Global view toggles / transcript scroll that must work while typing.
        // (plain t/r only fire via keymap when the composer is unfocused).
        //
        // Fixed complementary bind (no mode toggle):
        //   j/k       → block select (history focus only)
        //   Ctrl-J/K  → fine transcript scroll ±1 (history + composer)
        //   ↑/↓       → fine transcript scroll ±1 (history focus)
        if (event.code == KeyCode::Character && event.ctrl()) {
            if (event.ch == 't' || event.ch == 'T') {
                model_->showThoughts = !model_->showThoughts;
                model_->markProjFull();
                model_->forceFullProject_ = true;
                model_->transcriptWrapCache.invalidate();
                ++model_->transcriptVersion;
                model_->rebuildViews();
                persistUiPrefs(*model_);
                return true;
            }
            // Ctrl-O: cycle stream → compact.
            // Ctrl-Shift-O: truncate bodies.
            if (event.ch == 'o' || event.ch == 'O') {
                if (event.shift()) {
                    model_->toggleTruncateBodies();
                    model_->dashboard.flashNotice(
                        model_->truncateBodies ? "bodies · truncated"
                                               : "bodies · full");
                } else {
                    model_->chatBodyMode = (model_->chatBodyMode + 1) % 2;
                    static const char* kNames[] = {"stream", "compact"};
                    model_->dashboard.flashNotice(
                        std::string("view · ") +
                        kNames[model_->chatBodyMode % 2] + "  (ctrl-o)");
                }
                persistUiPrefs(*model_);
                return true;
            }
            // Cycle chat footer pane (live / session / engine) under the prompt.
            if (event.ch == 'f' || event.ch == 'F') {
                model_->chatFooterPane = chat::nextFooterPane(model_->chatFooterPane, +1);
                persistUiPrefs(*model_);
                return true;
            }
            if (event.ch == 'r' || event.ch == 'R') {
                model_->showRaw = !model_->showRaw;
                model_->markProjFull();
                model_->rebuildViews();
                persistUiPrefs(*model_);
                return true;
            }
            // Fine scroll when the composer is unfocused. While typing, Ctrl-K/U/W
            // belong to TextArea (kill-line / kill-to-bol / kill-word).
            if (event.ch == 'j' || event.ch == 'J') {
                model_->transcriptView.scroll_by(1);
                return true;
            }
            if ((event.ch == 'k' || event.ch == 'K') &&
                (!model_->composer.focused || model_->timelineFocus)) {
                model_->transcriptView.scroll_by(-1);
                return true;
            }
        }

        if (model_->timelineFocus || !model_->composer.focused) {
            // Leader-leader only when composer doesn't own space
            if (event.code == KeyCode::Character && !event.ctrl() && event.ch == ' ') {
                if (model_->cmdPalette.noteSpace()) {
                    model_->cmdPalette.show(components::chatCommands());
                    model_->openModalFocus("palette");
                    return true;
                }
                return true;
            }
            if (event.code == KeyCode::Character && !event.ctrl())
                model_->cmdPalette.clearLeader();

            if (event.code == KeyCode::Character && (event.ch == 'm' || event.ch == 'M')) {
                model_->requestRoute(PendingRoute::Main);
                return true;
            }
            if (event.code == KeyCode::Character && event.ch == '?') {
                model_->helpVisible = true;
                model_->openModalFocus("help");
                return true;
            }
            if (event.code == KeyCode::Character && event.ch == 'T') {
                theme::toggle();
                return true;
            }
            // Escape handled in the top ladder (no goBack / no main route).
            // Complementary binds — no sticky mode, no Tab toggle:
            //   j/k      block select (+ viewport follows selection)
            //   Ctrl-J/K fine scroll (global ctrl block; also while typing)
            //   ↑/↓      fine scroll
            //   PgUp/Dn  half-page · Home/End ends
            if (event.code == KeyCode::ArrowUp) {
                model_->transcriptView.scroll_by(-1);  // unlocks stick unless at end
                return true;
            }
            if (event.code == KeyCode::ArrowDown) {
                model_->transcriptView.scroll_by(1);
                return true;
            }
            if (event.code == KeyCode::Character && (event.ch == 'k' || event.ch == 'K')) {
                model_->selectDelta(-1);
                return true;
            }
            if (event.code == KeyCode::Character && (event.ch == 'j' || event.ch == 'J')) {
                model_->selectDelta(1);
                return true;
            }
            if (event.code == KeyCode::Character && event.ch == 'z' && !event.ctrl()) {
                model_->toggleSelectedCollapsed();
                return true;
            }
            // gg = top block, G = bottom block (vim-ish; history/nav mode only)
            if (event.code == KeyCode::Character && event.ch == 'g' && !event.shift() &&
                !event.ctrl()) {
                // double-g: simple sticky — second g within same focus does edge
                static int64_t lastGms = 0;
                using clock = std::chrono::steady_clock;
                const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        clock::now().time_since_epoch())
                                        .count();
                if (now - lastGms < 450) {
                    model_->selectEdge(false);  // gg → top
                    lastGms = 0;
                } else {
                    lastGms = now;
                    // single g still refreshes nested (legacy)
                    model_->refreshNested();
                }
                return true;
            }
            if (event.code == KeyCode::Character && (event.ch == 'G') && !event.ctrl()) {
                model_->selectEdge(true);  // G → bottom
                return true;
            }
            // Shift-[ / Shift-] → skip 4 blocks (smooth via ensureSelectionVisible)
            if (event.code == KeyCode::Character && event.shift() && !event.ctrl() &&
                (event.ch == '{' || event.ch == '[')) {
                model_->selectDelta(-4);
                return true;
            }
            if (event.code == KeyCode::Character && event.shift() && !event.ctrl() &&
                (event.ch == '}' || event.ch == ']')) {
                model_->selectDelta(4);
                return true;
            }
            // y → yank selected block body (clipboard / fallback file)
            if (event.code == KeyCode::Character && (event.ch == 'y' || event.ch == 'Y') &&
                !event.ctrl()) {
                std::string body = model_->yankSelectedBody();
                chat::Notification n;
                n.id = "yank";
                n.source = "yank";
                n.lifetimeMs = 2500;
                if (body.empty()) {
                    n.severity = "info";
                    n.title = "nothing to yank";
                } else {
                    auto copied = chat::copyText(body, "/tmp/mk3-yank.txt");
                    n.severity = copied.copied ? "info" : "warn";
                    n.title = copied.copied
                                  ? ("yanked " + std::to_string(body.size()) + "B")
                                  : ("yank → " + copied.destination);
                }
                model_->notificationStack.push(std::move(n));
                return true;
            }
            if (event.code == KeyCode::PageUp) {
                model_->transcriptView.scroll_by(-std::max(1, model_->transcriptView.viewport_h / 2));
                return true;
            }
            if (event.code == KeyCode::PageDown) {
                model_->transcriptView.scroll_by(std::max(1, model_->transcriptView.viewport_h / 2));
                return true;
            }
            if (event.code == KeyCode::Home) {
                model_->transcriptView.scroll_to_start();
                return true;
            }
            if (event.code == KeyCode::End) {
                model_->transcriptView.scroll_to_end();
                return true;
            }
            if (event.code == KeyCode::Enter) {
                model_->enterSelected();
                return true;
            }
            if (event.code == KeyCode::Backspace ||
                (event.code == KeyCode::Character && (event.ch == 'h' || event.ch == 'H'))) {
                if (model_->goBack()) return true;
            }
            if (event.code == KeyCode::Character && event.ch == 'i') {
                model_->focusComposer();
                return true;
            }
            if (event.code == KeyCode::Character && event.ch == 'g') {
                model_->refreshNested();
                return true;
            }
            if (event.code == KeyCode::Character && event.ch == 'u') {
                model_->transcriptView.scroll_by(-std::max(1, model_->transcriptView.viewport_h / 2));
                return true;
            }
            if (event.code == KeyCode::Character && event.ch == 'd') {
                model_->transcriptView.scroll_by(std::max(1, model_->transcriptView.viewport_h / 2));
                return true;
            }
            return false;
        }

        if (event.code == KeyCode::Escape) {
            model_->focusTimeline();
            return true;
        }
        if (event.code == KeyCode::Tab) {
            if (chat::completeAtPath(*model_, false)) return true;
            completeSlashCommand(/*reverse=*/false);
            return true;
        }
        if (event.code == KeyCode::BackTab) {
            if (chat::completeAtPath(*model_, true)) return true;
            completeSlashCommand(/*reverse=*/true);
            return true;
        }
        // Multi-line prompt: Up/Down navigate logical lines inside the composer
        // when there is more than one line; only edge lines fall through to
        // prompt history (pi-like). Single-line keeps history on every Up/Down.
        if (event.code == KeyCode::ArrowUp) {
            model_->clearTabCompletion();
            auto [line, col] = model_->composer.cursor_line_col();
            (void)col;
            auto ls = model_->composer.lines();
            if (ls.size() > 1 && line > 0) {
                model_->composer.handle_key(event);
                return true;
            }
            model_->historyPrevious();
            return true;
        }
        if (event.code == KeyCode::ArrowDown) {
            model_->clearTabCompletion();
            auto [line, col] = model_->composer.cursor_line_col();
            (void)col;
            auto ls = model_->composer.lines();
            if (ls.size() > 1 && line + 1 < static_cast<int>(ls.size())) {
                model_->composer.handle_key(event);
                return true;
            }
            model_->historyNext();
            return true;
        }
        // Pi-style composer contract:
        //   Enter           → submit (or run /command)
        //   Shift+Enter     → newline
        //   Ctrl+Enter      → newline (Windows Terminal / fallback)
        // Requires inkcell keyboard_enhance (CSI-u / modifyOtherKeys) so
        // modified Enter is distinguishable from bare CR.
        if (event.code == KeyCode::Enter) {
            model_->clearTabCompletion();
            if (event.shift() || event.ctrl()) {
                // Newline path — TextArea inserts '\n' on Enter.
                return model_->composer.handle_key(event);
            }
            if (runSlashCommand()) return true;
            if (chat::runBangCommand(*model_, model_->composer.value)) return true;
            model_->submitComposer();
            return true;
        }
        // PageUp/PageDown scroll the transcript without leaving the composer.
        // Home/End are LINE motions in the multi-line prompt (TextArea),
        // not transcript jumps — that was part of the unusable one-row UX.
        if (event.code == KeyCode::PageUp) {
            model_->transcriptView.scroll_by(-std::max(1, model_->transcriptView.viewport_h / 2));
            return true;
        }
        if (event.code == KeyCode::PageDown) {
            model_->transcriptView.scroll_by(std::max(1, model_->transcriptView.viewport_h / 2));
            return true;
        }
        if (event.code == KeyCode::Home || event.code == KeyCode::End) {
            model_->clearTabCompletion();
            return model_->composer.handle_key(event);
        }
        // Any normal edit invalidates the tab-cycle stem (readline behavior).
        if (event.code != KeyCode::Tab && event.code != KeyCode::BackTab)
            model_->clearTabCompletion();
        if (event.code == KeyCode::Paste) {
            int nlines = 1;
            for (char c : event.text)
                if (c == '\n') ++nlines;
            bool ok = model_->composer.handle_key(event);
            if (nlines >= 8) {
                chat::Notification n;
                n.id = "paste";
                n.source = "composer";
                n.severity = nlines > 200 ? "warn" : "info";
                n.lifetimeMs = 2200;
                n.title = std::to_string(nlines) + " lines pasted";
                model_->notificationStack.push(std::move(n));
            }
            return ok;
        }
        if (model_->composer.handle_key(event)) return true;
        return false;
    }

    void draw(inkcell::Surface& surface) const override {
        if (layout::render_min_size_notice(surface)) return;
        // Claim bottom inset (page is inset(2)) — footer sits on the glass edge.
        auto p = layout::page(surface);
        {
            auto b = surface.bounds();
            if (p.bottom() < b.bottom()) p.h = b.bottom() - p.y;
        }

        chat::ChatSurfaceModel vm;
        vm.path = model_->breadcrumb();
        // Live agent identity — never paint frozen InkcellAppConfig. Session
        // resume used to leave cfg_ as stale opencode-go/flash while the
        // running Agent was x-ai/grok-4.5 (or whatever -m loaded).
        if (Agent* ident = model_->currentAgent() ? model_->currentAgent() : model_->rootAgent) {
            const auto& c = ident->config();
            vm.provider = !c.provider.empty() ? c.provider : model_->agentProvider;
            vm.model = !c.model.empty() ? c.model : model_->agentModel;
        } else {
            vm.provider = !model_->agentProvider.empty() ? model_->agentProvider : cfg_.provider;
            vm.model = !model_->agentModel.empty() ? model_->agentModel : cfg_.model;
        }
        vm.sessionId = model_->activeSessionId;
        vm.status = model_->status;
        {
            // Compact mode chips for the elevated status bar.
            std::string mode;
            if (model_->showRaw) mode = "raw";
            else mode = model_->showThoughts ? "think" : "clean";
            mode += model_->truncateBodies ? " · trunc" : " · full";
            if (model_->rootAgent && model_->rootAgent->config().compaction.enabled)
                mode += " · cpk";
            if (model_->uiDevMode) mode += " · dev";
            // Session identity strip — ends "who am I running" confusion.
            if (!model_->agentName.empty()) {
                mode += " · ";
                mode += model_->agentName;
            }
            if (!model_->activeManifestPath.empty()) {
                auto stem = std::filesystem::path(model_->activeManifestPath)
                                .parent_path()
                                .filename()
                                .string();
                if (stem.empty() || stem == ".")
                    stem = std::filesystem::path(model_->activeManifestPath).stem().string();
                if (!stem.empty() && stem != model_->agentName) {
                    mode += "/";
                    mode += stem;
                }
            }
            vm.mode = mode;
        }
        vm.running = model_->running;
        vm.failed = model_->failed;
        vm.inputFocused = model_->composer.focused && !model_->timelineFocus;
        vm.historyFocused = model_->timelineFocus || !model_->composer.focused;
        vm.showThoughts = model_->showThoughts;
        vm.bodyMode = model_->chatBodyMode;
        vm.timelineRows = &model_->activeRows();
        vm.selectedRow = model_->selectedBlock;
        vm.showRaw = model_->showRaw;
        vm.pendingOps = model_->pendingOps;
        vm.queuedSteer = 0;
        if (Agent* cur = model_->currentAgent()) {
            if (cur->hasPendingSteer()) vm.queuedSteer = 1;
        } else if (model_->rootAgent && model_->rootAgent->hasPendingSteer()) {
            vm.queuedSteer = 1;
        }
        if (!model_->pendingSteerBuffer.empty()) vm.queuedSteer += 1;
        vm.actionCount = model_->actionCount;
        vm.resultCount = model_->resultCount;
        vm.tokenBytes = model_->tokenBytes;
        vm.lastStreamBytes = model_->lastStreamBytes;
        {
            using clock = std::chrono::steady_clock;
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 clock::now().time_since_epoch())
                                 .count();
            vm.nowMs = static_cast<uint64_t>(now < 0 ? 0 : now);
            vm.lastTurnElapsedMs = model_->lastTurnElapsedMs;
            if (model_->running && model_->turnStartMs > 0)
                vm.turnElapsedMs = static_cast<int64_t>(now) - model_->turnStartMs;
            else
                vm.turnElapsedMs = model_->lastTurnElapsedMs;
        }
        vm.scrollOffset = model_->transcriptView.offset;
        vm.followBottom = model_->transcriptView.stick_bottom;
        vm.transcriptSource = &model_->transcriptView.lines;
        vm.transcriptVersion = model_->transcriptVersion;
        vm.transcriptCache = &model_->transcriptWrapCache;
        vm.input = model_->composer.value;
        vm.inputCursor = model_->composer.cursor;
        vm.inputMaxRows = 8;
        // Keep composer scroll so the cursor line stays visible in the box.
        {
            const int promptH = chat::promptBoxHeight(vm, p.w);
            model_->composer.ensure_cursor_visible(promptH);
            vm.inputScrollRow = model_->composer.scroll_row;
        }
        // Classification/palette keys off the assistant label text. In a nested
        // sub-agent scope that label is the CHILD name — use it here too so
        // blocks get the same cyan/kind backgrounds as the parent chat.
        if (!model_->atRoot()) {
            if (Agent* cur = model_->currentAgent()) vm.agentName = cur->name();
            else vm.agentName = model_->agentPath.back();
        } else {
            vm.agentName = model_->agentName;
        }
        vm.scopeName = model_->atRoot() ? std::string() : model_->agentPath.back();
        // Transient readline completion listing — chrome only, never history.
        vm.completionMenu = model_->tabMatches;
        vm.completionSelected = model_->tabMatchIndex;
        // No keybind hint spam on the status bar — help overlay owns that.
        vm.hint.clear();

        // Tick expiry before paint; alert folds into status line (right).
        model_->notificationStack.tick();
        vm.notifications = &model_->notificationStack;
        // j/k selection snap writebacks (draw-side, real wrap spans).
        vm.selectionNavPending = &model_->selectionNavPending;
        vm.scrollOffsetWriteback = &model_->transcriptView.offset;
        vm.stickBottomWriteback = &model_->transcriptView.stick_bottom;
        vm.contentHWriteback = &model_->transcriptView.content_h;

        // Build cyclable footer (under prompt). No top header.
        chat::ChatFooterModel foot;
        foot.pane = model_->chatFooterPane;
        foot.running = vm.running;
        foot.failed = vm.failed;
        foot.inputFocused = vm.inputFocused;
        foot.nowMs = vm.nowMs;
        foot.turnElapsedMs = vm.turnElapsedMs;
        foot.actionCount = vm.actionCount;
        foot.resultCount = vm.resultCount;
        foot.pendingOps = vm.pendingOps;
        foot.tokenBytes = vm.running ? vm.tokenBytes : (vm.tokenBytes > 0 ? vm.tokenBytes : vm.lastStreamBytes);
        foot.lastStreamBytes = vm.lastStreamBytes;
        if (Agent* ident = model_->currentAgent())
            foot.agentName = ident->name();
        else
            foot.agentName = model_->agentName;
        foot.provider = vm.provider;
        foot.model = vm.model;
        foot.sessionId = vm.sessionId;
        foot.path = vm.path;
        foot.themeName = theme::name();
        foot.bodyFmt = std::string("in:") + bodyRenderModeName(model_->actionBodyMode) +
                       " · out:" + bodyRenderModeName(model_->resultBodyMode) +
                       (model_->truncateBodies ? " · trunc" : " · full");
        if (!model_->activeManifestPath.empty()) {
            auto stem = std::filesystem::path(model_->activeManifestPath)
                            .parent_path().filename().string();
            if (stem.empty() || stem == ".")
                stem = std::filesystem::path(model_->activeManifestPath).stem().string();
            foot.manifestStem = stem;
        }
        // Context pressure + iteration + history from agent config.
        foot.ctxMaxTokens = 128000;
        foot.ctxCompactAt = 60000;
        foot.ctxUsedTokens = std::max(0, vm.tokenBytes / 3);
        foot.iterCurrent = vm.actionCount;  // rough proxy; replaced below if agent is live
        foot.iterMax = 180;
        foot.historyUsed = 0;
        foot.historyMax = 1700;
        if (Agent* live = model_->currentAgent() ? model_->currentAgent()
                                                 : model_->rootAgent) {
            const auto& c = live->config();
            foot.trimEnabled = c.trim.configured || c.effectiveHistoryCap() > 0;
            foot.trimFilter = c.trim.filterEnabled;
            foot.compactEnabled = c.compaction.enabled;
            foot.compactProfile = c.compaction.profile;
            foot.tailCap = c.effectiveHistoryCap();
            foot.tailEvery = c.effectiveTailEveryTurns();
            foot.trimArmTokens = c.trim.triggerContextTokens;
            foot.compactArmTokens = c.compaction.triggerContextTokens;
            {
                int win = 0;
                if (c.trim.modelContextTokens > 0) win = c.trim.modelContextTokens;
                if (c.compaction.modelContextTokens > win)
                    win = c.compaction.modelContextTokens;
                if (win > 0) foot.ctxMaxTokens = win;
            }
            // Arm the nearest live trigger (trim first — it fires first).
            if (c.trim.filterEnabled && c.trim.triggerContextTokens > 0)
                foot.ctxCompactAt = c.trim.triggerContextTokens;
            else if (c.compaction.triggerContextTokens > 0)
                foot.ctxCompactAt = c.compaction.triggerContextTokens;
            else if (c.trim.triggerContextTokens > 0)
                foot.ctxCompactAt = c.trim.triggerContextTokens;
            foot.iterMax = c.iterationCap;
            int li = live->liveIteration();
            int lastI = live->lastIteration();
            foot.iterCurrent = li > 0 ? li : lastI;
            foot.historyMax = c.effectiveHistoryCap();
            foot.historyUsed = static_cast<int>(live->history().size());
            // Same estimator compact uses. Live stream bytes are NOT prompt tokens.
            foot.ctxUsedTokens = live->estimatedPromptTokens();
            if (foot.ctxMaxTokens <= 0) foot.ctxMaxTokens = 128000;
            // Flash COMPACTED ~8s after a real compact — never sticky forever.
            foot.compactedRecently =
                live->compactBadgeActive(static_cast<int64_t>(vm.nowMs));
            foot.lastEconomyCode = live->lastEconomyCode();
        }
        // Phase from live timeline tail.
        foot.phaseKey = "ready";
        foot.phaseDetail.clear();
        if (model_->failed) foot.phaseKey = "fail";
        else if (!model_->running && model_->status == "waiting human input")
            foot.phaseKey = "ask";
        else if (model_->running) {
            if (model_->status.rfind("cancel", 0) == 0) foot.phaseKey = "cancel";
            else {
                foot.phaseKey = "wait";  // default until we see structure
                // Children in flight beat a parent thought / stale tool verb.
                const auto& phaseRows = model_->activeRows();
                if (model_->pendingOps > 0) {
                    for (auto it = phaseRows.rbegin();
                         it != phaseRows.rend(); ++it) {
                        if (it->kind != TimelineKind::Action || it->actionId.empty()) continue;
                        if (!model_->pendingActionIds.count(it->actionId)) continue;
                        const bool child =
                            it->actionType == "agent" ||
                            (model_->rootAgent && !it->actionName.empty() &&
                             model_->rootAgent->hasSubAgent(it->actionName));
                        if (child) {
                            foot.phaseKey = "delegate";
                            foot.phaseDetail =
                                it->actionName.empty() ? "subagent" : it->actionName;
                            break;
                        }
                    }
                }
                // Open tools own the plate. Thought tokens still arriving
                // (native thinking while grep/read run) must NOT paint
                // "thinking" over 3 open cards — that's the hang lie.
                if (foot.phaseKey != "delegate" && model_->pendingOps <= 0) {
                    foot.phaseKey = "wait";
                    foot.phaseDetail.clear();
                } else if (foot.phaseKey != "delegate" && model_->pendingOps > 0) {
                    foot.phaseKey = "act";
                }
                int scanned = 0;
                for (auto it = phaseRows.rbegin();
                     it != phaseRows.rend() && scanned < 12 &&
                     foot.phaseKey != "delegate";
                     ++it, ++scanned) {
                    if (model_->pendingOps > 0 &&
                        (it->kind == TimelineKind::Thought ||
                         it->kind == TimelineKind::Response))
                        continue;
                    if (it->kind == TimelineKind::Thought) {
                        foot.phaseKey = "think";
                        break;
                    }
                    if (it->kind == TimelineKind::Response) {
                        foot.phaseKey = "reply";
                        break;
                    }
                    // Only paint tool verb if that action is still pending.
                    if (it->kind == TimelineKind::Action) {
                        if (!it->actionId.empty() &&
                            !model_->pendingActionIds.count(it->actionId)) {
                            continue;  // completed tool — keep scanning / stay on wait
                        }
                        const std::string& n = it->actionName;
                        if (it->actionType == "agent" ||
                            (model_->rootAgent && !n.empty() &&
                             model_->rootAgent->hasSubAgent(n))) {
                            foot.phaseKey = "delegate";
                            foot.phaseDetail = n.empty() ? "subagent" : n;
                            break;
                        }
                        foot.phaseKey = "act";
                        auto chip = [&](const std::string& body) -> std::string {
                            // basename of path= or first short token
                            auto p = body.find("\"path\"");
                            if (p == std::string::npos) p = body.find("path");
                            // try JSON path via simple scan
                            auto k = body.find("\"path\":");
                            if (k == std::string::npos) k = body.find("\"path\": ");
                            std::string s;
                            auto extract = [&](const char* key) {
                                std::string pat = std::string("\"") + key + "\":\"";
                                auto at = body.find(pat);
                                if (at == std::string::npos) return std::string();
                                at += pat.size();
                                auto end = body.find('"', at);
                                if (end == std::string::npos) return std::string();
                                return body.substr(at, end - at);
                            };
                            s = extract("path");
                            if (s.empty()) s = extract("command");
                            if (s.empty()) s = extract("pattern");
                            if (s.empty()) s = extract("query");
                            if (s.size() > 28) {
                                auto slash = s.find_last_of('/');
                                if (slash != std::string::npos) s = s.substr(slash + 1);
                                if (s.size() > 28) s = s.substr(0, 26) + "…";
                            }
                            return s;
                        };
                        std::string detail = chip(it->body);
                        if (n == "fs_read" || n == "read" || n.find("read") != std::string::npos) {
                            foot.phaseDetail = detail.empty() ? "reading" : ("reading " + detail);
                        } else if (n == "fs_write" || n == "write") {
                            foot.phaseDetail = detail.empty() ? "writing" : ("writing " + detail);
                        } else if (n == "exec" || n == "bash" || n == "shell") {
                            foot.phaseDetail = detail.empty() ? "running command" : ("exec " + detail);
                        } else if (n == "grep" || n == "search") {
                            foot.phaseDetail = detail.empty() ? "searching" : ("search " + detail);
                        } else if (n == "list" || n == "ls") {
                            foot.phaseDetail = detail.empty() ? "listing" : ("list " + detail);
                        } else if (n == "ask" || n.find("ask") != std::string::npos) {
                            foot.phaseKey = "ask";
                            foot.phaseDetail = "card";
                        } else if (!n.empty()) {
                            foot.phaseDetail = n;
                        }
                        break;
                    }
                    if (it->kind == TimelineKind::Result) {
                        // Just finished a tool — if no newer thought, still acting/recovering
                        if (!it->ok) {
                            foot.phaseKey = "act";
                            foot.phaseDetail = "recovering";
                        }
                        // keep scanning for thought/action above
                        continue;
                    }
                }
            }
        }
        foot.turnCount = 0;
        for (const auto& r : model_->rootRows)
            if (r.kind == TimelineKind::User) ++foot.turnCount;

        // ── Instrument feed (footer was empty without this) ───────────
        foot.queuedSteer = vm.queuedSteer;
        foot.bodyMode = model_->chatBodyMode;
        foot.focusLine.clear();
        foot.openLine.clear();
        foot.lastResultLine.clear();
        foot.statusHint.clear();
        foot.childPending = 0;
        {
            const auto& rows = model_->activeRows();
            std::vector<std::string> openBits;
            openBits.reserve(8);
            for (const auto& r : rows) {
                if (r.kind != TimelineKind::Action || r.actionId.empty()) continue;
                if (!model_->pendingActionIds.count(r.actionId)) continue;
                std::string bit =
                    r.actionName.empty() ? r.actionType : r.actionName;
                if (bit.empty()) bit = "tool";
                bit += " #";
                bit += r.actionId;
                openBits.push_back(bit);
                if (r.actionType == "agent" ||
                    (model_->rootAgent && !r.actionName.empty() &&
                     model_->rootAgent->hasSubAgent(r.actionName)))
                    ++foot.childPending;
            }
            if (!openBits.empty()) {
                foot.openLine = std::to_string(openBits.size()) + " open: ";
                for (size_t i = 0; i < openBits.size() && i < 3; ++i) {
                    if (i) foot.openLine += ", ";
                    foot.openLine += openBits[i];
                }
                if (openBits.size() > 3) foot.openLine += "…";
                foot.focusLine = openBits.back();
            } else if (!foot.phaseDetail.empty() &&
                       (foot.phaseKey == "act" || foot.phaseKey == "delegate")) {
                foot.focusLine = foot.phaseDetail;
            }
            // Newest completed result / status for the truth row.
            for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
                if (it->kind == TimelineKind::Result) {
                    std::string line = it->ok ? "✓ " : "✗ ";
                    if (!it->actionName.empty())
                        line += it->actionName;
                    else
                        line += "result";
                    if (!it->actionId.empty()) {
                        line += " #";
                        line += it->actionId;
                    }
                    // First non-empty body line as payload teaser
                    std::string body = it->body;
                    auto nl = body.find('\n');
                    if (nl != std::string::npos) body = body.substr(0, nl);
                    // strip leading spaces
                    size_t b0 = body.find_first_not_of(" \t");
                    if (b0 != std::string::npos) body = body.substr(b0);
                    if (!body.empty() && body != "tree" && body != "list") {
                        if (body.size() > 48) body = body.substr(0, 46) + "…";
                        line += "  ·  ";
                        line += body;
                    } else if (!it->title.empty()) {
                        line += "  ·  ";
                        line += it->title;
                    }
                    foot.lastResultLine = line;
                    break;
                }
                if (it->kind == TimelineKind::Status) {
                    std::string s = it->body.empty() ? it->title : it->body;
                    auto nl = s.find('\n');
                    if (nl != std::string::npos) s = s.substr(0, nl);
                    if (s.size() > 96) s = s.substr(0, 94) + "…";
                    foot.statusHint = s;
                    // keep scanning for a Result if Status is older noise
                    if (foot.lastResultLine.empty() &&
                        (s.find('[') != std::string::npos || s.rfind("LIMIT", 0) == 0 ||
                         s.find("FALLBACK") != std::string::npos ||
                         s.find("TIMEOUT") != std::string::npos))
                        break;
                    continue;
                }
            }
            // Prefer fresh status over stale result when live fallback/timeout.
            if (!foot.statusHint.empty() &&
                (foot.statusHint.find("FALLBACK") != std::string::npos ||
                 foot.statusHint.find("TIMEOUT") != std::string::npos ||
                 foot.statusHint.find("CANCEL") != std::string::npos ||
                 foot.statusHint.find("403") != std::string::npos))
                foot.lastResultLine.clear();  // truth row shows statusHint
        }

        // Reserve: fixed 3-row footer + prompt + menu.
        int menuH = chat::completionMenuHeight(vm, p.w);
        const int promptH = chat::promptBoxHeight(vm, p.w);
        const int footerH = chat::chatFooterReserve(&foot, p.h, promptH, menuH);
        model_->transcriptView.viewport_h =
            std::max(1, p.h - footerH - promptH - menuH);
        // Keep scroll math in DISPLAY space (wrap total), not source-line count.
        if (model_->transcriptWrapCache.totalDisplayLines > 0)
            model_->transcriptView.content_h = model_->transcriptWrapCache.totalDisplayLines;
        if (model_->transcriptView.stick_bottom) model_->transcriptView.scroll_to_end();
        else model_->transcriptView.clamp();

        chat::drawChatSurface(surface, p, vm, &foot);

        // drawChatSurface refreshes wrap cache — publish height for next clamp/j/k.
        if (model_->transcriptWrapCache.totalDisplayLines > 0)
            model_->transcriptView.content_h = model_->transcriptWrapCache.totalDisplayLines;

        if (model_->askActive)
            chat::drawAskDialog(surface, p, model_->askDialog, model_->askInput.value,
                                model_->askMultiSelected);
        else if (model_->helpVisible)
            chat::drawHelpOverlay(surface, p);
        // Palette above help/ask chrome
        components::drawCmdPalette(surface, p, model_->cmdPalette);
        // Block reader on top of everything except (none) — full-page read mode.
        if (model_->blockReader.open)
            chat::drawBlockReader(surface, p, model_->blockReader);
    }

    void handleModelCommand(const std::string& spec) {
        auto curProv = model_->agentProvider;
        auto curModel = model_->agentModel;
        if (model_->rootAgent) {
            const auto& c = model_->rootAgent->config();
            if (!c.provider.empty()) curProv = c.provider;
            if (!c.model.empty()) curModel = c.model;
        }
        if (spec.empty()) {
            std::vector<std::string> lines;
            lines.push_back("current  " + curProv + "/" + curModel);
            lines.push_back("usage    /model <provider>/<model>");
            lines.push_back("         /model <model>     (keeps provider)");
            lines.push_back("providers");
            for (const auto& p : providers::availableProviders())
                lines.push_back("  " + p + "  default=" + providers::defaultProviderModel(p));
            model_->appendNotice("model", lines);
            return;
        }
        if (model_->running) {
            model_->appendNotice("model", {"turn live — stop first (ctrl-x), then /model"});
            return;
        }
        if (!model_->rootAgent) {
            model_->appendNotice("model", {"no live agent"});
            return;
        }
        std::string prov = curProv;
        std::string mod = spec;
        size_t slash = spec.find('/');
        if (slash != std::string::npos) {
            prov = spec.substr(0, slash);
            mod = spec.substr(slash + 1);
        }
        if (prov.empty() || mod.empty()) {
            model_->appendNotice("model", {"expected /model provider/model or /model model"});
            return;
        }
        auto next = providers::createProvider(prov, mod);
        if (!next) {
            model_->appendNotice("model", {"unknown provider: " + prov,
                                           "try /model for the list"});
            return;
        }
        next->setQuietLogs(true);
        model_->rootAgent->setProvider(next, prov, mod);
        model_->agentProvider = prov;
        model_->agentModel = mod;
        // Persist onto the session so --continue keeps the switch.
        if (!model_->activeSessionId.empty()) {
            try {
                session::SessionManager sm;
                if (sm.exists(model_->activeSessionId)) {
                    auto s = sm.load(model_->activeSessionId);
                    s.provider = prov;
                    s.model = mod;
                    s.metadata["provider"] = prov;
                    s.metadata["model"] = mod;
                    sm.save(s);
                }
            } catch (...) {
            }
        }
        model_->appendNotice("model", {"switched → " + prov + "/" + mod});
    }

   private:
    void runPaletteAction(const std::string& id) {
        if (id == "nav.main") {
            model_->requestRoute(PendingRoute::Main);
            return;
        }
        if (id == "chat.thoughts") {
            model_->showThoughts = !model_->showThoughts;
            model_->markProjFull();
            model_->rebuildViews();
            persistUiPrefs(*model_);
            return;
        }
        if (id == "chat.scroll_down") {
            model_->transcriptView.scroll_by(1);
            return;
        }
        if (id == "chat.scroll_up") {
            model_->transcriptView.scroll_by(-1);
            return;
        }
        if (id == "chat.truncate") {
            model_->toggleTruncateBodies();
            persistUiPrefs(*model_);
            return;
        }
        if (id == "chat.raw") {
            model_->showRaw = !model_->showRaw;
            model_->markProjFull();
            model_->rebuildViews();
            persistUiPrefs(*model_);
            return;
        }
        if (id == "chat.clear") {
            model_->clearTranscript();
            return;
        }
        if (id == "chat.stop") {
            stopAgentLoop("palette");
            return;
        }
        if (id == "act.theme") {
            theme::toggle();
            persistUiPrefs(*model_);
            return;
        }
        if (id == "sys.quit") {
            model_->requestRoute(PendingRoute::Quit);
            return;
        }
        if (id.rfind("slash:", 0) == 0) {
            model_->composer.value = id.substr(6);
            model_->composer.cursor = static_cast<int>(model_->composer.value.size());
            model_->composer.focused = true;
            runSlashCommand();
        }
    }

    bool handleAskKey(const inkcell::KeyEvent& event) {
        using inkcell::KeyCode;
        auto* card = model_->askDialog.index < model_->askDialog.cards.size()
                         ? &model_->askDialog.cards[model_->askDialog.index]
                         : nullptr;
        if (!card) return true;

        if (event.code == KeyCode::Escape || event.code == KeyCode::CtrlC) {
            model_->askDialog.cancelled = true;
            model_->askActive = false;
            model_->closeModalFocus("ask");
            bridge_.cancelAsk();
            return true;
        }
        if (event.code == KeyCode::ArrowUp ||
            (event.code == KeyCode::Character && (event.ch == 'k' || event.ch == 'K'))) {
            model_->askDialog.selectedOption = std::max(0, model_->askDialog.selectedOption - 1);
            return true;
        }
        if (event.code == KeyCode::ArrowDown ||
            (event.code == KeyCode::Character && (event.ch == 'j' || event.ch == 'J'))) {
            model_->askDialog.selectedOption = std::min(
                std::max(0, static_cast<int>(card->options.size()) - 1),
                model_->askDialog.selectedOption + 1);
            return true;
        }
        if (card->type == "confirm" && event.code == KeyCode::Character &&
            (event.ch == 'y' || event.ch == 'Y' || event.ch == 'n' || event.ch == 'N')) {
            chat::advanceDialog(model_->askDialog, event.ch == 'y' || event.ch == 'Y');
            finishAskCard();
            return true;
        }
        if (card->type == "multi_choice" && event.code == KeyCode::Character && event.ch == ' ') {
            int selected = model_->askDialog.selectedOption;
            if (selected >= 0 && selected < static_cast<int>(card->options.size()) &&
                !card->options[static_cast<size_t>(selected)].disabled) {
                if (model_->askMultiSelected.count(selected)) model_->askMultiSelected.erase(selected);
                else model_->askMultiSelected.insert(selected);
            }
            return true;
        }
        if (event.code == KeyCode::Enter) {
            bool accepted = false;
            if (card->type == "choice") {
                int selected = model_->askDialog.selectedOption;
                if (selected >= 0 && selected < static_cast<int>(card->options.size()) &&
                    !card->options[static_cast<size_t>(selected)].disabled) {
                    chat::advanceDialog(model_->askDialog,
                                        card->options[static_cast<size_t>(selected)].value);
                    accepted = true;
                }
            } else if (card->type == "multi_choice") {
                std::string indices;
                for (int selected : model_->askMultiSelected) {
                    if (!indices.empty()) indices += ',';
                    indices += std::to_string(selected + 1);
                }
                accepted = chat::handleDialogLine(model_->askDialog, indices);
            } else if (card->type == "ranker" && model_->askInput.value.empty()) {
                std::string indices;
                for (int i = 0; i < static_cast<int>(card->options.size()); ++i) {
                    if (!indices.empty()) indices += ',';
                    indices += std::to_string(i + 1);
                }
                accepted = chat::handleDialogLine(model_->askDialog, indices);
            } else {
                accepted = chat::handleDialogLine(model_->askDialog, model_->askInput.value);
            }
            if (accepted) finishAskCard();
            return true;
        }
        if (model_->askInput.handle_key(event)) return true;
        return true;
    }

    void finishAskCard() {
        model_->askInput.value.clear();
        model_->askInput.cursor = 0;
        model_->askMultiSelected.clear();
        model_->askDialog.selectedOption = 0;
        chat::completeNonInteractiveCards(model_->askDialog);
        // Unblock the worker thread waiting on bridge.requestAsk(). Without
        // completeAsk/cancelAsk here, ask_tool hangs forever after the operator
        // answers in the TUI (P0).
        if (model_->askDialog.done()) {
            model_->askActive = false;
            model_->closeModalFocus("ask");
            if (model_->askDialog.cancelled) {
                bridge_.cancelAsk();
                model_->appendNotice("ask", {"cancelled"});
            } else {
                bridge_.completeAsk(model_->askDialog.results);
                std::vector<std::string> lines;
                const auto& members = model_->askDialog.results.getMemberNames();
                for (const auto& key : members) {
                    const auto& v = model_->askDialog.results[key];
                    std::string val;
                    if (v.isString()) {
                        val = v.asString();
                    } else if (v.isBool()) {
                        val = v.asBool() ? "true" : "false";
                    } else if (v.isInt() || v.isUInt() || v.isInt64() || v.isUInt64()) {
                        val = std::to_string(v.asInt64());
                    } else if (v.isDouble()) {
                        val = std::to_string(v.asDouble());
                    } else {
                        Json::StreamWriterBuilder wb;
                        wb["indentation"] = "";
                        val = Json::writeString(wb, v);
                    }
                    lines.push_back(key + " = " + val);
                }
                if (lines.empty()) lines.push_back("(no fields)");
                model_->appendNotice("ask answered", lines);
            }
            if (!model_->running && model_->status == "waiting human input")
                model_->status = "ready";
        }
    }

    bool slashDevMode() const {
        if (model_->uiDevMode) return true;
        if (model_->rootAgent && model_->rootAgent->devMode()) return true;
        if (const char* e = std::getenv("CORTEX_DEV_MODE")) {
            std::string v = e;
            if (!(v.empty() || v == "0" || v == "false" || v == "FALSE"))
                return true;
        }
        return false;
    }

    bool runSlashCommand() {
        const std::string command = model_->composer.value;
        if (command.empty() || command[0] != '/') return false;

        // History: keep slash (+ args). No consecutive exact duplicate.
        model_->pushPromptHistory(chat::trimCommandText(command));

        // ── Composer command batching ────────────────────────────────
        // A prompt-box input may carry several /skill:x /prompt:y tags plus
        // free-text. Resolve each tag to its expanded body and submit the
        // composed prompt directly (unlike a single control slash which sets
        // composerReplacement for editing). Guards: skip batching for pure
        // control commands (quit/stop/theme/...) — those stay single-shot.
        if (chat::looksLikeCommandTag(command)) {
            auto dynamicCommands = chat::discoverDynamicChatCommands();
            auto batch = chat::parseComposerBatch(command, dynamicCommands);
            if (batch.anyCommand && batch.commands.size() > 0) {
                // TRUE batching intent = multiple tags, OR a single tag PLUS
                // free text. A lone '/skill:x' (no plain text) falls through to
                // the classic single-command path (edit body in composer first).
                const bool isPureSingle =
                    batch.commands.size() == 1 && batch.plainText.empty();
                const bool batchIntent =
                    !isPureSingle &&
                    !(batch.commands.size() == 1 && !batch.commands[0].command);
                if (batchIntent) {
                    // Unknown tags → report, don't silently drop.
                    if (!batch.allResolved) {
                        std::vector<std::string> lines;
                        for (const auto& t : batch.commands)
                            if (!t.resolved)
                                lines.push_back("unknown command: " + t.name);
                        lines.push_back("use /help for available commands");
                        model_->appendNotice("batch", lines);
                        model_->composer.value.clear();
                        model_->composer.cursor = 0;
                        model_->rebuildViews();
                        return true;
                    }
                    // Compose and submit as a real prompt if any text/expanded body.
                    std::string prompt = chat::composeBatchPrompt(batch);
                    if (!chat::trimCommandText(prompt).empty()) {
                        model_->composer.value = prompt;
                        model_->composer.cursor =
                            static_cast<int>(model_->composer.value.size());
                        model_->submitComposer();
                        // submitComposer consumed composer.value; surface the
                        // resolved tags so the operator sees what got loaded.
                        if (batch.commands.size() > 1 || !batch.plainText.empty()) {
                            std::string summary;
                            for (size_t i = 0; i < batch.commands.size(); ++i)
                                summary += (i ? " " : "") + batch.commands[i].name;
                            model_->appendNotice("batch",
                                                 {"loaded " + summary});
                        }
                        return true;
                    }
                }
            }
        }

        chat::ChatCommandContext ctx;
        ctx.manifestPath = cfg_.manifestPath;
        ctx.harnessPath = cfg_.harnessPath;
        ctx.systemPromptPath = cfg_.systemPromptPath;
        ctx.personaPath = cfg_.personaPath;
        if (model_->rootAgent && !model_->rootAgent->config().userPath.empty())
            ctx.userPath = model_->rootAgent->config().userPath;
        ctx.toolCount = cfg_.toolCount;
        ctx.feedCount = cfg_.feedCount;
        ctx.relicCount = cfg_.relicCount;
        ctx.subAgentCount = cfg_.subAgentCount;
        ctx.devMode = slashDevMode();
        auto result = chat::executeChatCommand(command, ctx);
        if (!result.handled) return false;

        model_->composer.value.clear();
        model_->composer.cursor = 0;
        if (result.quit) model_->requestRoute(PendingRoute::Quit);
        if (result.stopLoop) stopAgentLoop("slash");
        if (result.continueLoop) {
            if (model_->running) {
                model_->appendNotice("continue", {"already running"});
            } else if (!model_->rootAgent) {
                model_->appendNotice("continue", {"no live agent"});
            } else if (model_->rootAgent->history().empty()) {
                model_->appendNotice("continue", {"nothing to continue — empty history"});
            } else {
                // Silent kick: empty prompt, no YOU row (agent skips User: push).
                if (model_->activeSessionId.empty()) {
                    model_->activeSessionId = session::mintSessionId();
                    session::activeSession().set(model_->activeSessionId,
                                                 session::activeSession().isEphemeral());
                }
                model_->pendingContinue = true;
                model_->status = "continuing";
            }
        }
        if (result.replayFirst) {
            std::string first;
            for (const auto& r : model_->rootRows) {
                if (r.kind == TimelineKind::User && !r.body.empty()) {
                    first = r.body;
                    break;
                }
            }
            if (first.empty() && !model_->promptHistory.empty()) {
                // Skip slash-only history entries.
                for (const auto& h : model_->promptHistory) {
                    if (!h.empty() && h[0] != '/') {
                        first = h;
                        break;
                    }
                }
            }
            if (first.empty() && model_->rootAgent) {
                for (const auto& h : model_->rootAgent->history()) {
                    if (h.rfind("User: ", 0) == 0) {
                        first = h.substr(6);
                        break;
                    }
                }
            }
            if (first.empty()) {
                model_->appendNotice("replay", {"no first prompt — nothing to rerun"});
            } else {
                if (model_->running) {
                    stopAgentLoop("replay");
                    model_->running = false;  // don't steer — arm a new turn
                }
                while (!model_->atRoot()) model_->goBack();
                if (model_->rootAgent) {
                    model_->rootAgent->clearHistory();
                    for (const auto& name : model_->rootAgent->subAgentNames()) {
                        if (Agent* ch = model_->rootAgent->getSubAgent(name))
                            ch->clearHistory();
                    }
                }
                model_->clearTranscript();
                model_->pendingSteerBuffer.clear();
                model_->pendingContinue = false;
                model_->composer.value = first;
                model_->composer.cursor = static_cast<int>(first.size());
                model_->submitComposer();
                model_->appendNotice("replay", {"wiped · replaying first prompt"});
            }
        }
        if (result.clearTranscript) model_->clearTranscript();
        bool prefsDirty = false;
        if (result.toggleThoughts) {
            model_->showThoughts = !model_->showThoughts;
            model_->markProjFull();
            model_->rebuildViews();
            prefsDirty = true;
        }
        if (result.toggleTruncate) {
            model_->toggleTruncateBodies();
            prefsDirty = true;
        }
        if (result.toggleRaw) {
            model_->showRaw = !model_->showRaw;
            prefsDirty = true;
        }
        if (result.toggleTheme) {
            if (result.themeName == "graphite") theme::set(theme::Variant::Graphite);
            else if (result.themeName == "neon") theme::set(theme::Variant::Neon);
            else theme::toggle();
            result.lines = {std::string("active theme: ") + theme::name()};
            prefsDirty = true;
        }
        if (prefsDirty) persistUiPrefs(*model_);
        if (result.showPrompts) showCapturedPrompts();
        if (result.dumpPrompts) {
            auto messages = chat::dumpPrompts(model_->rootAgent ? model_->rootAgent->iterationPrompts()
                                                                : std::vector<std::string>{});
            model_->appendNotice("dump prompt", messages);
        }
        if (result.exportChat) {
            std::ostringstream hdr;
            hdr << "# cortex-mk3 chat export\n"
                << "# agent=" << model_->agentName
                << " provider=" << model_->agentProvider
                << " model=" << model_->agentModel << "\n"
                << "# session=" << (model_->activeSessionId.empty() ? "(none)"
                                                                   : model_->activeSessionId)
                << "\n"
                << "# source=timeline rows (structured markdown, full bodies)\n";
            auto copied = chat::exportTimelineMarkdown(model_->rootRows, hdr.str());
            model_->appendNotice("export chat",
                                 {copied.copied ? "wrote " + copied.destination
                                                : "failed " + copied.destination});
        }
        if (result.exportDump) {
            if (!model_->rootAgent) {
                model_->appendNotice("export dump", {"no live agent — launch one first"});
            } else {
                model_->rootAgent->dumpSessionArtifacts(/*force=*/true);
                std::string dir = model_->rootAgent->lastDevDumpDir();
                model_->appendNotice(
                    "export dump",
                    {dir.empty() ? "dump wrote (path unknown — see .cortex/dev/)"
                                 : ("wrote " + dir +
                                    " (iterations.md raw.md history.md protocol.md)"),
                     "no agent.yml runtime.dev_mode required"});
            }
        }
        if (result.openArtifacts) {
            if (model_->suspendTui) model_->suspendTui();
            int rc = chat::launchArtFullscreen(result.artifactsArgs,
                                              /*manageScreen=*/!static_cast<bool>(model_->suspendTui));
            if (model_->resumeTui) model_->resumeTui();
            // Silent on success — no toast spam. Only surface real failures.
            if (rc != 0) {
                model_->appendNotice("artifacts",
                                     {"art exited " + std::to_string(rc),
                                      "need ~/.pi/agent/bin/art ?"});
            }
            model_->transcriptWrapCache.invalidate();
            ++model_->transcriptVersion;
            model_->markProjFull();
            model_->rebuildViews();
        }
        if (result.switchModel) {
            handleModelCommand(result.modelSpec);
        }
        if (result.copyAll) {
            auto copied = chat::copyText(chat::joinLines(model_->transcriptView.lines), "/tmp/mk3-cp-all.txt");
            model_->appendNotice("copy transcript", {copied.copied ? "wrote " + copied.destination
                                                                  : "failed " + copied.destination});
        }
        if (result.copyRaw) {
            std::string raw = model_->rootAgent ? model_->rootAgent->rawLlOutput() : model_->raw;
            auto copied = chat::copyText(raw, "/tmp/mk3-cp-raw.txt");
            model_->appendNotice("copy raw", {copied.copied ? "wrote " + copied.destination
                                                           : "failed " + copied.destination});
        }
        if (!result.composerReplacement.empty()) {
            model_->composer.value = result.composerReplacement;
            model_->composer.cursor = static_cast<int>(model_->composer.value.size());
        } else if (!result.lines.empty()) {
            model_->appendNotice(result.title, result.lines);
        }
        model_->rebuildViews();
        return true;
    }

    void showCapturedPrompts() {
        std::vector<std::string> lines;
        if (!model_->rootAgent || model_->rootAgent->iterationPrompts().empty()) {
            lines.push_back("no prompts captured — run a prompt first");
        } else {
            const auto& prompts = model_->rootAgent->iterationPrompts();
            for (size_t i = 0; i < prompts.size(); ++i) {
                lines.push_back("--- iteration " + std::to_string(i + 1) + " ---");
                auto promptLines = splitDisplayLines(prompts[i]);
                lines.insert(lines.end(), promptLines.begin(), promptLines.end());
            }
        }
        model_->appendNotice("prompts", lines);
    }

    void stopAgentLoop(const char* reason) {
        if (model_->askActive) {
            model_->askDialog.cancelled = true;
            model_->askActive = false;
            model_->closeModalFocus("ask");
            bridge_.cancelAsk();
            chat::Notification n;
            n.id = "cancel";
            n.source = "cancel";
            n.severity = "warn";
            n.title = std::string("ask cancelled · ") + reason;
            n.lifetimeMs = 2800;
            model_->notificationStack.push(std::move(n));
        }
        if (model_->running) {
            model_->status = "cancelling";
            if (model_->rootAgent)
                model_->rootAgent->requestStop(RunStopKind::Operator);
            else
                requestRunStop(RunStopKind::Operator);
            // Optimistic UI settle — don't wait for TurnDone to drop sticky chips.
            // Must clear running here: if TurnDone is slow/lost, the next Enter
            // would take the steer path and never paint a YOU row.
            model_->running = false;
            model_->pendingActionIds.clear();
            model_->pendingOps = 0;
            model_->actionCount = 0;
            model_->resultCount = 0;
            chat::Notification n;
            n.id = "cancel";
            n.source = "cancel";
            n.severity = "warn";
            n.title = std::string("stopping · ") + reason;
            n.lifetimeMs = 2800;
            model_->notificationStack.push(std::move(n));
            // Keep transcript clean — status line + alert carry the signal.
        } else if (!model_->askActive) {
            chat::Notification n;
            n.id = "cancel";
            n.source = "cancel";
            n.severity = "info";
            n.title = "no active turn";
            n.lifetimeMs = 1800;
            model_->notificationStack.push(std::move(n));
        }
    }

    // GNU-readline-ish slash completion:
    //  1) first Tab → extend to longest common prefix
    //  2) if already at LCP (or single match) → cycle matches
    //  3) Shift-Tab cycles backwards
    //  4) unique match gets a trailing space
    void completeSlashCommand(bool reverse) {
        std::string value = model_->composer.value;
        if (value.empty()) {
            value = "/";
            model_->composer.value = value;
            model_->composer.cursor = 1;
        }
        if (value[0] != '/') return;

        size_t space = value.find(' ');
        // Only complete the command token (before first space).
        if (space != std::string::npos &&
            model_->composer.cursor > static_cast<int>(space))
            return;

        std::string prefix =
            space == std::string::npos ? value : value.substr(0, space);

        // Fresh match set when stem changed.
        if (model_->tabMatches.empty() || model_->tabStem != prefix) {
            model_->tabMatches = chat::completeChatCommand(prefix, slashDevMode());
            model_->tabStem = prefix;
            model_->tabMatchIndex = -1;
        }
        auto& matches = model_->tabMatches;
        if (matches.empty()) {
            // Silent — no matches. Menu stays empty; do not pollute transcript.
            return;
        }

        if (matches.size() == 1) {
            model_->composer.value = matches.front() + " ";
            model_->composer.cursor = static_cast<int>(model_->composer.value.size());
            model_->clearTabCompletion();  // menu disappears after unique resolve
            return;
        }

        std::string lcp = chat::commonPrefixOf(matches);
        // First press (or after typing): jump to LCP if it extends the prefix.
        // Menu stays visible with the narrowed candidate set (readline list).
        if (model_->tabMatchIndex < 0 && lcp.size() > prefix.size()) {
            model_->composer.value = lcp;
            model_->composer.cursor = static_cast<int>(lcp.size());
            model_->tabStem = lcp;
            model_->tabMatches = chat::completeChatCommand(lcp, slashDevMode());
            model_->tabMatchIndex = -1;  // highlight none until cycle
            return;
        }

        // Cycle through full matches — composer shows the pick; menu highlights it.
        if (model_->tabMatchIndex < 0)
            model_->tabMatchIndex = reverse ? static_cast<int>(matches.size()) - 1 : 0;
        else if (reverse)
            model_->tabMatchIndex =
                (model_->tabMatchIndex - 1 + static_cast<int>(matches.size())) %
                static_cast<int>(matches.size());
        else
            model_->tabMatchIndex =
                (model_->tabMatchIndex + 1) % static_cast<int>(matches.size());

        const std::string& pick = matches[static_cast<size_t>(model_->tabMatchIndex)];
        model_->composer.value = pick;
        model_->composer.cursor = static_cast<int>(pick.size());
        model_->tabStem = pick;
        // tabMatches unchanged → draw layer paints the menu with selection.
    }

    void handle(const inkcell::Action& action) override {
        if (action.is("shell.focus_composer")) model_->focusComposer();
        else if (action.is("shell.focus_timeline")) model_->focusTimeline();
        else if (action.is("shell.toggle_raw")) {
            model_->showRaw = !model_->showRaw;
            model_->markProjFull();
            model_->rebuildViews();
        } else if (action.is("shell.toggle_thoughts")) {
            model_->showThoughts = !model_->showThoughts;
            model_->markProjFull();
            model_->rebuildViews();
        } else if (action.is("scroll.up")) model_->selectDelta(-1);
        else if (action.is("scroll.down")) model_->selectDelta(1);
        else if (action.is("history.enter")) model_->enterSelected();
        else if (action.is("history.back")) model_->goBack();
    }
};

}  // namespace cortex::mk3::ui::scenes
