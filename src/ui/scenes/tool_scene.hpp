#pragma once
// Tool scene v2 — dedicated page for one tool manifest.
//
// Enter from Manifests (kind=tool) → schema-driven input form auto-filled from
// tool.yml examples → ↵ / r runs tools::dispatch on a worker. A focused app
// surface: two panes (INPUT form | OUTPUT + run history), a live status strip
// (spinner · elapsed · params), scrollback, and a run-history browser so any
// past run can be reviewed, copied, or re-run (params frozen per run).
//
// dispatch() is synchronous — output lands in one shot when the worker returns,
// so the spinner + elapsed are the honest "working" signal. Esc does NOT claim
// an interrupt the sync dispatcher can't honor.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include <json/json.h>

#include "inkcell/widgets/scroll_view.hpp"

#include "base_scene.hpp"
#include "src/core/manifest_loader.hpp"
#include "src/tools/dispatch.hpp"
#include "src/tools/registry.hpp"
#include "src/ui/chat/chat_io.hpp"
#include "src/ui/chat/chat_view.hpp"
#include "src/ui/components/chrome.hpp"
#include "src/ui/components/cmd_palette.hpp"
#include "src/ui/model/ui_prefs.hpp"

namespace cortex::mk3::ui::scenes {

class ToolScene final : public BaseScene {
   public:
    using BaseScene::BaseScene;
    std::string name() const override { return "Tool"; }

    void on_enter() override {
        BaseScene::on_enter();
        // The ShellModel defaults composer.focused=true (chat). Tool page owns
        // keys — leave composer unfocused or Enter never reaches run.
        model_->composer.focused = false;
        model_->composer.value.clear();
        model_->composer.cursor = 0;
        model_->timelineFocus = false;
        model_->helpVisible = false;
        outFocused_ = false;
        histIdx_ = -1;
        formScroll_ = 0;
        outView_ = inkcell::widgets::ScrollViewState{};

        if (model_->activeToolManifestPath.empty()) {
            model_->dashboard.flashNotice("tool · no manifest path");
            return;
        }
        if (loadedPath_ != model_->activeToolManifestPath) {
            tool_ = ManifestLoader::loadToolManifest(model_->activeToolManifestPath);
            if (tool_.name.empty())  // basename of parent dir (built-in/tools/name/tool.yml)
                tool_.name = model_->activeToolName;
            if (model_->activeToolName.empty())
                model_->activeToolName = tool_.name;
            params_ = defaultsFromSchema(tool_);
            formFocus_ = 0;
            loadedPath_ = model_->activeToolManifestPath;
            model_->lastToolRun = ShellModel::ToolRunRecord{};
        }
        model_->status = "ready";
        model_->dashboard.notice = "tool · " + tool_.name + " · e edit · ↵ run · m hub";
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

        // Esc: cancel edit → blur modal → hub.
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
        // d toggles the full description.
        if (event.code == KeyCode::Character && (event.ch == 'd' || event.ch == 'D') &&
            !model_->composer.focused && !tool_.description.empty()) {
            descExpanded_ = !descExpanded_;
            model_->dashboard.flashNotice(descExpanded_ ? "tool · description expanded"
                                                        : "tool · description collapsed");
            return true;
        }

        // Composer focused: edit the current field value.
        if (model_->composer.focused) {
            if (event.code == KeyCode::Escape) {
                model_->composer.focused = false;
                return true;
            }
            if (event.code == KeyCode::Enter) {
                commitFieldEdit();
                model_->composer.focused = false;
                return true;
            }
            return model_->composer.handle_key(event);
        }

        // History browsing (from either pane).
        if (event.code == KeyCode::Character && event.ch == '[') {
            browseHistory(-1);
            return true;
        }
        if (event.code == KeyCode::Character && event.ch == ']') {
            browseHistory(+1);
            return true;
        }
        if (event.code == KeyCode::Character && (event.ch == 'g' || event.ch == 'G') &&
            !model_->toolRunsBusy) {
            histIdx_ = -1;
            outView_.scroll_to_end();
            model_->dashboard.flashNotice("tool · latest run");
            return true;
        }
        // Load a browsed run's params into the form for re-run.
        if (event.code == KeyCode::Character && event.ch == 'o' && !model_->toolRunsBusy &&
            histIdx_ >= 0) {
            loadViewedParams();
            return true;
        }

        // Copy viewed output (from either pane).
        if (event.code == KeyCode::Character && (event.ch == 'c' || event.ch == 'C')) {
            copyViewedOutput();
            return true;
        }
        // Run (from either pane).
        if (event.code == KeyCode::Enter ||
            (event.code == KeyCode::Character && (event.ch == 'r' || event.ch == 'R'))) {
            if (model_->toolRunsBusy)
                model_->dashboard.flashNotice("tool · already running");
            else
                runTool();
            return true;
        }

        // Output pane focus: scroll owns j/k + paging.
        if (outFocused_) {
            if (event.code == KeyCode::Character && (event.ch == 'j' || event.ch == 'J')) {
                outView_.scroll_by(1);
                return true;
            }
            if (event.code == KeyCode::Character && (event.ch == 'k' || event.ch == 'K')) {
                outView_.scroll_by(-1);
                return true;
            }
            if (event.code == KeyCode::PageUp) {
                outView_.scroll_by(-std::max(1, outView_.viewport_h / 2));
                return true;
            }
            if (event.code == KeyCode::PageDown) {
                outView_.scroll_by(std::max(1, outView_.viewport_h / 2));
                return true;
            }
            if (event.code == KeyCode::Home) {
                outView_.scroll_to_start();
                return true;
            }
            if (event.code == KeyCode::End) {
                outView_.scroll_to_end();
                return true;
            }
        }

        // Tab / Shift-Tab toggles form <-> output focus.
        if (event.code == KeyCode::Tab || event.code == KeyCode::BackTab) {
            outFocused_ = !outFocused_;
            model_->dashboard.flashNotice(!outFocused_ ? "form · fields j/k · e edit · space toggle"
                                                       : "output · j/k scroll · ↵ run · o load");
            return true;
        }

        // ── Form field navigation (form focus) ─────────────────────────
        if (event.code == KeyCode::Character && (event.ch == 'j' || event.ch == 'J')) {
            moveFocus(+1);
            return true;
        }
        if (event.code == KeyCode::Character && (event.ch == 'k' || event.ch == 'K')) {
            moveFocus(-1);
            return true;
        }
        // e → edit (bool/enum inline; scalar/string via composer).
        if (event.code == KeyCode::Character && (event.ch == 'e' || event.ch == 'E')) {
            beginFieldEdit();
            return true;
        }
        if (event.code == KeyCode::Character && event.ch == ' ') {
            if (event.ctrl()) return false;  // don't steal Ctrl-Space from the field editor
            cycleFocusedInline();
            return true;
        }
        return false;
    }

