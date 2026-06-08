// src/tui/components/protocol.hpp — Incremental ProtocolView renderer
// Appends new items on each render() call — never re-renders old content.
// Background color blocks, --- separators, info-rich builtins.
#pragma once

#include "../terminal.hpp"
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <unordered_set>
#include <json/json.h>

namespace cortex::mk3::tui {

enum class ActionType { TOOL, AGENT, RELIC, FEED };
enum class BlockType { THOUGHT, RESPONSE, FEED };

struct ContentBlock { BlockType type = BlockType::RESPONSE; std::string text; bool isFinal = false; };

struct ActionEvent {
    ActionType type = ActionType::TOOL;
    std::string name, id, body;
    bool sync = true;
};

struct ResultEvent {
    std::string id;
    bool ok = true;
    std::string summary, toolName;
    int exitCode = 0;        // 0 = success
    double elapsedMs = 0;    // execution time
    size_t outputBytes = 0;  // byte count
    bool dirty = false;      // set on streaming update — rebuild deferred
    bool isDelta = true;     // summary is a chunk, not accumulated (streaming contract)
};

class ProtocolView {
public:
    void addAction(const ActionEvent& a) { actions_.push_back(a); }
    void addResult(const ResultEvent& r) {
        // Check if this is an update to an existing result (progressive streaming)
        for (size_t i = 0; i < results_.size(); i++) {
            if (results_[i].id == r.id) {
                // Progressive streaming: summary is a delta (chunk), not the full string.
                // If your caller accumulates, set r.isDelta = false and assign, don't append.
                results_[i].summary += "\n" + r.summary;  // append delta lines
                results_[i].dirty = true;
                needsRebuild_ = true;
                return;
            }
        }
        results_.push_back(r);
    }
    void addRenderedLines(const std::vector<std::string>& lines) {
        renderedBlocks_.push_back(lines);
    }

    void clear() {
        actions_.clear(); results_.clear(); renderedBlocks_.clear();
        cached_lines_.clear(); lastAction_ = 0; lastResult_ = 0;
    }

    // Incremental: only renders NEW actions/results since last call
    std::vector<std::string> render(int width) {
        if (width_ != width) { cached_lines_.clear(); lastAction_ = 0; lastResult_ = 0; lastBlock_ = 0; width_ = width; }
        if (needsRebuild_) { cached_lines_.clear(); lastAction_ = 0; lastResult_ = 0; lastBlock_ = 0; needsRebuild_ = false; }

        size_t ai = lastAction_, ri = lastResult_;
        for (; ai < actions_.size(); ai++) {
            auto& a = actions_[ai];
            std::string bg = bgAction();

            // Action header
            cached_lines_.push_back(bg + "  " + actionIcon(a) + fgBold() + a.name + fgReset() +
                (a.id.empty() || a.id == a.name ? "" : fgDim() + "#" + a.id + fgReset()) +
                std::string(std::max(0, width - (int)visLen(actionIcon(a) + a.name + a.id)), ' ') +
                bgReset());

            // Action params
            auto params = actionParams(a, width);
            for (auto& p : params)
                cached_lines_.push_back(bg + "      " + p + std::string(std::max(0, width - (int)visLen(p) - 6), ' ') + bgReset());

            // Result
            if (ri < results_.size()) {
                auto& r = results_[ri++];
                auto resLines = resultLines(r, width);
                std::string rb = r.ok ? bgOk() : bgErr();
                bool first = true;
                for (auto& rl : resLines) {
                    cached_lines_.push_back(rb + (first ? "  " : "    ") + rl +
                        std::string(std::max(0, width - (int)visLen(rl) - (first ? 2 : 4)), ' ') + bgReset());
                    first = false;
                }
            }

            if (ai + 1 < actions_.size() || ri < results_.size())
                cached_lines_.push_back(fgDim() + "  ---" + fgReset());
        }

        for (; ri < results_.size(); ri++) {
            auto& r = results_[ri];
            auto resLines = resultLines(r, width);
            std::string rb = r.ok ? bgOk() : bgErr();
            bool first = true;
            for (auto& rl : resLines) {
                cached_lines_.push_back(rb + (first ? "  " : "    ") + rl +
                    std::string(std::max(0, width - (int)visLen(rl) - (first ? 2 : 4)), ' ') + bgReset());
                first = false;
            }
            cached_lines_.push_back(fgDim() + "  ---" + fgReset());
        }

        lastAction_ = actions_.size();
        lastResult_ = results_.size();

        // Append pre-rendered markdown blocks (incremental — only new blocks)
        for (size_t i = lastBlock_; i < renderedBlocks_.size(); i++)
            cached_lines_.insert(cached_lines_.end(), renderedBlocks_[i].begin(), renderedBlocks_[i].end());
        lastBlock_ = renderedBlocks_.size();

        return cached_lines_;
    }

