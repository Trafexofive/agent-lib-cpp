#pragma once
// Tool scene — dedicated page for one tool manifest.
// Mirrors AgentScene's structure: BaseScene + on_enter/on_key/draw.
// Loads tool.yml on entry via ManifestLoader::loadToolManifest, builds
// an editable input form from input_schema, runs the tool on ↵, shows
// streaming output, and keeps a per-tool run history in ShellModel.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "base_scene.hpp"
#include "src/core/manifest_loader.hpp"
#include "src/tools/dispatch.hpp"
#include "src/tools/registry.hpp"
#include "src/ui/chat/chat_view.hpp"
#include "src/ui/model/ui_prefs.hpp"

namespace cortex::mk3::ui::scenes {

class ToolScene final : public BaseScene {
   public:
    using BaseScene::BaseScene;
    std::string name() const override { return "Tool"; }

    void on_enter() override {
        BaseScene::on_enter();
        if (model_->activeToolManifestPath.empty()) {
            // No manifest set — pick the first tool from the hub as a
            // sensible default so /tool route doesn't open blank.
            return;
        }
        if (tool_.name != model_->activeToolName) {
            tool_ = ManifestLoader::loadToolManifest(model_->activeToolManifestPath);
            params_ = defaultsFromSchema(tool_);
            lastError_.clear();
            formFocus_ = 0;
        }
        model_->status = "ready";
    }

    bool on_key(const inkcell::KeyEvent& event) override {
        using inkcell::KeyCode;
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
            return true;
        }
        // Esc ladder: dismiss overlays, focus timeline, then leave scene.
        if (event.code == KeyCode::Escape) {
            if (model_->cmdPalette.open && !model_->cmdPalette.closing) {
                model_->cmdPalette.requestClose();
                model_->closeModalFocus("palette");
                return true;
            }
            // Cancel a live run if any.
            if (model_->toolRunsBusy) {
                model_->toolCancelRequested = true;
                return true;
            }
            model_->requestRoute(PendingRoute::Main);
            return true;
        }
        if (event.code == KeyCode::Backspace &&
            !model_->cmdPalette.open && !model_->composer.focused) {
            model_->requestRoute(PendingRoute::Main);
            return true;
        }
        if (event.code == KeyCode::Character && (event.ch == 'm' || event.ch == 'M')) {
            model_->requestRoute(PendingRoute::Main);
            return true;
        }
        if (event.code == KeyCode::Character && event.ch == '?' &&
            !model_->composer.focused) {
            model_->helpVisible = !model_->helpVisible;
            if (model_->helpVisible) model_->openModalFocus("help");
            else model_->closeModalFocus("help");
            return true;
        }
        if (event.code == KeyCode::Character && (event.ch == 'j' || event.ch == 'J') &&
            !model_->composer.focused) {
            formFocus_ = std::min(formFocus_ + 1, (int)formFields().size() - 1);
            return true;
        }
        if (event.code == KeyCode::Character && (event.ch == 'k' || event.ch == 'K') &&
            !model_->composer.focused) {
            formFocus_ = std::max(formFocus_ - 1, 0);
            return true;
        }
        // ↵ run with current params. While running, ↵ is a no-op (worker busy).
        if (event.code == KeyCode::Enter && !model_->composer.focused) {
            if (model_->toolRunsBusy) {
                model_->dashboard.flashNotice("tool · already running · Esc cancels");
                return true;
            }
            runTool();
            return true;
        }
        // r re-run with last params (or defaults if no run yet)
        if (event.code == KeyCode::Character && (event.ch == 'r' || event.ch == 'R') &&
            !model_->composer.focused) {
            if (model_->toolRunsBusy) {
                model_->dashboard.flashNotice("tool · already running");
                return true;
            }
            runTool();
            return true;
        }
        // c copy last output to clipboard file
        if (event.code == KeyCode::Character && (event.ch == 'c' || event.ch == 'C') &&
            !model_->composer.focused) {
            copyLastOutput();
            return true;
        }
        // e enter param edit mode for focused field
        if (event.code == KeyCode::Character && (event.ch == 'e' || event.ch == 'E') &&
            !model_->composer.focused) {
            model_->composer.focused = true;
            model_->composer.value = currentFieldValue();
            model_->composer.cursor = (int)model_->composer.value.size();
            return true;
        }
        // Composer focused: capture chars / backspace / enter (commit)
        if (model_->composer.focused) {
            if (event.code == KeyCode::Escape) {
                model_->composer.focused = false;
                model_->composer.value.clear();
                return true;
            }
            if (event.code == KeyCode::Enter) {
                commitFieldEdit();
                model_->composer.focused = false;
                return true;
            }
            if (model_->composer.handle_key(event)) return true;
        }
        return false;
    }