    void draw(inkcell::Surface& surface) const override {
        if (layout::render_min_size_notice(surface)) return;
        auto page = layout::page(surface);
        surface.clear(theme::base_bg());

        const int left = page.x;
        const int right = page.x + page.w;  // exclusive
        const int bottom = page.y + page.h;  // exclusive (footer on bottom-1)

        // ── Clean app bar (flat, no dashed rules) — matches MainScene ──
        std::string title =
            tool_.name.empty() ? (model_->activeToolName.empty() ? "tool" : model_->activeToolName)
                               : tool_.name;
        auto barBg = theme::panel_2();
        surface.fill({page.x, page.y, page.w, 2}, " ", barBg);
        auto onBar = [barBg](inkcell::Style st) { return st.with_bg(barBg.bg); };
        surface.text({page.x, page.y}, "CORTEX ", onBar(theme::bright()));
        surface.text({page.x + 7, page.y}, "MK3", onBar(theme::cyan()));
        std::string sec = "  ·  TOOL  ·  " + title;
        surface.text({page.x + 11, page.y},
                     inkcell::text::truncate(sec, std::max(1, page.w - 11)),
                     onBar(theme::italic_accent()));
        std::string ident = nonempty(cfg_.provider, "provider") + "/" +
                            nonempty(cfg_.model, "default");
        int iw = inkcell::text::display_width(ident);
        surface.text({std::max(page.x + 11, right - iw - 1), page.y},
                     inkcell::text::truncate(ident, page.w - 11), onBar(theme::dim()));
        std::string sub = model_->dashboard.notice.empty()
                              ? (model_->toolRunsBusy ? "running " + title
                                                      : "manifest · tool")
                              : model_->dashboard.notice;
        surface.text({page.x, page.y + 1}, inkcell::text::truncate(sub, page.w),
                     onBar(theme::dim()));

        int y = page.y + 3;  // after the 2-row bar + one gap row

        // ── Tool identity (3 tight rows; no PE wall) ──────────────────
        surface.text({left, y}, inkcell::text::truncate(title, std::max(20, page.w - 18)),
                     theme::bright());
        auto pill = statusPill();
        if (!pill.first.empty()) {
            int pw = inkcell::text::display_width(pill.first);
            surface.text({std::max(left, right - pw - 1), y},
                         inkcell::text::truncate(pill.first, page.w), pill.second);
        }
        if (y < bottom) ++y;

        // meta row: kind chip + runtime·entry, path right-aligned
        std::string meta;
        if (!tool_.runtime.empty()) meta += tool_.runtime;
        if (!tool_.entrypoint.empty()) {
            if (!meta.empty()) meta += " · ";
            meta += tool_.entrypoint;
        }
        int mx = left;
        components::kindChip(surface, left, y, "tool", true);
        mx = left + 6;
        if (!meta.empty())
            surface.text({mx, y}, inkcell::text::truncate(meta, std::max(0, right - mx - 12)),
                         theme::dim());
        std::string spath = shortPath();
        if (!spath.empty()) {
            int pw = inkcell::text::display_width(spath);
            surface.text({std::max(left, right - pw - 1), y},
                         inkcell::text::truncate(spath, std::max(1, page.w / 2)),
                         theme::footer_dim());
        }
        if (y < bottom) ++y;

        // Description: one line by default; d expands to up to 5.
        if (!tool_.description.empty() && y < bottom - 6) {
            auto descLines = chat::wrapWordsLossless(tool_.description, page.w);
            if (descExpanded_) {
                int shown = 0;
                for (const auto& line : descLines) {
                    if (shown >= 5) break;
                    surface.text({left, y}, line, theme::text());
                    ++y;
                    ++shown;
                }
            } else {
                std::string sum = summaryOf(tool_.description);
                auto sumLines = chat::wrapWordsLossless(sum, page.w);
                std::string line1 = sumLines.empty() ? "" : sumLines[0];
                size_t hidden = descLines.size() > 1 ? descLines.size() - 1 : 0;
                if (hidden > 0) {
                    std::string hint = "  ⋯ +" + std::to_string(hidden) + " d";
                    int hintW = inkcell::text::display_width(hint);
                    int avail = std::max(8, page.w - hintW - 1);
                    std::string cut = inkcell::text::truncate(line1, avail);
                    surface.text({left, y}, cut, theme::text());
                    surface.text({left + inkcell::text::display_width(cut) + 1, y}, hint,
                                 theme::italic_dim());
                } else {
                    surface.text({left, y}, line1, theme::text());
                }
                ++y;
            }
        }

        // ── Panes (flat cards; gap between, no divider glyphs) ──
        const bool wide = page.w >= 104;
        const int paneTop = y + 1;
        const int paneBot = bottom - 2;  // 1-row gap above the footer
        inkcell::Rect formPane, outPane;
        if (wide) {
            int split = std::max<int>(40, std::min(page.w * 2 / 5, page.w - 58));
            int avail = paneBot - paneTop;
            // Form hugs its content; output absorbs the slack for scrollback.
            int wantH = (int)formFields().size() + 3;  // header + rows + desc + params
            int formH = std::max(4, std::min(wantH, avail - 3));
            formPane = {left, paneTop, split, formH};
            outPane = {left + split + 2, paneTop, page.w - split - 2, avail};
        } else {
            int inH = std::max(4, std::min<int>(6 + (int)formFields().size(), (paneBot - paneTop) / 2));
            formPane = {left, paneTop, page.w, inH};
            outPane = {left, paneTop + inH + 1, page.w, paneBot - (paneTop + inH + 1)};
        }

        drawInputPane(surface, formPane);
        drawOutputPane(surface, outPane);

        // ── Flat footer (hint left, identity right) ──
        std::string hint = footerHint();
        surface.text({left, bottom - 1}, inkcell::text::truncate(hint, page.w / 2),
                     theme::footer_dim());
        std::string rightId = "tool · " + title;
        int rw = inkcell::text::display_width(rightId);
        surface.text({std::max(left, right - rw), bottom - 1},
                     inkcell::text::truncate(rightId, page.w / 2), theme::footer_dim());


        if (model_->helpVisible) drawHelp(surface, left, bottom - 1, page.w);
    }

