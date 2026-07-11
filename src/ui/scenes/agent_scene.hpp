#pragma once
// Agent / Chat scene.
// Product chat surface: inkcell-native port of ReplSession composition
// (transcript + status + prompt), not the old experimental card UI.

#include "base_scene.hpp"
#include "src/ui/chat/chat_view.hpp"

namespace cortex::mk3::ui::scenes {

class AgentScene final : public BaseScene {
   public:
    using BaseScene::BaseScene;
    std::string name() const override { return "Chat"; }

    bool on_key(const inkcell::KeyEvent& event) override {
        using inkcell::KeyCode;

        if (model_->timelineFocus || !model_->atRoot() || !model_->composer.focused) {
            if (event.code == KeyCode::Escape) {
                if (model_->goBack()) return true;
                model_->focusComposer();
                return true;
            }
            if (event.code == KeyCode::ArrowUp || (event.code == KeyCode::Character && event.ch == 'k')) {
                model_->selectDelta(-1);
                return true;
            }
            if (event.code == KeyCode::ArrowDown || (event.code == KeyCode::Character && event.ch == 'j')) {
                model_->selectDelta(1);
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
        if (event.code == KeyCode::Enter) {
            model_->submitComposer();
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
        vm.sessionId = cfg_.sessionId;
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
        vm.transcript = model_->transcriptView.lines;
        vm.input = model_->composer.value;
        if (!model_->atRoot()) vm.hint = "Esc/Backspace back · j/k select · Enter drill · g refresh";
        else if (vm.historyFocused) vm.hint = "j/k select · Enter open sub-agent · i composer · t thoughts · r raw · q quit";
        else vm.hint = "Enter send · Esc history · /slash commands preserved · q quit";

        chat::drawChatSurface(surface, p, vm);
    }

   private:
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
