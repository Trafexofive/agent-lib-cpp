#pragma once
// Relic scene — dedicated inspect page for one relic.yml.
// Gold chrome: identity, endpoints, health, path. Esc/m → hub.

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "base_scene.hpp"
#include "src/core/mini_yaml.hpp"
#include "src/ui/chat/chat_view.hpp"
#include "src/ui/components/cmd_palette.hpp"
#include "src/ui/model/ui_prefs.hpp"

namespace cortex::mk3::ui::scenes {

class RelicScene final : public BaseScene {
   public:
    using BaseScene::BaseScene;
    std::string name() const override { return "Relic"; }

    void on_enter() override {
        BaseScene::on_enter();
        model_->composer.focused = false;
        model_->timelineFocus = false;
        model_->helpVisible = false;
        reload();
        model_->status = "ready";
        model_->dashboard.notice =
            "relic · " + (name_.empty() ? "?" : name_) + " · m hub · Esc back";
    }

    bool on_key(const inkcell::KeyEvent& event) override {
        using inkcell::KeyCode;
        if (model_->cmdPalette.open && !model_->cmdPalette.closing) {
            std::string action;
            if (components::handleCmdPaletteKey(model_->cmdPalette, event, &action)) {
                if (!action.empty()) {
                    if (action == "nav.main") model_->requestRoute(PendingRoute::Main);
                    else if (action == "sys.quit") model_->requestRoute(PendingRoute::Quit);
                }
                return true;
            }
        }
        if (event.code == KeyCode::Character && event.ctrl() &&
            (event.ch == 'p' || event.ch == 'P')) {
            model_->cmdPalette.toggle(components::hubCommands());
            return true;
        }
        if (event.code == KeyCode::Escape || event.code == KeyCode::Backspace ||
            (event.code == KeyCode::Character &&
             (event.ch == 'm' || event.ch == 'M'))) {
            model_->requestRoute(PendingRoute::Main);
            return true;
        }
        if (event.code == KeyCode::Enter ||
            (event.code == KeyCode::Character && (event.ch == 'r' || event.ch == 'R'))) {
            reload();
            model_->dashboard.flashNotice("relic · refreshed");
            return true;
        }
        if (event.code == KeyCode::Character && (event.ch == 'j' || event.ch == 'J')) {
            if (!endpoints_.empty())
                epFocus_ = std::min(epFocus_ + 1, (int)endpoints_.size() - 1);
            return true;
        }
        if (event.code == KeyCode::Character && (event.ch == 'k' || event.ch == 'K')) {
            epFocus_ = std::max(epFocus_ - 1, 0);
            return true;
        }
        return false;
    }

    void draw(inkcell::Surface& surface) const override {
        if (layout::render_min_size_notice(surface)) return;
        auto p = layout::page(surface);
        surface.clear(theme::base_bg());
        views::topbar(surface, cfg_, *model_, name());

        int y = p.y + 4;
        std::string title = name_.empty() ? "relic" : name_;
        if (!version_.empty()) title += "  v" + version_;
        surface.text({p.x, y++}, inkcell::text::truncate(title, p.w), theme::bright());

        std::string meta = "RELIC";
        if (!category_.empty()) meta += " · " + category_;
        surface.text({p.x, y++}, inkcell::text::truncate(meta, p.w), theme::kindAccent("relic", true));

        if (!summary_.empty()) {
            for (const auto& line : chat::wrapWordsLossless(summary_, p.w)) {
                if (y >= p.y + p.h - 8) break;
                surface.text({p.x, y++}, line, theme::text());
            }
        }
        y++;

        // Health chip
        auto st = endpoints_.empty() ? theme::amber() : theme::green();
        surface.text({p.x, y++},
                     endpoints_.empty() ? "○ no endpoints parsed"
                                        : ("● " + std::to_string(endpoints_.size()) +
                                           " endpoint(s)"),
                     st);
        y++;

        surface.text({p.x, y++}, "ENDPOINTS", theme::violet_soft());
        if (endpoints_.empty()) {
            surface.text({p.x, y++}, "  (none in relic.yml)", theme::italic_dim());
        } else {
            for (int i = 0; i < (int)endpoints_.size() && y < p.y + p.h - 4; ++i) {
                bool sel = (i == epFocus_);
                std::string line = (sel ? "› " : "  ") + endpoints_[static_cast<size_t>(i)];
                surface.text({p.x, y++}, inkcell::text::truncate(line, p.w),
                             sel ? theme::bright() : theme::text());
            }
        }

        if (y < p.y + p.h - 3) {
            y++;
            if (!path_.empty() && y < p.y + p.h - 2)
                surface.text({p.x, y++},
                             inkcell::text::truncate("path  " + path_, p.w), theme::dim());
            if (y < p.y + p.h - 1)
                surface.text({p.x, y}, "j/k select · ↵ refresh · m hub", theme::italic_dim());
        }

        if (model_->cmdPalette.open || model_->cmdPalette.closing)
            components::drawCmdPalette(surface, surface.bounds(), model_->cmdPalette);
    }

   private:
    void reload() {
        path_ = model_->activeRelicManifestPath;
        name_ = model_->activeRelicName;
        endpoints_.clear();
        summary_.clear();
        version_.clear();
        category_.clear();
        epFocus_ = 0;
        if (path_.empty()) return;

        std::ifstream in(path_);
        if (!in) return;
        std::ostringstream ss;
        ss << in.rdbuf();
        auto root = ManifestYaml::parse(ss.str());
        std::string n = ManifestYaml::get(root, "name");
        if (!n.empty()) name_ = n;
        version_ = ManifestYaml::get(root, "version");
        summary_ = ManifestYaml::get(root, "summary");
        if (summary_.empty()) summary_ = ManifestYaml::get(root, "description");
        category_ = ManifestYaml::get(root, "category");
        if (category_.empty()) category_ = "relic";

        auto* eps = ManifestYaml::find(root, "endpoints");
        if (eps) {
            for (const auto& child : eps->children) {
                std::string ep = child.value;
                if (ep.empty()) ep = ManifestYaml::get(child, "name");
                if (ep.empty()) ep = ManifestYaml::get(child, "url");
                if (ep.empty() && !child.key.empty()) ep = child.key;
                if (!ep.empty()) endpoints_.push_back(ep);
            }
        }
        // Mirror into dashboard cache for cards
        auto& r = model_->dashboard.relicRun;
        r.relicName = name_;
        r.endpoints = endpoints_;
        r.endpoint = endpoints_.empty() ? std::string() : endpoints_.front();
        r.healthy = !endpoints_.empty();
        r.output = std::to_string(endpoints_.size()) + " endpoint(s)";
    }

    std::string path_;
    std::string name_;
    std::string version_;
    std::string summary_;
    std::string category_;
    std::vector<std::string> endpoints_;
    int epFocus_ = 0;
};

}  // namespace cortex::mk3::ui::scenes