   private:
    using ToolRun = ShellModel::ToolRunRecord;

    struct Field {
        std::string name;
        std::string type;
        std::string desc;
        bool required = false;
        std::vector<std::string> enumValues;  // JSON-schema enum (cycle-edit)
    };

    // ── Run resolution ─────────────────────────────────────────────────
    std::string shortPath() const {
        std::string p = model_->activeToolManifestPath;
        if (p.empty()) return p;
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd))) {
            std::string c = cwd;
            if (c.size() > 1 && p.rfind(c, 0) == 0) p = p.substr(c.size() + 1);
        }
        return p;
    }

    // First sentence only — the PE prose after it is noise until `d`.
    std::string summaryOf(const std::string& d) const {
        size_t end = d.find(". ");
        if (end == std::string::npos) end = d.find('.');
        if (end != std::string::npos) return d.substr(0, end + 1);
        return d;
    }

    const ToolRun* viewedRun() const {
        // While a run is in flight the live body is empty (sync dispatch) —
        // keep the last completed result on display.
        if (model_->toolRunsBusy) {
            if (!model_->toolHistory.empty()) return &model_->toolHistory.back();
            return &model_->lastToolRun;
        }
        if (histIdx_ >= 0 && histIdx_ < (int)model_->toolHistory.size())
            return &model_->toolHistory[histIdx_];
        return &model_->lastToolRun;
    }

    std::string runText(const ToolRun& r) const {
        std::string body = r.output;
        if (!r.error.empty())
            body = body.empty() ? ("[error] " + r.error) : ("[error] " + r.error + "\n" + body);
        return body;
    }

    std::string viewedRunParamsJson() const {
        if (model_->toolRunsBusy) return jsonCompact(params_);
        const ToolRun* r = viewedRun();
        if (!r || r->toolName.empty()) return "{}";
        Json::Value v(Json::objectValue);
        Json::CharReaderBuilder rb;
        std::string err;
        std::istringstream ss(r->paramsJson);
        if (!Json::parseFromStream(rb, ss, &v, &err) || !v.isObject()) return "{}";
        return jsonCompact(v);
    }

    // ── Form (schema) ──────────────────────────────────────────────────
    std::vector<Field> formFields() const {
        std::vector<Field> out;
        if (tool_.inputSchema.empty()) return out;
        Json::Value schema;
        Json::CharReaderBuilder rb;
        std::string errs;
        std::istringstream ss(tool_.inputSchema);
        if (!Json::parseFromStream(rb, ss, &schema, &errs) || !schema.isObject()) return out;
        std::vector<std::string> required;
        if (schema.isMember("required") && schema["required"].isArray())
            for (const auto& v : schema["required"]) required.push_back(v.asString());
        const auto& props = schema.isMember("properties") ? schema["properties"] : Json::Value();
        if (!props.isObject()) return out;
        for (const auto& fn : props.getMemberNames()) {
            const auto& p = props[fn];
            Field f;
            f.name = fn;
            f.type = p.get("type", "string").asString();
            f.desc = p.get("description", "").asString();
            f.required = std::find(required.begin(), required.end(), fn) != required.end();
            if (p.isMember("enum") && p["enum"].isArray())
                for (const auto& en : p["enum"]) f.enumValues.push_back(en.asString());
            out.push_back(std::move(f));
        }
        return out;
    }

    std::string fieldTypeChip(const Field& f) const {
        if (!f.enumValues.empty()) return "enum";
        if (f.type == "boolean") return "bool";
        if (f.type == "integer") return "int";
        if (f.type == "number") return "num";
        if (f.type == "array") return "arr";
        if (f.type == "object") return "obj";
        return "str";
    }

    std::string fieldValueString(const Field& f) const {
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
        return val;
    }

    void moveFocus(int delta) {  // not const: formFocus_ is non-mutable
        int n = (int)formFields().size();
        if (n == 0) return;
        formFocus_ = std::max(0, std::min(formFocus_ + delta, n - 1));
    }

    void beginFieldEdit() {
        auto fields = formFields();
        if (fields.empty()) {
            model_->dashboard.flashNotice("tool · no input fields");
            return;
        }
        formFocus_ = std::max(0, std::min(formFocus_, (int)fields.size() - 1));
        const Field& f = fields[formFocus_];
        if (f.type == "boolean") {
            params_[f.name] = currentBool(f) ? false : true;
            model_->dashboard.flashNotice("set " + f.name + " = " +
                                          (params_[f.name].asBool() ? "true" : "false"));
            return;
        }
        if (!f.enumValues.empty()) {
            cycleEnum(f);
            return;
        }
        model_->composer.value = currentFieldValue();
        model_->composer.cursor = (int)model_->composer.value.size();
        model_->composer.focused = true;
    }

    void cycleFocusedInline() {
        auto fields = formFields();
        if (fields.empty()) return;
        const Field& f = fields[formFocus_ < (int)fields.size() ? formFocus_ : 0];
        if (f.type == "boolean") {
            params_[f.name] = currentBool(f) ? false : true;
            model_->dashboard.flashNotice("set " + f.name + " = " +
                                          (params_[f.name].asBool() ? "true" : "false"));
        } else if (!f.enumValues.empty()) {
            cycleEnum(f);
        }
    }

    void cycleEnum(const Field& f) {
        if (f.enumValues.empty()) return;
        std::string cur = fieldValueString(f);
        size_t idx = 0;
        for (size_t i = 0; i < f.enumValues.size(); ++i)
            if (f.enumValues[i] == cur) idx = i + 1;
        idx %= f.enumValues.size();
        params_[f.name] = f.enumValues[idx];
        model_->dashboard.flashNotice("set " + f.name + " = " + f.enumValues[idx]);
    }

    bool currentBool(const Field& f) const {
        return params_.isMember(f.name) && params_[f.name].isBool() && params_[f.name].asBool();
    }

    std::string currentFieldValue() const {
        auto fields = formFields();
        if (formFocus_ < 0 || formFocus_ >= (int)fields.size()) return "";
        const auto& fn = fields[formFocus_].name;
        if (!params_.isMember(fn)) return "";
        if (params_[fn].isString()) return params_[fn].asString();
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        return Json::writeString(wb, params_[fn]);
    }

    void commitFieldEdit() {
        auto fields = formFields();
        if (formFocus_ < 0 || formFocus_ >= (int)fields.size()) return;
        const Field& f = fields[formFocus_];
        const std::string raw = model_->composer.value;
        if (f.type == "boolean") {
            params_[f.name] = (raw == "true" || raw == "1" || raw == "yes" || raw == "on" || raw == "t");
        } else if (f.type == "integer" || f.type == "number") {
            try {
                if (f.type == "integer")
                    params_[f.name] = std::stoi(raw);
                else
                    params_[f.name] = std::stod(raw);
            } catch (...) {
                model_->dashboard.flashNotice("invalid number — kept previous");
                return;
            }
        } else if (f.type == "array" || f.type == "object") {
            Json::Value parsed;
            Json::CharReaderBuilder rb;
            std::string err;
            std::istringstream is(raw);
            if (Json::parseFromStream(rb, is, &parsed, &err) &&
                (f.type == "array" ? parsed.isArray() : parsed.isObject())) {
                params_[f.name] = parsed;
            } else {
                model_->dashboard.flashNotice("invalid JSON for " + f.name);
                return;
            }
        } else {
            params_[f.name] = raw;
        }
        model_->dashboard.flashNotice("set " + f.name + " = " + raw);
    }

    Json::Value defaultsFromSchema(const ToolSchema& t) const {
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

    // ── History ────────────────────────────────────────────────────────
    void browseHistory(int dir) {  // dir<0 older, dir>0 newer
        if (model_->toolRunsBusy) {
            model_->dashboard.flashNotice("tool · running — wait");
            return;
        }
        size_t n = model_->toolHistory.size();
        if (n == 0) {
            model_->dashboard.flashNotice("tool · no run history yet");
            return;
        }
        if (histIdx_ == -1) {
            histIdx_ = (dir < 0) ? (int)n - 1 : -1;
        } else {
            histIdx_ += (dir < 0) ? -1 : +1;
            if (histIdx_ >= (int)n) histIdx_ = -1;  // newest → live
            if (histIdx_ < -1) histIdx_ = (int)n - 1;
        }
        outView_.scroll_to_end();
        model_->dashboard.flashNotice(histIdx_ == -1
                                          ? "tool · latest run"
                                          : "tool · run #" + std::to_string(histIdx_ + 1) + "/" +
                                                std::to_string(n));
    }

    void loadViewedParams() {
        const ToolRun* r = viewedRun();
        if (!r || r->toolName.empty()) return;
        Json::Value v(Json::objectValue);
        Json::CharReaderBuilder rb;
        std::string err;
        std::istringstream ss(r->paramsJson);
        if (Json::parseFromStream(rb, ss, &v, &err) && v.isObject()) params_ = v;
        else model_->dashboard.flashNotice("tool · no params stored for that run");
        formFocus_ = 0;
        histIdx_ = -1;
        outView_ = inkcell::widgets::ScrollViewState{};
        model_->dashboard.flashNotice("loaded run params into form");
    }

    void runTool() {
        if (model_->toolRunsBusy) return;
        const std::string name =
            tool_.name.empty() ? model_->activeToolName : tool_.name;
        if (name.empty()) {
            model_->dashboard.flashNotice("tool · no name resolved");
            return;
        }
        model_->lastToolRun = ToolRun{};
        model_->lastToolRun.toolName = name;
        model_->lastToolRun.paramsJson = jsonCompact(params_);
        model_->lastToolRun.running = true;
        model_->lastToolRun.timestampMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        model_->toolRunsBusy = true;
        model_->status = "tool running";
        model_->dashboard.flashNotice("running " + name + "…");
        histIdx_ = -1;
        outView_.scroll_to_end();

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
                if (parsed.isMember("output") && parsed["output"].isString())
                    model->lastToolRun.output = parsed["output"].asString();
                else if (parsed.isMember("result"))
                    model->lastToolRun.output = jsonCompact(parsed["result"]);
                else
                    model->lastToolRun.output = raw;
                model->lastToolRun.error = parsed.get("error", "").asString();
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
            if (bridge) bridge->publish(UiEvent::status(
                model->lastToolRun.success ? "tool ok" : "tool fail"));
        });
        worker.detach();
    }

    void copyViewedOutput() {
        const ToolRun* r = viewedRun();
        if (!r || r->toolName.empty()) {
            model_->dashboard.flashNotice("tool · nothing to copy");
            return;
        }
        std::string body = runText(*r);
        if (body.empty()) {
            model_->dashboard.flashNotice("tool · nothing to copy");
            return;
        }
        auto path = "/tmp/mk3-tool-" + r->toolName + "-" + std::to_string(r->timestampMs) + ".out";
        auto res = chat::copyText(body, path);
        model_->dashboard.flashNotice(
            res.copied
                ? ("tool · copied " + std::to_string(body.size()) + "B (" + res.destination + ")")
                : ("tool · wrote → " + res.destination));
    }

    // ── Drawing ────────────────────────────────────────────────────────
    std::pair<std::string, inkcell::Style> statusPill() const {
        if (model_->toolRunsBusy) {
            static const char* g[4] = {"◐", "◓", "◑", "◒"};
            int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
            int frame = (int)((now / 90) % 4);
            int64_t el = now - model_->lastToolRun.timestampMs;
            if (el < 0) el = 0;
            return {std::string(g[frame]) + "  " + std::to_string(el / 1000) + "." +
                        std::to_string((el % 1000) / 100) + "s",
                    theme::amber()};
        }
        const ToolRun* r = viewedRun();
        if (r && !r->toolName.empty()) {
            bool ok = r->success;
            return {(ok ? "● " : "✗ ") + std::to_string((long)r->elapsedMs) + "ms",
                    ok ? theme::green() : theme::red()};
        }
        return {"○ idle", theme::dim()};
    }

    // Color-coded type tag so the form reads at a glance.
    inkcell::Style fieldTagStyle(const Field& f) const {
        if (f.type == "boolean") return theme::green_soft();
        if (!f.enumValues.empty()) return theme::violet_soft();
        if (f.type == "integer" || f.type == "number") return theme::amber();
        if (f.type == "array" || f.type == "object") return theme::cyan_soft();
        return theme::dim();
    }

    std::string footerHint() const {
        if (model_->composer.focused) return "↵ commit value · Esc cancel";
        if (outFocused_)
            return "j/k scroll · PgDn/Up · Home/End · ↵ run · tab form · c copy · [ ] hist · o load";
        return "tab output · j/k field · e edit · space toggle · ↵/r run · c copy · [ ] hist · ? help";
    }

    void drawInputPane(inkcell::Surface& s, inkcell::Rect p) const {
        if (p.h < 3) return;
        auto fields = formFields();
        // Flat panel header band (panel_2 fill, no accent strip).
        components::fillRect(s, {p.x, p.y, p.w, 1}, theme::panel_2());
        s.text({p.x + 2, p.y}, "INPUT", theme::bright());
        // Right block: field count dim + a live RUN affordance button.
        std::string run = model_->toolRunsBusy ? "◐ RUN" : "▶ RUN";
        inkcell::Style runSt = model_->toolRunsBusy ? theme::amber() : theme::green();
        std::string countS =
            std::to_string(fields.size()) + (fields.size() == 1 ? " field" : " fields");
        if (model_->composer.focused) countS += " · editing";
        int countW = (int)inkcell::text::display_width(countS);
        int runW = (int)inkcell::text::display_width(run);
        if (countW + 2 + runW <= p.w - 6) {  // room on the header row
            s.text({p.right() - 1 - runW, p.y}, run, runSt);
            s.text({p.right() - 1 - runW - 2 - countW, p.y},
                   inkcell::text::truncate(countS, countW), theme::dim());
        } else {
            s.text({p.right() - 1 - countW, p.y},
                   inkcell::text::truncate(countS, countW), theme::dim());
        }
        if (p.h < 4) return;
        components::fillRect(s, {p.x, p.y + 1, p.w, p.h - 1}, theme::panel_bg());

        const int listArea = std::max(1, p.h - 2);  // bottom 2 rows pinned
        int n = (int)fields.size();
        if (n > listArea && formScroll_ > n - listArea) formScroll_ = n - listArea;

        if (fields.empty()) {
            s.text({p.x + 2, p.y + 1},
                   inkcell::text::truncate("(no input_schema — run with empty params)", p.w),
                   theme::italic_dim());
            return;
        }

        int rows = 0;
        for (int i = formScroll_; i < n && rows < listArea; ++i, ++rows) {
            const auto& f = fields[i];
            const int ry = p.y + 1 + rows;
            const bool sel = (i == formFocus_);
            components::fillRect(s, {p.x, ry, p.w, 1}, sel ? theme::panel_3() : theme::panel_bg());            if (sel) components::accentBar(s, p.x, ry, 1, theme::footer_accent_focus());

            std::string chip = "[" + fieldTypeChip(f) + "]";
            s.text({p.x + 2, ry}, chip, fieldTagStyle(f));
            int nx = p.x + 2 + (int)chip.size() + 1;
            std::string namePart = f.name + (f.required ? " *" : "");
            std::string val = fieldValueString(f);
            int avail = p.w - (nx - p.x);

            if (sel && model_->composer.focused) {
                s.text({nx, ry}, inkcell::text::truncate(namePart + "  " + val, avail),
                       theme::green());
                continue;
            }
            inkcell::Style nameSt = sel ? theme::bright() : theme::dim();
            inkcell::Style valSt = sel ? theme::bright() : theme::text();
            int vw = inkcell::text::display_width(val);
            int minName = (int)inkcell::text::display_width(namePart);
            if (vw + minName + 2 <= avail && vw > 0) {  // right-align value
                int vx = p.x + p.w - 1 - vw;
                s.text({nx, ry}, inkcell::text::truncate(namePart, vx - nx), nameSt);
                s.text({vx, ry}, inkcell::text::truncate(val, p.w - (vx - p.x) - 1), valSt);
            } else {
                s.text({nx, ry}, inkcell::text::truncate(namePart + "  " + val, avail), valSt);
            }
        }

        int fy = p.y + p.h - 2;
        const Field& rep = fields[formFocus_ < n ? formFocus_ : n - 1];
        if (model_->composer.focused) {
            // VISIBLE editor — the page never drew one before, so editing
            // was blind. Two pinned rows: label/type + a live input box.
            components::fillRect(s, {p.x, fy, p.w, 1}, theme::panel_2());
            s.text({p.x + 2, fy}, "EDIT", theme::bright());
            std::string chip = "[" + fieldTypeChip(rep) + "]";
            s.text({p.x + 8, fy}, chip, fieldTagStyle(rep));
            int ey = p.x + 8 + (int)chip.size() + 2;
            s.text({ey, fy},
                   inkcell::text::truncate(rep.name + "  " + rep.desc, p.right() - ey - 1),
                   theme::italic_dim());

            int py = p.y + p.h - 1;
            components::fillRect(s, {p.x, py, p.w, 1}, theme::footer_bg());
            std::string prop = rep.name + " =";
            int px = p.x + 2;
            s.text({px, py}, prop, theme::dim());
            int ix = px + (int)inkcell::text::display_width(prop) + 1;
            const std::string& val = model_->composer.value;
            int cx = std::max(0, std::min(model_->composer.cursor, (int)val.size()));
            std::string before = val.substr(0, (size_t)cx);
            std::string after = val.substr((size_t)cx);
            int avail = (p.right() - 1) - ix;
            std::string bcut = inkcell::text::truncate(before, avail);
            s.text({ix, py}, bcut, theme::bright());
            int bx = ix + (int)inkcell::text::display_width(bcut);
            s.text({bx, py}, "▊", theme::amber());
            int afterW = avail - (int)inkcell::text::display_width(bcut) - 1;
            if (afterW > 0)
                s.text({bx + 1, py}, inkcell::text::truncate(after, afterW), theme::text());
            return;
        }

        s.text({p.x + 2, fy},
               inkcell::text::truncate((rep.required ? "* " : "") + rep.name + "  " + rep.desc,
                                       p.w - 3),
               theme::italic_dim());
        int py = p.y + p.h - 1;
        std::string preview = viewedRunParamsJson();
        if (!preview.empty() && preview != "{}")
            s.text({p.x + 2, py},
                   inkcell::text::truncate("params  " + preview, p.w - 2), theme::footer_dim());
    }

    void drawOutputPane(inkcell::Surface& s, inkcell::Rect p) const {
        if (p.h < 2) return;
        const ToolRun* r = viewedRun();
        std::string right;
        if (model_->toolRunsBusy) {
            right = "◐ running";
        } else if (histIdx_ >= 0) {
            right = "run #" + std::to_string(histIdx_ + 1) + "/" +
                    std::to_string(model_->toolHistory.size()) + " · o load";
        } else if (r && !r->toolName.empty()) {
            right = (r->success ? "● " : "✗ ") + std::to_string((long)r->elapsedMs) + "ms";
        } else {
            right = "no run yet";
        }
        components::fillRect(s, {p.x, p.y, p.w, 1}, theme::panel_2());
        s.text({p.x + 2, p.y}, "OUTPUT", theme::bright());
        int rw = inkcell::text::display_width(right);
        s.text({std::max(p.x + 2, p.right() - rw - 2), p.y},
               inkcell::text::truncate(right, p.w - 4), theme::dim());
        components::fillRect(s, {p.x, p.y + 1, p.w, p.h - 1}, theme::panel_bg());

        // Busy: indeterminate shimmer sweep on the first content row (pure
        // function of wall-clock — no fake per-frame state).
        if (model_->toolRunsBusy) {
            std::string sh = busyShimmer(p.w - 3);
            s.text({p.x + 2, p.y + 1}, sh, theme::amber());
            s.text({p.x + 2, p.y + 2},
                   inkcell::text::truncate("synchronous dispatch — result pending", p.w - 4),
                   theme::dim());
            return;
        }

        std::vector<std::string> lines;
        if (r && !r->toolName.empty()) {
            std::string body = runText(*r);
            if (body.empty()) body = "(no output)";
            for (const auto& l : chat::wrapWordsLossless(body, p.w - 3)) {
                lines.push_back(l);
                if ((int)lines.size() >= 1500) {
                    lines.push_back("… (truncated at 1500 lines)");
                    break;
                }
            }
        } else {
            // designed empty state
            s.text({p.x + 2, p.y + 1}, "nothing run yet", theme::italic_dim());
            s.text({p.x + 2, p.y + 2},
                   inkcell::text::truncate("press ↵ to dispatch the tool with the current form parameters",
                                           p.w - 3),
                   theme::dim());
            return;
        }
        outView_.viewport_h = p.h - 1;
        outView_.set_lines(std::move(lines));
        int off = outView_.offset;
        for (int row = 1; row < p.h; ++row) {
            int idx = off + (row - 1);
            if (idx >= 0 && idx < (int)outView_.lines.size())
                s.text({p.x + 2, p.y + row}, inkcell::text::truncate(outView_.lines[idx], p.w - 3),
                       theme::text());
        }

        // Scrollbar thumb (right-edge col) — only when content overflows;
        // tracks offset, greens/blues at the bottom of the run.
        int total = (int)outView_.lines.size();
        int vp = outView_.viewport_h;
        if (total > vp && p.h > 3) {
            int maxOff = total - vp;
            int o = std::max(0, std::min(off, maxOff));
            int trackH = p.h - 1;
            int trow = (trackH > 1) ? (int)((long long)o * (trackH - 1) / maxOff) : 0;
            s.text({p.right() - 1, p.y + 1 + trow}, "▌",
                   o >= maxOff ? theme::cyan() : theme::violet_soft());
        }
    }

    // Indeterminate brightness pulse traveling left→right. Block chars give
    // 8 luminance steps; pure function of wall-clock.
    std::string busyShimmer(int w) const {
        static const std::string blk = " ▁▂▃▄▅▆▇█";
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
        std::string out;
        out.reserve(w > 0 ? (size_t)w : 0);
        int pos = (int)((now / 40) % std::max(1, w - 1));
        for (int i = 0; i < w; ++i) {
            int d = std::abs(i - pos);
            int v = 8 - 2 * d;
            out += blk[(size_t)std::clamp(v, 0, 8)];
        }
        return out;
    }

    void drawFooterHint(inkcell::Surface& s, int x, int y, int w) const {
        std::string hint;
        inkcell::Style st = theme::footer_dim();
        if (model_->composer.focused) {
            hint = "↵ commit value · Esc cancel";
        } else if (outFocused_) {
            hint = "j/k scroll · ↑/↓ page · Home/End · ↵ run · tab form · c copy · [ ] hist · o load · m hub";
        } else {
            hint = "tab output · j/k field · e edit · space toggle · ↵/r run · c copy · [ ] hist · ? help";
        }
        s.text({x, y}, inkcell::text::truncate(hint, w), st);
    }

    void drawHelp(inkcell::Surface& s, int x, int bottom, int /*w*/) const {
        std::vector<std::string> rows = {
            "TOOL KEYS", "",
            "tab          form ⇄ output focus", "j / k        field / scroll lines",
            "e / enter    edit (bool·enum toggle · text prompt)", "space        toggle focused bool / cycle enum",
            "↵ / r        run with current params", "[ / ]        browse run history",
            "g            back to latest run", "o            load run params into form",
            "c            copy viewed output → clipboard", "?            this help",
            "m / esc      back to hub",
        };
        int bh = (int)rows.size() + 2;
        int y0 = bottom - bh;
        for (size_t i = 0; i < rows.size() && y0 + (int)i <= bottom; ++i) {
            int y = y0 + (int)i;
            inkcell::Style st = rows[i].empty() ? theme::dim() : theme::bright();
            if (rows[i] == "TOOL KEYS") st = theme::violet_soft();
            s.text({x, y}, rows[i], st);
        }
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
    bool outFocused_ = false;
    int histIdx_ = -1;
    // Mutable: clamped in drawInputPane (const) against the pane height.
    mutable int formScroll_ = 0;
    mutable bool descExpanded_ = false;
    mutable inkcell::widgets::ScrollViewState outView_;
};

}  // namespace cortex::mk3::ui::scenes