    std::vector<std::string> incremental(int width) {
        auto all = render(width);
        if (cursor_ >= all.size()) return {};
        auto r = std::vector<std::string>(all.begin() + cursor_, all.end());
        cursor_ = all.size();
        return r;
    }

    void resetStream() { cursor_ = 0; }
    size_t totalLines() const { return cached_lines_.size(); }

private:
    // ── ANSI helpers (foreground-only, never reset background) ──
    static std::string bgAction() { return "\033[48;2;30;30;40m"; }
    static std::string bgOk()     { return "\033[48;2;25;35;30m"; }
    static std::string bgErr()    { return "\033[48;2;40;25;25m"; }
    static std::string bgReset()  { return "\033[49m"; }
    static std::string fgReset()  { return "\033[39m\033[22m\033[24m"; }  // no bg reset
    static std::string fgDim()    { return "\033[2m\033[37m"; }
    static std::string fgBold()   { return "\033[1m\033[97m"; }

    static std::string actionIcon(const ActionEvent& a) {
        switch (a.type) {
            case ActionType::AGENT: return "\033[38;2;255;180;0m→ \033[0m";
            default: return fgDim() + std::string(a.type == ActionType::TOOL ? "⚙ " :
                a.type == ActionType::RELIC ? "◇ " : "⏳ ") + fgReset();
        }
    }

    static size_t visLen(const std::string& s) {
        size_t n = 0; bool esc = false;
        for (char c : s) { if (c == '\033') esc = true; else if (esc && c == 'm') esc = false; else if (!esc) n++; }
        return n;
    }

    // ── Action params ──
    std::vector<std::string> actionParams(const ActionEvent& a, int width) const {
        std::vector<std::string> lines;
        if (a.body.empty()) return lines;
        std::string body = a.body;
        if (body[0] == '{') {
            Json::Value root;
            Json::CharReaderBuilder b; std::string errs;
            std::istringstream js(body);
            if (Json::parseFromStream(b, js, &root, &errs) && root.isObject()) {
                for (auto& key : root.getMemberNames()) {
                    std::string val = root[key].isObject() || root[key].isArray() ? "{...}" : root[key].asString();
                    if (val.size() > (size_t)(width - 20)) val = val.substr(0, width - 23) + "...";
                    lines.push_back(ansi::fg(50,200,200) + fgBold() + key + fgReset() +
                        fgDim() + ":  " + fgReset() + fgDim() + val + fgReset());
                }
                return lines;
            }
        }
        std::string c = body;
        if (c.size() > (size_t)(width - 8)) c = c.substr(0, width - 11) + "...";
        lines.push_back(fgDim() + c + fgReset());
        return lines;
    }

