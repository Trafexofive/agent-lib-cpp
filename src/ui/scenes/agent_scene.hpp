#pragma once
// Agent / Chat scene.
// Product chat surface: inkcell-native port of ReplSession composition
// (transcript + status + prompt), not the old experimental card UI.

#include "base_scene.hpp"
#include "src/ui/chat/chat_commands.hpp"
#include "src/ui/chat/chat_io.hpp"
#include "src/ui/chat/chat_view.hpp"

namespace cortex::mk3::ui::scenes {

class AgentScene final : public BaseScene {
   public:
    using BaseScene::BaseScene;
    std::string name() const override { return "Chat"; }

    bool on_key(const inkcell::KeyEvent& event) override {
        using inkcell::KeyCode;

        if (model_->askActive) return handleAskKey(event);
        if (model_->helpVisible) {
            if (event.code == KeyCode::Escape ||
                (event.code == KeyCode::Character && event.ch == '?'))
                model_->helpVisible = false;
            return true;
        }

        if (event.code == KeyCode::CtrlC) {
            if (model_->running) {
                model_->status = "cancelling";
                g_running = false;
            } else {
                model_->pendingRoute = "quit";
            }
            return true;
        }

        if (model_->timelineFocus || !model_->atRoot() || !model_->composer.focused) {
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
            completeSlashCommand();
            return true;
        }
        if (event.code == KeyCode::ArrowUp) {
            model_->historyPrevious();
            return true;
        }
        if (event.code == KeyCode::ArrowDown) {
            model_->historyNext();
            return true;
        }
        if (event.code == KeyCode::Enter) {
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
        if (model_->composer.handle_key(event)) return true;
        return false;
    }

    void draw(inkcell::Surface& surface) const override {
        if (layout::render_min_size_notice(surface)) return;
        auto p = layout::page(surface);

        model_->transcriptView.viewport_h = std::max(1, p.h - 7);
        if (model_->transcriptView.stick_bottom) model_->transcriptView.scroll_to_end();
        else model_->transcriptView.clamp();

        chat::ChatSurfaceModel vm;
        vm.path = model_->breadcrumb();
        vm.provider = cfg_.provider;
        vm.model = cfg_.model;
        vm.sessionId = model_->activeSessionId;
        vm.status = model_->status;
        vm.mode = model_->showRaw ? "RAW" : model_->showThoughts ? "FULL+THOUGHTS" : "FULL";
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
        vm.scrollOffset = model_->transcriptView.offset;
        vm.followBottom = model_->transcriptView.stick_bottom;
        vm.transcriptSource = &model_->transcriptView.lines;
        vm.transcriptVersion = model_->transcriptVersion;
        vm.transcriptCache = &model_->transcriptWrapCache;
        vm.input = model_->composer.value;
        vm.inputCursor = model_->composer.cursor;
        vm.agentName = model_->agentName;
        vm.scopeName = model_->atRoot() ? std::string() : model_->agentPath.back();
        if (model_->running) vm.hint = "Ctrl-C cancel · Esc history · PgUp/Dn scroll · t thoughts · r raw";
        else if (!model_->atRoot()) vm.hint = "↑↓ scroll · j/k select · Enter drill · Esc back · g refresh";
        else if (vm.historyFocused) vm.hint = "↑↓ scroll · j/k select · Enter open · PgUp/Dn · i composer · ? help · q quit";
        else vm.hint = "Enter send · ↑↓ history · PgUp/Dn scroll · Esc transcript · Tab commands";

        chat::drawChatSurface(surface, p, vm);
        if (model_->askActive)
            chat::drawAskDialog(surface, p, model_->askDialog, model_->askInput.value,
                                model_->askMultiSelected);
        else if (model_->helpVisible)
            chat::drawHelpOverlay(surface, p);
    }

   private:
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
        chat::completeNonInteractiveCards(model_->askDialog);
        if (model_->askDialog.done()) model_->askActive = false;
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
        if (result.clearTranscript) model_->clearTranscript();
        if (result.toggleThoughts) model_->showThoughts = !model_->showThoughts;
        if (result.toggleRaw) model_->showRaw = !model_->showRaw;
        if (result.toggleTheme) {
            if (result.themeName == "graphite") theme::set(theme::Variant::Graphite);
            else if (result.themeName == "neon") theme::set(theme::Variant::Neon);
            else theme::toggle();
            result.lines = {std::string("active theme: ") + theme::name()};
        }
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

    void completeSlashCommand() {
        if (model_->composer.value.empty() || model_->composer.value[0] != '/') return;
        size_t space = model_->composer.value.find(' ');
        std::string prefix = space == std::string::npos
                                 ? model_->composer.value
                                 : model_->composer.value.substr(0, space);
        auto matches = chat::completeChatCommand(prefix);
        if (matches.size() == 1) {
            model_->composer.value = matches.front() + " ";
            model_->composer.cursor = static_cast<int>(model_->composer.value.size());
        } else if (!matches.empty()) {
            model_->appendNotice("completions", matches);
        }
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
