#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "inkcell/scene.hpp"
#include "src/ui/model/inkcell_app_model.hpp"
#include "src/ui/views/shell_views.hpp"

namespace cortex::mk3::ui::scenes {

class BaseScene : public inkcell::Scene {
   public:
    BaseScene(InkcellAppConfig cfg, AgentBridge& bridge, std::shared_ptr<ShellModel> model)
        : cfg_(std::move(cfg)), bridge_(bridge), model_(std::move(model)) {}

    void on_enter() override { model_->routeTo(name()); }

    void update(inkcell::Tick, inkcell::Action action) override {
        model_->drain(bridge_);
        if (bridge_.askPending() && model_->askDialog.done()) {
            if (model_->askDialog.cancelled) bridge_.cancelAsk();
            else bridge_.completeAsk(model_->askDialog.results);
            model_->askActive = false;
            model_->closeModalFocus("ask");
            model_->status = model_->running ? "agent running" : model_->status;
        }
        model_->tickRoute();
        if (action.is("shell.toggle_raw")) {
            model_->showRaw = !model_->showRaw;
            model_->markProjFull();
            model_->rebuildViews();
        } else if (action.is("shell.toggle_thoughts")) {
            model_->showThoughts = !model_->showThoughts;
            model_->markProjFull();
            model_->rebuildViews();
        } else if (action.is("shell.focus_composer")) {
            model_->composer.focused = true;
        } else if (action.is("shell.focus_timeline")) {
            model_->composer.focused = false;
        }
        handle(action);
    }

   protected:
    InkcellAppConfig cfg_;
    AgentBridge& bridge_;
    std::shared_ptr<ShellModel> model_;

    virtual std::string name() const = 0;
    virtual void handle(const inkcell::Action&) {}

    inkcell::Rect bodyRect(inkcell::Surface& surface) const {
        auto p = layout::page(surface);
        return {p.x, p.y + 7 + model_->transitionInset(), p.w, std::max(1, p.h - 10 - model_->transitionInset())};
    }

    void drawCommon(inkcell::Surface& surface) const {
        surface.clear(theme::base_bg());
        views::topbar(surface, cfg_, *model_, name());
    }
};

}  // namespace cortex::mk3::ui::scenes
