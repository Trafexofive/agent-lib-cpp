#pragma once
// Welcome options screen — only entry surface besides Agent/History.

#include "base_scene.hpp"

namespace cortex::mk3::ui::scenes {

class WelcomeScene final : public BaseScene {
   public:
    using BaseScene::BaseScene;
    std::string name() const override { return "Welcome"; }

    bool on_key(const inkcell::KeyEvent& event) override {
        if (event.code == inkcell::KeyCode::ArrowUp) {
            welcomeIndex_ = std::max(0, welcomeIndex_ - 1);
            return true;
        }
        if (event.code == inkcell::KeyCode::ArrowDown) {
            welcomeIndex_ = std::min(1, welcomeIndex_ + 1);
            return true;
        }
        if (event.code == inkcell::KeyCode::Enter) {
            model_->pendingRoute = (welcomeIndex_ == 0) ? "agent" : "quit";
            return true;
        }
        if (event.code == inkcell::KeyCode::Character && event.ch == '1') {
            model_->pendingRoute = "agent";
            return true;
        }
        if (event.code == inkcell::KeyCode::Character && (event.ch == 'q' || event.ch == 'Q')) {
            model_->pendingRoute = "quit";
            return true;
        }
        return false;
    }

    void draw(inkcell::Surface& surface) const override {
        using namespace inkcell;
        if (layout::render_min_size_notice(surface)) return;
        surface.clear(theme::base_bg());
        Rect p = layout::page(surface);

        surface.text({p.x, p.y}, "CORTEX MK3", theme::cyan());
        surface.text({p.x, p.y + 1}, "protocol-native agent control plane", theme::dim());
        layout::section_rule(surface, {p.x, p.y + 3}, p.w, "start");

        struct Opt {
            const char* key;
            const char* label;
            const char* hint;
        };
        Opt opts[] = {
            {"1", "Agent / History", "open the live agent timeline + composer"},
            {"q", "Quit", "leave without starting a session"},
        };

        int y = p.y + 5;
        for (int i = 0; i < 2; ++i) {
            bool sel = (welcomeIndex_ == i);
            layout::selected_row(surface, {p.x, y, p.w, 1},
                                 std::string(opts[i].key) + "  " + opts[i].label, sel);
            surface.text({p.x + 4, y + 1}, inkcell::text::truncate(opts[i].hint, p.w - 6), theme::dim());
            y += 3;
        }

        layout::section_rule(surface, {p.x, p.bottom() - 4}, p.w, "session");
        surface.text({p.x, p.bottom() - 3},
                     inkcell::text::truncate("provider  " + nonempty(cfg_.provider, "?") + "/" +
                                                 nonempty(cfg_.model, "default"),
                                             p.w),
                     theme::text());
        surface.text({p.x, p.bottom() - 2},
                     inkcell::text::truncate(cfg_.ephemeral ? "mode  ephemeral" : "mode  session", p.w),
                     theme::dim());
        surface.text({p.x, p.bottom() - 1}, "↑↓ select · Enter open · 1 agent · q quit", theme::dim());
    }

   private:
    int welcomeIndex_ = 0;

    void handle(const inkcell::Action& action) override {
        if (action.is("scroll.up")) welcomeIndex_ = std::max(0, welcomeIndex_ - 1);
        else if (action.is("scroll.down")) welcomeIndex_ = std::min(1, welcomeIndex_ + 1);
    }
};

}  // namespace cortex::mk3::ui::scenes