    // ── Result lines ──
    std::vector<std::string> resultLines(const ResultEvent& r, int width) const {
        std::vector<std::string> lines;
        std::string body = r.summary;
        std::string mark = r.ok ?
            ansi::fg(0,200,0) + "✓ " + fgReset() :
            ansi::fg(200,50,0) + "✗ " + fgReset();

        // Metadata line: timing, exit code, byte count (only when present)
        if (r.elapsedMs > 0 || r.exitCode != 0 || r.outputBytes > 0) {
            std::ostringstream meta;
            meta << fgDim();
            if (r.elapsedMs > 0) meta << std::fixed << std::setprecision(0) << r.elapsedMs << "ms ";
            if (r.exitCode != 0) meta << ansi::fg(200,50,0) << "exit:" << r.exitCode << fgDim() << " ";
            if (r.outputBytes > 0) {
                if (r.outputBytes >= 1024) meta << std::fixed << std::setprecision(1) << (r.outputBytes / 1024.0) << "KB";
                else meta << r.outputBytes << "B";
            }
            meta << fgReset();
            lines.push_back(mark + meta.str());
            mark = "  ";  // subsequent lines get indent, not checkmark
        }

        if (!r.ok && (body.empty() || body == "?")) {
            lines.push_back(mark + ansi::fg(200,50,0) + "failed" + fgReset()); return lines;
        }

        // Builtins (constant-time lookup)
        static const std::unordered_set<std::string> bi = {"bash","exec","list","ls","read","fs_read","write","edit","grep","search","pin","ethereal"};
        bool isB = !r.toolName.empty() && bi.count(r.toolName);
        if (isB) return builtinLines(r, mark, width);

        // JSON → YAML
        if (!body.empty() && (body[0] == '{' || body[0] == '[')) {
            std::string yaml = jsonToYaml(body, 4);
            if (!yaml.empty()) {
                std::istringstream ys(yaml); std::string yl; bool first = true;
                while (std::getline(ys, yl)) { if (yl.empty()) continue; lines.push_back((first ? mark : "  ") + yl); first = false; }
                return lines;
            }
        }

        // Plain text
        std::istringstream bs(body); std::string bl; bool first = true;
        while (std::getline(bs, bl)) { lines.push_back((first ? mark : "  ") + fgDim() + bl + fgReset()); first = false; }
        return lines;
    }

    std::vector<std::string> builtinLines(const ResultEvent& r, const std::string& mark, int width) const {
        std::vector<std::string> lines;
        auto& tn = r.toolName;
        std::string body = r.summary;
        (void)width;

        if (tn == "bash" || tn == "exec") {
            std::istringstream ss(body); std::string l; bool first = true;
            while (std::getline(ss, l)) { lines.push_back((first ? mark : "  ") + (first ? l : fgDim() + l + fgReset())); first = false; }
            return lines;
        }
        if (tn == "list" || tn == "ls") {
            std::istringstream ss(body); std::string l; bool first = true;
            while (std::getline(ss, l)) { if (l.empty()) continue; lines.push_back((first ? mark : "  ") + fgDim() + l + fgReset()); first = false; }
            return lines;
        }
        // read, write, edit, grep, search, pin, ethereal
        size_t nl = body.find('\n');
        std::string first = (nl != std::string::npos) ? body.substr(0, nl) : body;
        if (first.empty()) first = "(empty)";
        lines.push_back(mark + fgDim() + first + fgReset());
        return lines;
    }

    static std::string jsonToYaml(const std::string& json, int indent) {
        Json::Value root; Json::CharReaderBuilder b; std::string errs;
        std::istringstream js(json);
        if (!Json::parseFromStream(b, js, &root, &errs)) return "";
        std::string out; jsonToYamlImpl(root, indent, out); return out;
    }

    static void jsonToYamlImpl(const Json::Value& v, int indent, std::string& out) {
        std::string pad(indent, ' ');
        auto keyS = [](const std::string& s) { return std::string(ansi::fg(50,200,200)) + fgBold() + s + fgReset(); };
        auto valS = [](const std::string& s) { return fgDim() + s + fgReset(); };
        if (v.isObject()) {
            for (auto& key : v.getMemberNames()) {
                out += "\n" + pad + keyS(key) + fgDim() + ":" + fgReset();
                if (v[key].isObject()) jsonToYamlImpl(v[key], indent+2, out);
                else if (v[key].isArray() && v[key].size() > 0 && v[key][0].isObject())
                    for (auto& item : v[key]) jsonToYamlImpl(item, indent+2, out);
                else if (v[key].isArray())
                    for (auto& item : v[key]) out += "\n" + pad + "  - " + valS(item.asString());
                else out += " " + valS(v[key].asString());
            }
        } else if (v.isArray()) {
            for (auto& item : v) { out += "\n" + pad + fgDim() + "-" + fgReset(); if (item.isObject()) jsonToYamlImpl(item, indent+2, out); else out += " " + valS(item.asString()); }
        }
    }

    std::vector<ActionEvent> actions_;
    std::vector<ResultEvent> results_;
    std::vector<std::vector<std::string>> renderedBlocks_;
    std::vector<std::string> cached_lines_;
    int width_ = 0;
    size_t lastAction_ = 0, lastResult_ = 0, lastBlock_ = 0, cursor_ = 0;
    bool needsRebuild_ = false;
};

} // namespace cortex::mk3::tui
