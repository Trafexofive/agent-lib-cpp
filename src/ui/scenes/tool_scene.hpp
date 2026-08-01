#pragma once
// Tool scene — dedicated page for one tool manifest.
//
// Enter from Manifests (kind=tool) → form auto-filled from tool.yml examples
// → ↵ / r runs tools::dispatch on a worker → output + history.
//
// Keep this tight: match AgentScene patterns, no gold thrash.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <json/json.h>

#include "base_scene.hpp"
#include "src/core/manifest_loader.hpp"
#include "src/tools/dispatch.hpp"
#include "src/tools/registry.hpp"
#include "src/ui/chat/chat_view.hpp"
#include "src/ui/components/cmd_palette.hpp"
#include "src/ui/model/ui_prefs.hpp"

namespace cortex::mk3::ui::scenes {

class ToolScene final : public BaseScene {
   public:
    using BaseScene::BaseScene;
    std::string name() const override { return "Tool"; }

    void on_enter() override {
        BaseScene::on_enter();
        // ShellModel defaults composer.focused=true (chat). Tool page owns
        // keys itself — leave composer unfocused or ↵ never reaches runTool.
        model_->composer.focused = false;
        model_->composer.value.clear();
        model_->composer.cursor = 0;
        model_->timelineFocus = false;
        model_->helpVisible = false;

        if (model_->activeToolManifestPath.empty()) {
            model_->dashboard.flashNotice("tool · no manifest path");
            return;
        }
        // Reload when path or name changes (or first entry).
        if (loadedPath_ != model_->activeToolManifestPath) {
            tool_ = ManifestLoader::loadToolManifest(model_->activeToolManifestPath);
            if (tool_.name.empty()) {
                // Fallback: basename of parent dir (built-in/tools/<name>/tool.yml).
                tool_.name = model_->activeToolName;
            }
            if (model_->activeToolName.empty())
                model_->activeToolName = tool_.name;
            params_ = defaultsFromSchema(tool_);
            formFocus_ = 0;
            loadedPath_ = model_->activeToolManifestPath;
            // Fresh tool page — don't show the previous tool's last run.
            model_->lastToolRun = ShellModel::ToolRunRecord{};
        }
        model_->status = "ready";
        model_->dashboard.notice = "tool · " + tool_.name + " · ↵ run · e edit · m hub";
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

        // Esc: cancel busy → leave edit → hub.
        if (event.code == KeyCode::Escape) {
            if (model_->cmdPalette.open && !model_->cmdPalette.closing) {
                model_->cmdPalette.requestClose();
                model_->closeModalFocus("palette");
                return true;
            }
            if (model_->composer.focused) {
                model_->composer.focused = false;
                model_->composer.value.clear();
                return true;
            }
            if (model_->helpVisible) {
                model_->helpVisible = false;
                model_->closeModalFocus("help");
                return true;
            }
            if (model_->toolRunsBusy) {
                model_->toolCancelRequested = true;
                model_->dashboard.flashNotice("tool · cancel requested");
                return true;
            }
            model_->requestRoute(PendingRoute::Main);
            return true;
        }

        if (event.code == KeyCode::Backspace && !model_->composer.focused) {
            model_->requestRoute(PendingRoute::Main);
            return true;
        }
        if (event.code == KeyCode::Character && (event.ch == 'm' || event.ch == 'M') &&
            !model_->composer.focused) {
            model_->requestRoute(PendingRoute::Main);
            return true;
        }
        if (event.code == KeyCode::Character && event.ch == '?' && !model_->composer.focused) {
            model_->helpVisible = !model_->helpVisible;
            if (model_->helpVisible) model_->openModalFocus("help");
            else model_->closeModalFocus("help");
            return true;
        }

        // Field nav (form mode).
        if (!model_->composer.focused) {
            if (event.code == KeyCode::Character && (event.ch == 'j' || event.ch == 'J')) {
                int n = (int)formFields().size();
                if (n > 0) formFocus_ = std::min(formFocus_ + 1, n - 1);
                return true;
            }
            if (event.code == KeyCode::Character && (event.ch == 'k' || event.ch == 'K')) {
                formFocus_ = std::max(formFocus_ - 1, 0);
                return true;
            }
            // ↵ / r → run.
            if (event.code == KeyCode::Enter ||
                (event.code == KeyCode::Character && (event.ch == 'r' || event.ch == 'R'))) {
                if (model_->toolRunsBusy) {
                    model_->dashboard.flashNotice("tool · already running · Esc cancels");
                    return true;
                }
                runTool();
                return true;
            }
            // e → edit focused field.
            if (event.code == KeyCode::Character && (event.ch == 'e' || event.ch == 'E')) {
                auto fields = formFields();
                if (fields.empty()) {
                    model_->dashboard.flashNotice("tool · no input fields");
                    return true;
                }
                formFocus_ = std::max(0, std::min(formFocus_, (int)fields.size() - 1));
                model_->composer.value = currentFieldValue();
                model_->composer.cursor = (int)model_->composer.value.size();
                model_->composer.focused = true;
                return true;
            }
            // c → copy last output.
            if (event.code == KeyCode::Character && (event.ch == 'c' || event.ch == 'C')) {
                copyLastOutput();
                return true;
            }
            return false;
        }

        // Composer focused: edit field value.
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
        return false;
    }