    void draw(inkcell::Surface& surface) const override {
        if (layout::render_min_size_notice(surface)) return;
        auto p = layout::page(surface);
        surface.clear(theme::base_bg());
        views::topbar(surface, cfg_, *model_, name());

        // ── Header: tool name + status chip + last-run summary ──
        int y = p.y + 4;
        std::string title = tool_.name.empty() ? std::string("tool") : tool_.name;
        surface.text({p.x, y++}, inkcell::text::truncate(title, p.w),
                     theme::bright());
        if (!tool_.description.empty()) {
            for (const auto& line : chat::wrapWordsLossless(tool_.description, p.w)) {
                if (y >= p.y + p.h - 8) break;
                surface.text({p.x, y++}, line, theme::text());
            }
        }
        y++;

        // ── Status chip ──
        std::string status;
        if (model_->toolRunsBusy) {
            status = "◐ running…";
        } else if (!model_->lastToolRun.toolName.empty()) {
            status = model_->lastToolRun.success
                         ? ("● done · " + std::to_string((long)model_->lastToolRun.elapsedMs) + "ms")
                         : "✗ fail";
        } else {
            status = "○ idle";
        }
        surface.text({p.x, y++}, status,
                     model_->toolRunsBusy       ? theme::amber()
                     : model_->lastToolRun.success ? theme::green()
                                                  : theme::dim());

        // ── Input form ──
        y++;
        if (y < p.y + p.h - 4) {
            surface.text({p.x, y++}, "INPUT", theme::violet_soft());
            auto fields = formFields();
            for (int i = 0; i < (int)fields.size() && y < p.y + p.h - 4; ++i) {
                const auto& f = fields[i];
                std::string val = params_.get(f.name, "").asString();
                if (val.empty()) val = "—";
                std::string line = "  " + f.name + std::string(std::max(0, 14 - (int)f.name.size()), ' ') + " " + val;
                std::string prefix = (i == formFocus_) ? "▸ " : "  ";
                auto style = (i == formFocus_) ? theme::bright() : theme::text();
                if (model_->composer.focused && i == formFocus_) {
                    line = "  " + f.name + std::string(std::max(0, 14 - (int)f.name.size()), ' ') + " " +
                           model_->composer.value + "▏";
                    style = theme::green();
                }
                surface.text({p.x, y++}, prefix + line, style);
            }
            y++;
            surface.text({p.x, y++},
                         "↵ run · e edit · j/k move · c copy · r re-run · m back",
                         theme::italic_dim());
        }

        // ── Output area ──
        const int outTop = y + 1;
        if (outTop < p.y + p.h - 2) {
            surface.text({p.x, outTop - 1}, "OUTPUT",
                         theme::violet_soft());
            std::string out = model_->lastToolRun.output;
            if (!model_->lastToolRun.error.empty()) {
                out = "[error] " + model_->lastToolRun.error + "\n" + out;
            }
            if (out.size() > 4000) out = "…\n" + out.substr(out.size() - 4000);
            int oy = outTop;
            for (const auto& line : chat::wrapWordsLossless(out.empty() ? "(no run yet — press ↵)" : out, p.w)) {
                if (oy >= p.y + p.h - 1) break;
                surface.text({p.x, oy++}, line, theme::text());
            }
        }
    }

   private:
    // ── Form field extraction from input_schema ──
    struct Field {
        std::string name;
        std::string type;     // string / number / boolean
        std::string desc;
        bool required = false;
    };
    std::vector<Field> formFields() const {
        std::vector<Field> out;
        if (tool_.inputSchema.empty()) return out;
        Json::Value schema;
        Json::CharReaderBuilder rb;
        std::string errs;
        std::istringstream ss(tool_.inputSchema);
        if (!Json::parseFromStream(rb, ss, &schema, &errs) || !schema.isObject()) return out;
        const auto& props = schema.isMember("properties") ? schema["properties"] : Json::Value();
        std::vector<std::string> required;
        if (schema.isMember("required") && schema["required"].isArray())
            for (const auto& v : schema["required"]) required.push_back(v.asString());
        if (!props.isObject()) return out;
        for (const auto& name : props.getMemberNames()) {
            const auto& p = props[name];
            Field f;
            f.name = name;
            f.type = p.get("type", "string").asString();
            f.desc = p.get("description", "").asString();
            f.required = std::find(required.begin(), required.end(), name) != required.end();
            out.push_back(std::move(f));
        }
        return out;
    }

    std::string currentFieldValue() const {
        auto fields = formFields();
        if (formFocus_ < 0 || formFocus_ >= (int)fields.size()) return "";
        return params_.get(fields[formFocus_].name, "").asString();
    }

