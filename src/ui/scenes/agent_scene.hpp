#pragma once
// Agent / Chat scene.
// Product chat surface: inkcell-native port of ReplSession composition
// (transcript + status + prompt), not the old experimental card UI.

#include <chrono>

#include "base_scene.hpp"
#include "src/ui/chat/chat_commands.hpp"
#include "src/ui/chat/chat_io.hpp"
#include "src/ui/chat/chat_view.hpp"
#include "src/ui/components/cmd_palette.hpp"
#include "src/ui/model/ui_prefs.hpp"

namespace cortex::mk3::ui::scenes {

class AgentScene final : public BaseScene {
   public:
    using BaseScene::BaseScene;
    std::string name() const override { return "Chat"; }

    bool on_key(const inkcell::KeyEvent& event) override {
        using inkcell::KeyCode;

        if (model_->askActive) return handleAskKey(event);

        // Command palette (ctrl-p / space×2 when not typing)
        if (model_->cmdPalette.open && !model_->cmdPalette.closing) {
            std::string action;
            if (components::handleCmdPaletteKey(model_->cmdPalette, event, &action)) {
                if (!action.empty()) runPaletteAction(action);
                return true;
            }
        }
        if (event.code == KeyCode::Character && event.ctrl() &&
            (event.ch == 'p' || event.ch == 'P')) {
            model_->cmdPalette.toggle(components::chatCommands());
            return true;
        }

        // Help-overlay own Esc/?. Always closes first, regardless of focus rung.
        if (model_->helpVisible &&
            (event.code == KeyCode::Escape ||
             (event.code == KeyCode::Character && event.ch == '?'))) {
            model_->helpVisible = false;
            return true;
        }

        // Vet-fix: Esc ladder. Operator priority is "newest by default, `i` is
        // composer key, backspace/Esc back to main". Esc first dismisses the
        // top notification (if any), then drops focus timeline <-> composer
        // without stomp-scrolling. We do not auto-scroll to newest on Esc.
        if (event.code == KeyCode::Escape) {
            if (!model_->notificationStack.empty()) {
                model_->notificationStack.dismissTop();
                return true;
            }
            if (model_->composer.focused) {
                // Composer owned focus — drop to timeline without stomp-scrolling.
                model_->focusTimeline();
                return true;
            }
            if (model_->timelineFocus) {
                // Vet-fix: third rung. Drilldown-aware: in a subagent
                // pop, fall back only on a root timeline. Same chord
                // pattern as Backspace so the operator can use either.
                model_->timelineFocus = false;
                model_->composer.focused = false;
                if (!model_->atRoot()) {
                    model_->goBack();
                } else {
                    model_->pendingRoute = "main";
                }
                return true;
            }
            // No ladder rung consumed — fall through to existing Esc chain.
        }

        // Vet-fix: Backspace navigates. Drilldown-aware:
        //   - in a subagent drilldown: pop one level (don't fall through
        //     to main — operator said "what the fuck are we doing?" when
        //     a single key nuked the whole stack)
        //   - at chat root: route to dashboard
        //   - composer has untyped text: leave alone (delete-letter path)
        if (event.code == KeyCode::Backspace && !model_->askActive &&
            !model_->helpVisible && !model_->cmdPalette.open &&
            (!model_->composer.focused || model_->composer.value.empty())) {
            if (!model_->atRoot()) {
                model_->goBack();
            } else {
                model_->pendingRoute = "main";
            }
            return true;
        }

        if (model_->helpVisible) {
            if (event.code == KeyCode::Escape ||
                (event.code == KeyCode::Character && event.ch == '?'))
                model_->helpVisible = false;
            return true;
        }

        if (event.code == KeyCode::CtrlC) {
            if (model_->running || model_->askActive) {
                stopAgentLoop("ctrl-c");
            } else {
                model_->pendingRoute = "quit";
            }
            return true;
        }
        // Ctrl-X always means stop the loop (never quit) — explicit kill switch.
        if (event.code == KeyCode::Character && event.ctrl() &&
            (event.ch == 'x' || event.ch == 'X')) {
            stopAgentLoop("ctrl-x");
            return true;
        }
        // Global view toggles that must work while typing in the composer.
        // (plain t/r only fire via keymap when the composer is unfocused).
        if (event.code == KeyCode::Character && event.ctrl()) {
            if (event.ch == 't' || event.ch == 'T') {
                model_->showThoughts = !model_->showThoughts;
                model_->rebuildViews();
                persistUiPrefs(*model_);
                return true;
            }
            if (event.ch == 'o' || event.ch == 'O') {
                model_->truncateBodies = !model_->truncateBodies;
                model_->rebuildViews();
                persistUiPrefs(*model_);
                return true;
            }
            if (event.ch == 'r' || event.ch == 'R') {
                model_->showRaw = !model_->showRaw;
                model_->rebuildViews();
                persistUiPrefs(*model_);
                return true;
            }
        }

        if (model_->timelineFocus || !model_->atRoot() || !model_->composer.focused) {
            // Leader-leader only when composer doesn't own space
            if (event.code == KeyCode::Character && !event.ctrl() && event.ch == ' ') {
                if (model_->cmdPalette.noteSpace()) {
                    model_->cmdPalette.show(components::chatCommands());
                    return true;
                }
                return true;
            }
            if (event.code == KeyCode::Character && !event.ctrl())
                model_->cmdPalette.clearLeader();

            if (event.code == KeyCode::Character && (event.ch == 'm' || event.ch == 'M')) {
                model_->pendingRoute = "main";
                return true;
            }
            if (event.code == KeyCode::Character && event.ch == '?') {
                model_->helpVisible = true;
                return true;
            }
            if (event.code == KeyCode::Character && event.ch == 'T') {
                theme::toggle();
                return true;
            }
            if (event.code == KeyCode::Escape) {
                if (model_->goBack()) return true;
                model_->focusComposer();
                return true;
            }
            // Up/Down scroll the transcript line-by-line (free read scroll through
            // history). This is the intuitive scroll every chat has; the prior
            // binding jumped between block markers instead of scrolling.
            if (event.code == KeyCode::ArrowUp) {
                model_->transcriptView.scroll_by(-1);
                return true;
            }
            if (event.code == KeyCode::ArrowDown) {
                model_->transcriptView.scroll_by(1);
                return true;
            }
            // j/k select transcript blocks (drilldown navigation); Enter drills in.
            // Plain j/k = step-by-step; Ctrl-J/Ctrl-K = jump to start/end.
            if (event.code == KeyCode::Character && event.ctrl() &&
                (event.ch == 'j' || event.ch == 'J')) {
                model_->transcriptView.scroll_to_start();
                return true;
            }
            if (event.code == KeyCode::Character && event.ctrl() &&
                (event.ch == 'k' || event.ch == 'K')) {
                model_->transcriptView.scroll_to_end();
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
            // PageUp/PageDown = half-page scroll; Home/End = top/bottom.
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
            if (event.code == KeyCode::Character && event.ch == 'i' && model_->atRoot()) {
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
            completeSlashCommand(/*reverse=*/false);
            return true;
        }
        if (event.code == KeyCode::BackTab) {
            completeSlashCommand(/*reverse=*/true);
            return true;
        }
        if (event.code == KeyCode::ArrowUp) {
            model_->clearTabCompletion();
            model_->historyPrevious();
            return true;
        }
        if (event.code == KeyCode::ArrowDown) {
            model_->clearTabCompletion();
            model_->historyNext();
            return true;
        }
        if (event.code == KeyCode::Enter) {
            model_->clearTabCompletion();
            if (runSlashCommand()) return true;
            model_->submitComposer();
            return true;
        }
        // PageUp/PageDown scroll the transcript without leaving the composer,
        // so you can peek at history while typing. Home/End jump to top/bottom.
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
        // Any normal edit invalidates the tab-cycle stem (readline behavior).
        if (event.code != KeyCode::Tab && event.code != KeyCode::BackTab)
            model_->clearTabCompletion();
        if (model_->composer.handle_key(event)) return true;
        return false;
    }

    void draw(inkcell::Surface& surface) const override {
        if (layout::render_min_size_notice(surface)) return;
        auto p = layout::page(surface);

        chat::ChatSurfaceModel vm;
        vm.path = model_->breadcrumb();
        vm.provider = cfg_.provider;
        vm.model = cfg_.model;
        vm.sessionId = model_->activeSessionId;
        vm.status = model_->status;
        {
            // Compact mode chips for the elevated status bar.
            std::string mode;
            if (model_->showRaw) mode = "raw";
            else mode = model_->showThoughts ? "think" : "clean";
            mode += model_->truncateBodies ? " · trunc" : " · full";
            vm.mode = mode;
        }
        vm.running = model_->running;
        vm.failed = model_->failed;
        vm.inputFocused = model_->composer.focused && !model_->timelineFocus && model_->atRoot();
        vm.historyFocused = model_->timelineFocus || !model_->composer.focused;
        vm.showThoughts = model_->showThoughts;
        vm.showRaw = model_->showRaw;
        vm.pendingOps = model_->pendingOps;
        vm.actionCount = model_->actionCount;
        vm.resultCount = model_->resultCount;
        vm.tokenBytes = model_->tokenBytes;
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

        // Reserve transcript height for completion menu + footer (no sep line).
        int menuH = chat::completionMenuHeight(vm, p.w);
        model_->transcriptView.viewport_h = std::max(1, p.h - 6 - menuH);
        if (model_->transcriptView.stick_bottom) model_->transcriptView.scroll_to_end();
        else model_->transcriptView.clamp();

        chat::drawChatSurface(surface, p, vm);
        if (model_->askActive)
            chat::drawAskDialog(surface, p, model_->askDialog, model_->askInput.value,
                                model_->askMultiSelected);
        else if (model_->helpVisible)
            chat::drawHelpOverlay(surface, p);
        // Palette above help/ask chrome
        components::drawCmdPalette(surface, p, model_->cmdPalette);
    }

   private:
    void runPaletteAction(const std::string& id) {
        if (id == "nav.main") {
            model_->pendingRoute = "main";
            return;
        }
        if (id == "chat.thoughts") {
            model_->showThoughts = !model_->showThoughts;
            model_->rebuildViews();
            persistUiPrefs(*model_);
            return;
        }
        if (id == "chat.truncate") {
            model_->truncateBodies = !model_->truncateBodies;
            model_->rebuildViews();
            persistUiPrefs(*model_);
            return;
        }
        if (id == "chat.raw") {
            model_->showRaw = !model_->showRaw;
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
            model_->pendingRoute = "quit";
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
                    if (v.isString()) val = v.asString();
                    else if (v.isBool()) val = v.asBool() ? "true" : "false";
                    else if (v.isNumeric()) val = v.asString();
                    else {
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

    bool runSlashCommand() {
        const std::string command = model_->composer.value;
        if (command.empty() || command[0] != '/') return false;

        chat::ChatCommandContext ctx;
        ctx.manifestPath = cfg_.manifestPath;
        ctx.harnessPath = cfg_.harnessPath;
        ctx.systemPromptPath = cfg_.systemPromptPath;
        ctx.personaPath = cfg_.personaPath;
        ctx.toolCount = cfg_.toolCount;
        ctx.feedCount = cfg_.feedCount;
        ctx.relicCount = cfg_.relicCount;
        ctx.subAgentCount = cfg_.subAgentCount;
        auto result = chat::executeChatCommand(command, ctx);
        if (!result.handled) return false;

        model_->composer.value.clear();
        model_->composer.cursor = 0;
        if (result.quit) model_->pendingRoute = "quit";
        if (result.stopLoop) stopAgentLoop("slash");
        if (result.clearTranscript) model_->clearTranscript();
        bool prefsDirty = false;
        if (result.toggleThoughts) {
            model_->showThoughts = !model_->showThoughts;
            model_->rebuildViews();
            prefsDirty = true;
        }
        if (result.toggleTruncate) {
            model_->truncateBodies = !model_->truncateBodies;
            model_->rebuildViews();
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
            bridge_.cancelAsk();
        }
        if (model_->running) {
            model_->status = std::string("cancelling (") + reason + ")";
            g_running = false;
            model_->appendNotice("stop", {std::string("agent loop stop requested via ") + reason});
        } else if (!model_->askActive) {
            model_->appendNotice("stop", {"no active turn to stop"});
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
            model_->tabMatches = chat::completeChatCommand(prefix);
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
            model_->tabMatches = chat::completeChatCommand(lcp);
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
            model_->rebuildViews();
        } else if (action.is("shell.toggle_thoughts")) {
            model_->showThoughts = !model_->showThoughts;
            model_->rebuildViews();
        } else if (action.is("scroll.up")) model_->selectDelta(-1);
        else if (action.is("scroll.down")) model_->selectDelta(1);
        else if (action.is("history.enter")) model_->enterSelected();
        else if (action.is("history.back")) model_->goBack();
    }
};

}  // namespace cortex::mk3::ui::scenes