    void draw(inkcell::Surface& surface) const override {
        if (layout::render_min_size_notice(surface)) return;
        auto p = layout::page(surface);
        surface.clear(theme::base_bg());
        views::topbar(surface, cfg_, *model_, name());

        int y = p.y + 4;
        const std::string title =
            tool_.name.empty() ? (model_->activeToolName.empty() ? "tool" : model_->activeToolName)
                               : tool_.name;
        surface.text({p.x, y++}, inkcell::text::truncate(title, p.w), theme::bright());
        // Identity strip — runtime/entry from tool.yml (gold card parity).
        {
            std::string meta = "TOOL";
            if (!tool_.runtime.empty()) meta += " · " + tool_.runtime;
            if (!tool_.entrypoint.empty()) meta += " · " + tool_.entrypoint;
            surface.text({p.x, y++}, inkcell::text::truncate(meta, p.w),
                         theme::kindAccent("tool", true));
        }
        if (!tool_.description.empty()) {
            // Prefer first ~6 lines of PE description so the page isn't a wall.
            int linesLeft = 6;
            for (const auto& line : chat::wrapWordsLossless(tool_.description, p.w)) {
                if (y >= p.y + p.h - 12 || linesLeft-- <= 0) break;
                surface.text({p.x, y++}, line, theme::text());
            }
        }
        if (!model_->activeToolManifestPath.empty() && y < p.y + p.h - 10) {
            surface.text({p.x, y++},
                         inkcell::text::truncate("path  " + model_->activeToolManifestPath, p.w),
                         theme::dim());
        }
        y++;

        // Status chip.
        const auto& r = model_->lastToolRun;
        std::string status;
        inkcell::Style st = theme::dim();
        if (model_->toolRunsBusy || r.running) {
            status = "◐ running…";
            st = theme::amber();
        } else if (!r.toolName.empty()) {
            if (r.success) {
                status = "● done · " + std::to_string((long)r.elapsedMs) + "ms";
                st = theme::green();
            } else {
                status = "✗ fail · " + std::to_string((long)r.elapsedMs) + "ms";
                st = theme::red();
            }
        } else {
            status = "○ idle · ↵ run";
        }
        surface.text({p.x, y++}, status, st);

        // INPUT form.
        y++;
        if (y < p.y + p.h - 4) {
            surface.text({p.x, y++}, "INPUT", theme::violet_soft());
            auto fields = formFields();
            if (fields.empty()) {
                surface.text({p.x, y++}, "  (no input_schema — run with empty params)",
                             theme::italic_dim());
            } else {
                for (int i = 0; i < (int)fields.size() && y < p.y + p.h - 6; ++i) {
                    const auto& f = fields[i];
                    // Never call asString() on non-string JSON — JsonCpp throws
                    // "Type is not convertible to string" and kills the TUI.
                    std::string val = "—";
                    if (params_.isMember(f.name) && !params_[f.name].isNull()) {
                        const auto& v = params_[f.name];
                        if (v.isString()) val = v.asString();
                        else if (v.isBool()) val = v.asBool() ? "true" : "false";
                        else if (v.isInt() || v.isUInt() || v.isInt64() || v.isUInt64())
                            val = std::to_string(v.asInt64());
                        else if (v.isDouble()) val = std::to_string(v.asDouble());
                        else {
                            Json::StreamWriterBuilder wb;
                            wb["indentation"] = "";
                            val = Json::writeString(wb, v);
                        }
                        if (val.empty()) val = "—";
                    }
                    std::string line;
                    auto style = theme::text();
                    if (model_->composer.focused && i == formFocus_) {
                        line = "▸ " + f.name + std::string(std::max(1, 14 - (int)f.name.size()), ' ') +
                               model_->composer.value + "▏";
                        style = theme::green();
                    } else {
                        std::string mark = (i == formFocus_) ? "▸ " : "  ";
                        line = mark + f.name +
                               std::string(std::max(1, 14 - (int)f.name.size()), ' ') + val;
                        if (i == formFocus_) style = theme::bright();
                    }
                    surface.text({p.x, y++}, inkcell::text::truncate(line, p.w), style);
                }
            }
            y++;
            surface.text({p.x, y++},
                         "↵/r run · e edit · j/k field · c copy · m hub · ? help",
                         theme::italic_dim());
        }

        // OUTPUT.
        const int outTop = y + 1;
        if (outTop < p.y + p.h - 1) {
            surface.text({p.x, outTop - 1}, "OUTPUT", theme::violet_soft());
            std::string out = r.output;
            if (!r.error.empty()) {
                if (!out.empty()) out = "[error] " + r.error + "\n" + out;
                else out = "[error] " + r.error;
            }
            if (out.size() > 4000) out = "…\n" + out.substr(out.size() - 4000);
            int oy = outTop;
            const std::string body =
                out.empty() ? std::string("(no run yet — press ↵)") : out;
            for (const auto& line : chat::wrapWordsLossless(body, p.w)) {
                if (oy >= p.y + p.h - 1) break;
                surface.text({p.x, oy++}, line, theme::text());
            }
        }

        if (model_->helpVisible) {
            // Minimal help overlay — reuses chat help chrome if available.
            surface.text({p.x, p.y + 2},
                         "TOOL KEYS · ↵/r run · e edit · j/k field · c copy · Esc cancel/back · m hub",
                         theme::bright());
        }
    }