    void commitFieldEdit() {
        auto fields = formFields();
        if (formFocus_ < 0 || formFocus_ >= (int)fields.size()) return;
        params_[fields[formFocus_].name] = model_->composer.value;
        model_->dashboard.flashNotice("set " + fields[formFocus_].name + " = " + model_->composer.value);
    }

    // Pre-fill params from first example (if any), else empty object.
    Json::Value defaultsFromSchema(const ToolSchema& t) {
        Json::Value empty(Json::objectValue);
        if (t.examples.empty()) return empty;
        Json::Value arr;
        Json::CharReaderBuilder rb;
        std::string errs;
        std::istringstream ss(t.examples);
        if (!Json::parseFromStream(rb, ss, &arr, &errs) || !arr.isArray() || arr.empty())
            return empty;
        const auto& ex = arr[0];
        if (!ex.isMember("params")) return empty;
        return ex["params"];
    }

    void runTool() {
        if (model_->toolRunsBusy) return;
        const std::string name = tool_.name.empty() ? model_->activeToolName : tool_.name;
        if (name.empty()) {
            model_->dashboard.flashNotice("tool · no name resolved");
            return;
        }
        model_->lastToolRun = ShellModel::ToolRunRecord{};
        model_->lastToolRun.toolName = name;
        model_->lastToolRun.paramsJson = Json::FastWriter().write(params_);
        model_->lastToolRun.running = true;
        model_->lastToolRun.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch()).count();
        model_->toolRunsBusy = true;
        model_->toolCancelRequested = false;
        model_->status = "tool running";

        Json::Value paramsCopy = params_;
        std::thread worker([this, name, paramsCopy]() {
            std::string raw;
            auto t0 = std::chrono::steady_clock::now();
            try {
                tools::registerDefaults();
                raw = tools::dispatch(name, paramsCopy);
            } catch (const std::exception& e) {
                raw = std::string("{\"success\":false,\"error\":\"") + e.what() + "\"}";
            }
            auto t1 = std::chrono::steady_clock::now();
            int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            Json::Value parsed;
            Json::CharReaderBuilder rb;
            std::string errs;
            std::istringstream ss(raw);
            if (Json::parseFromStream(rb, ss, &parsed, &errs) && parsed.isObject()) {
                model_->lastToolRun.success = parsed.get("success", false).asBool();
                model_->lastToolRun.output = parsed.get("output", "").asString();
                model_->lastToolRun.error = parsed.get("error", "").asString();
            } else {
                model_->lastToolRun.success = false;
                model_->lastToolRun.output = raw;
                model_->lastToolRun.error = "non-json response";
            }
            if (model_->lastToolRun.output.empty() && !model_->lastToolRun.error.empty())
                model_->lastToolRun.output = "(no output)";
            model_->lastToolRun.elapsedMs = ms;
            model_->lastToolRun.running = false;
            // Append to history (FIFO 32-entry cap).
            model_->toolHistory.push_back(model_->lastToolRun);
            if (model_->toolHistory.size() > 32)
                model_->toolHistory.erase(model_->toolHistory.begin());
            model_->toolRunsBusy = false;
            model_->status = model_->lastToolRun.success ? "ready" : "tool failed";
            model_->dashboard.flashNotice(name +
                (model_->lastToolRun.success ? " ok" : " fail") +
                " · " + std::to_string(ms) + "ms");
        });
        worker.detach();
    }

    void copyLastOutput() {
        if (model_->lastToolRun.output.empty()) {
            model_->dashboard.flashNotice("tool · nothing to copy");
            return;
        }
        // /tmp/mk3-tool-<name>-<ts>.out
        auto path = "/tmp/mk3-tool-" + model_->lastToolRun.toolName + "-" +
                    std::to_string(model_->lastToolRun.timestampMs) + ".out";
        std::ofstream f(path);
        f << model_->lastToolRun.output;
        if (!model_->lastToolRun.error.empty()) f << "\n[error] " << model_->lastToolRun.error;
        model_->dashboard.flashNotice("copied → " + path);
    }

    void runPaletteAction(const std::string& id) {
        if (id == "nav.main") {
            model_->requestRoute(PendingRoute::Main);
            return;
        }
        if (id == "sys.quit") {
            model_->requestRoute(PendingRoute::Quit);
            return;
        }
        if (id == "act.theme") {
            theme::toggle();
            persistUiPrefs(*model_);
            return;
        }
    }

    // Cached tool manifest (loaded on first on_enter for the path)
    ToolSchema tool_;
    Json::Value params_ = Json::Value(Json::objectValue);
    int formFocus_ = 0;
    std::string lastError_;
};

}  // namespace cortex::mk3::ui::scenes