   private:
    struct Field {
        std::string name;
        std::string type;
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
        const auto& props =
            schema.isMember("properties") ? schema["properties"] : Json::Value();
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
            f.required =
                std::find(required.begin(), required.end(), name) != required.end();
            out.push_back(std::move(f));
        }
        return out;
    }

    std::string currentFieldValue() const {
        auto fields = formFields();
        if (formFocus_ < 0 || formFocus_ >= (int)fields.size()) return "";
        const auto& name = fields[formFocus_].name;
        if (!params_.isMember(name)) return "";
        if (params_[name].isString()) return params_[name].asString();
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        return Json::writeString(wb, params_[name]);
    }

    void commitFieldEdit() {
        auto fields = formFields();
        if (formFocus_ < 0 || formFocus_ >= (int)fields.size()) return;
        const auto& f = fields[formFocus_];
        // Type-aware commit: bool / int / string.
        if (f.type == "boolean") {
            const std::string v = model_->composer.value;
            params_[f.name] = (v == "true" || v == "1" || v == "yes" || v == "on");
        } else if (f.type == "integer" || f.type == "number") {
            try {
                if (f.type == "integer")
                    params_[f.name] = std::stoi(model_->composer.value);
                else
                    params_[f.name] = std::stod(model_->composer.value);
            } catch (...) {
                params_[f.name] = model_->composer.value;
            }
        } else {
            params_[f.name] = model_->composer.value;
        }
        model_->dashboard.flashNotice("set " + f.name + " = " + model_->composer.value);
    }

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

    static std::string jsonCompact(const Json::Value& v) {
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        return Json::writeString(wb, v);
    }

    void runTool() {
        if (model_->toolRunsBusy) return;
        const std::string name =
            tool_.name.empty() ? model_->activeToolName : tool_.name;
        if (name.empty()) {
            model_->dashboard.flashNotice("tool · no name resolved");
            return;
        }
        model_->lastToolRun = ShellModel::ToolRunRecord{};
        model_->lastToolRun.toolName = name;
        model_->lastToolRun.paramsJson = jsonCompact(params_);
        model_->lastToolRun.running = true;
        model_->lastToolRun.timestampMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        model_->toolRunsBusy = true;
        model_->toolCancelRequested = false;
        model_->status = "tool running";
        model_->dashboard.flashNotice("running " + name + "…");

        // Capture by shared_ptr so detached worker is safe if scene is destroyed.
        auto model = model_;
        AgentBridge* bridge = &bridge_;
        Json::Value paramsCopy = params_;
        std::thread worker([model, bridge, name, paramsCopy]() {
            std::string raw;
            auto t0 = std::chrono::steady_clock::now();
            try {
                tools::registerDefaults();
                raw = tools::dispatch(name, paramsCopy);
            } catch (const std::exception& e) {
                raw = std::string("{\"success\":false,\"error\":\"") + e.what() + "\"}";
            }
            auto t1 = std::chrono::steady_clock::now();
            int64_t ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

            Json::Value parsed;
            Json::CharReaderBuilder rb;
            std::string errs;
            std::istringstream ss(raw);
            if (Json::parseFromStream(rb, ss, &parsed, &errs) && parsed.isObject()) {
                model->lastToolRun.success = parsed.get("success", false).asBool();
                // Prefer output, then result, then raw.
                if (parsed.isMember("output") && parsed["output"].isString())
                    model->lastToolRun.output = parsed["output"].asString();
                else if (parsed.isMember("result"))
                    model->lastToolRun.output = jsonCompact(parsed["result"]);
                else
                    model->lastToolRun.output = raw;
                model->lastToolRun.error = parsed.get("error", "").asString();
                // Some tools only set exit_code without success.
                if (!parsed.isMember("success") && parsed.isMember("exit_code"))
                    model->lastToolRun.success = (parsed["exit_code"].asInt() == 0);
            } else {
                model->lastToolRun.success = false;
                model->lastToolRun.output = raw;
                model->lastToolRun.error = "non-json response";
            }
            if (model->lastToolRun.output.empty() && !model->lastToolRun.error.empty())
                model->lastToolRun.output = "(no output)";
            model->lastToolRun.elapsedMs = ms;
            model->lastToolRun.running = false;
            model->toolHistory.push_back(model->lastToolRun);
            if (model->toolHistory.size() > 32)
                model->toolHistory.erase(model->toolHistory.begin());
            model->toolRunsBusy = false;
            model->status = model->lastToolRun.success ? "ready" : "tool failed";
            model->dashboard.flashNotice(
                name + (model->lastToolRun.success ? " ok" : " fail") + " · " +
                std::to_string(ms) + "ms");
            // Wake UI so the next frame drains + redraws immediately.
            if (bridge) bridge->publish(UiEvent::status(
                model->lastToolRun.success ? "tool ok" : "tool fail"));
        });
        worker.detach();
    }

    void copyLastOutput() {
        if (model_->lastToolRun.output.empty()) {
            model_->dashboard.flashNotice("tool · nothing to copy");
            return;
        }
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

    ToolSchema tool_;
    Json::Value params_ = Json::Value(Json::objectValue);
    int formFocus_ = 0;
    std::string loadedPath_;
};

}  // namespace cortex::mk3::ui::scenes
