// src/tui/renderer.hpp — TuiRenderer: ANSI-aware transcript renderer
// Mode 1 FULL RENDER: ordered transcript events (thought/action/result/response)
// Mode 2 SEMI:        raw stream on left, protocol/response frame on right
// Mode 3 RAW:         raw LLM stream only
//
// Renderer architecture follows a small 2D-engine model: build immutable rows,
// compose into ANSI-aware surfaces, then let the terminal viewport decide what
// to present. The runtime owns event order; the renderer only draws it.
#pragma once

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "../protocol/events.hpp"
#include "components/markdown.hpp"
#include "components/protocol.hpp"
#include "surface.hpp"
#include "terminal.hpp"
#include "width.hpp"

namespace cortex::mk3::tui {

enum class RenderMode { FULL = 0, SEMI = 1, RAW = 2 };

inline ActionType actionTypeFromName(const std::string& type) {
    if (type == "agent")
        return ActionType::AGENT;
    if (type == "relic")
        return ActionType::RELIC;
    if (type == "feed")
        return ActionType::FEED;
    return ActionType::TOOL;
}

inline ActionEvent toActionEvent(const ProtocolAction& a) {
    ActionEvent ev;
    ev.type = actionTypeFromName(a.type);
    ev.name = a.name;
    ev.id = a.id;
    ev.body = a.body;
    ev.sync = a.sync;
    ev.mode = a.mode;
    ev.modifiers = a.modifiers;
    return ev;
}

inline ResultEvent toResultEvent(const ProtocolResult& r) {
    ResultEvent ev;
    ev.id = r.id;
    ev.ok = r.ok;
    ev.summary = r.summary;
    ev.toolName = r.toolName;
    ev.exitCode = r.exitCode;
    ev.elapsedMs = r.elapsedMs;
    ev.outputBytes = r.outputBytes;
    return ev;
}

class TuiRenderer {
   public:
    struct FrameStats {
        int width = 0;
        int lineCount = 0;
        int dirtyRows = 0;
        RenderMode mode = RenderMode::FULL;
    };

    TuiRenderer(int width = 80) : width_(std::max(1, width)) { md_.setWidth(markdownWidth(width_)); }

    // ── Set content (called after each prompt / stream tick) ──
    void setRawStream(const std::string& raw) { raw_ = raw; }
    void setResponse(const std::string& text) { response_ = text; }
    void appendResponse(const std::string& text) { response_ += text; }

    bool setThought(const std::string& text) {
        if (thought_ == text)
            return false;
        thought_ = text;
        return true;
    }

    // Legacy protocol mutators retained for tests and simple embedders. The
    // production runtime normally calls renderTranscript(events, response, w).
    void addProtocolAction(const std::string& type, const std::string& name, const std::string& id,
                           bool sync = true, const std::string& body = "") {
        ProtocolEvent ev;
        ev.kind = ProtocolEventKind::ACTION;
        ev.action.type = type;
        ev.action.name = name;
        ev.action.id = id;
        ev.action.sync = sync;
        ev.action.mode = sync ? "sync" : "async";
        ev.action.body = body;
        events_.push_back(ev);
    }

    void addProtocolResult(const std::string& id, bool ok, const std::string& summary,
                           const std::string& toolName = "", int exitCode = 0,
                           double elapsedMs = 0, size_t outputBytes = 0) {
        ProtocolEvent ev;
        ev.kind = ProtocolEventKind::RESULT;
        ev.result.id = id;
        ev.result.ok = ok;
        ev.result.summary = summary;
        ev.result.toolName = toolName;
        ev.result.exitCode = exitCode;
        ev.result.elapsedMs = elapsedMs;
        ev.result.outputBytes = outputBytes;
        events_.push_back(ev);
    }

    void setToolAnsiPassthrough(bool enabled) { pv_.setAnsiPassthrough(enabled); }

    // ── Mode control ──
    void setMode(RenderMode m) { mode_ = m; }
    RenderMode mode() const { return mode_; }

    void cycleMode() {
        switch (mode_) {
            case RenderMode::FULL:
                mode_ = RenderMode::SEMI;
                break;
            case RenderMode::SEMI:
                mode_ = RenderMode::RAW;
                break;
            case RenderMode::RAW:
                mode_ = RenderMode::FULL;
                break;
        }
    }

    static const char* modeName(RenderMode m) {
        switch (m) {
            case RenderMode::FULL:
                return "FULL";
            case RenderMode::SEMI:
                return "SEMI";
            case RenderMode::RAW:
                return "RAW";
        }
        return "?";
    }

    // ── Render based on current mode ──
    std::vector<std::string> render() {
        std::vector<std::string> lines;
        switch (mode_) {
            case RenderMode::FULL:
                lines = renderLegacyFull();
                break;
            case RenderMode::SEMI:
                lines = renderSemi(events_, response_, width_);
                break;
            case RenderMode::RAW:
                lines = renderRaw(raw_);
                break;
        }
        updateStats(lines);
        return lines;
    }

    // Stateless ordered transcript render. The agent runtime owns the order.
    std::vector<std::string> renderTranscript(const std::vector<ProtocolEvent>& events,
                                             const std::string& responseText, int width) const {
        width = std::max(1, width);
        TuiSurface frame(width);

        auto sourceById = buildSourceIndex(events);
        for (const auto& ev : events) {
            switch (ev.kind) {
                case ProtocolEventKind::THOUGHT:
                    appendThoughtBlock(frame, ev.text, width);
                    break;
                case ProtocolEventKind::ACTION: {
                    auto card = pv_.renderActionCard(toActionEvent(ev.action), width);
                    frame.appendLines(card);
                    break;
                }
                case ProtocolEventKind::RESULT: {
                    ResultEvent re = toResultEvent(ev.result);
                    auto it = sourceById.find(re.id);
                    if (it != sourceById.end())
                        re.sourceType = it->second;
                    auto card = pv_.renderResultCard(re, width);
                    frame.appendLines(card);
                    break;
                }
                case ProtocolEventKind::RESPONSE:
                    appendResponseBlock(frame, ev.text.empty() ? responseText : ev.text, width);
                    break;
                case ProtocolEventKind::STATUS:
                    frame.appendLines({ev.text});
                    break;
            }
        }

        const bool hasResponseEvent = std::any_of(events.begin(), events.end(), [](const ProtocolEvent& ev) {
            return ev.kind == ProtocolEventKind::RESPONSE;
        });
        if (!hasResponseEvent)
            appendResponseBlock(frame, responseText, width);
        return frame.rows();
    }

    std::string renderDiff(const std::vector<std::string>& previous,
                           const std::vector<std::string>& current, int startRow = 1) const {
        return TuiSurface::renderDiff(previous, current, startRow);
    }

    const FrameStats& lastStats() const { return lastStats_; }

    void clear() {
        raw_.clear();
        response_.clear();
        thought_.clear();
        lastMarkdownText_.clear();
        events_.clear();
        previousFrame_.clear();
        pv_.clear();
    }

    void setWidth(int w) {
        width_ = std::max(1, w);
        md_.setWidth(markdownWidth(width_));
        previousFrame_.clear();
    }

   private:
    static int markdownWidth(int width) { return std::max(1, width - 5); }

    std::vector<std::string> renderLegacyFull() const {
        std::vector<ProtocolEvent> events = events_;
        if (!thought_.empty()) {
            ProtocolEvent thought;
            thought.kind = ProtocolEventKind::THOUGHT;
            thought.text = thought_;
            events.insert(events.begin(), thought);
        }
        return renderTranscript(events, response_, width_);
    }

    std::vector<std::string> renderSemi(const std::vector<ProtocolEvent>& events,
                                        const std::string& responseText, int width) const {
        width = std::max(1, width);
        if (width < 24)
            return renderTranscript(events, responseText, width);

        const std::string sep = ansi::dim() + " │ " + ansi::reset();
        const int sepW = 3;
        const int leftW = std::clamp(width / 3, 10, std::max(10, width - sepW - 8));
        const int rightW = std::max(1, width - leftW - sepW);

        auto left = renderRaw(raw_);
        auto right = renderTranscript(events, responseText, rightW);
        if (left.empty() && !raw_.empty())
            left.push_back(raw_);

        const size_t rows = std::max(left.size(), right.size());
        std::vector<std::string> out;
        out.reserve(rows);
        for (size_t i = 0; i < rows; ++i) {
            std::string l = i < left.size() ? ansi::dim() + left[i] + ansi::reset() : "";
            std::string r = i < right.size() ? right[i] : "";
            out.push_back(TuiSurface::fitRow(l, leftW) + sep + TuiSurface::fitRow(r, rightW));
        }
        return out;
    }

    static std::vector<std::string> renderRaw(const std::string& raw) {
        std::vector<std::string> lines;
        std::istringstream rs(raw);
        std::string rl;
        while (std::getline(rs, rl)) {
            if (!rl.empty() && rl.back() == '\r')
                rl.pop_back();
            lines.push_back(rl);
        }
        if (lines.empty() && !raw.empty())
            lines.push_back(raw);
        return lines;
    }

    static std::unordered_map<std::string, ActionType> buildSourceIndex(
        const std::vector<ProtocolEvent>& events) {
        std::unordered_map<std::string, ActionType> byId;
        for (const auto& e : events) {
            if (e.kind == ProtocolEventKind::ACTION && !e.action.id.empty())
                byId[e.action.id] = actionTypeFromName(e.action.type);
        }
        return byId;
    }

    static void appendThoughtBlock(TuiSurface& frame, const std::string& text, int width) {
        if (text.empty())
            return;
        const std::string bg = "\033[48;2;18;22;32m";
        frame.appendLine(bg);
        std::istringstream ts(text);
        std::string tl;
        while (std::getline(ts, tl)) {
            if (!tl.empty() && tl.back() == '\r')
                tl.pop_back();
            if (tl.empty())
                continue;
            frame.appendLine(bg + " " + ansi::dim() + tl + " ");
        }
        frame.appendLine(bg);
        (void)width;
    }

    static void appendResponseBlock(TuiSurface& frame, const std::string& responseText, int width) {
        if (responseText.empty())
            return;

        Markdown localMd;
        localMd.setWidth(markdownWidth(width));
        localMd.setText(responseText);
        auto rendered = localMd.render();

        bool hasContent = false;
        for (const auto& l : rendered) {
            for (char c : l) {
                if (c != ' ' && c != '\n' && c != '\r') {
                    hasContent = true;
                    break;
                }
            }
            if (hasContent)
                break;
        }

        if (hasContent && !rendered.empty()) {
            for (const auto& l : rendered)
                frame.appendLine(" " + l);
        } else {
            frame.appendLine(" " + responseText);
        }
        frame.appendLine("\033[48;2;28;25;38m");
    }

    void updateStats(const std::vector<std::string>& lines) {
        lastStats_.width = width_;
        lastStats_.lineCount = static_cast<int>(lines.size());
        lastStats_.dirtyRows = static_cast<int>(TuiSurface::dirtyRows(previousFrame_, lines).size());
        lastStats_.mode = mode_;
        previousFrame_ = lines;
    }

    ProtocolView pv_;
    Markdown md_;
    int width_ = 80;
    RenderMode mode_ = RenderMode::FULL;
    std::string raw_;       // raw LLM output
    std::string response_;  // sanitized response
    std::string thought_;   // live thought content for legacy render()
    std::string lastMarkdownText_;
    std::vector<ProtocolEvent> events_;
    std::vector<std::string> previousFrame_;
    FrameStats lastStats_;
};

}  // namespace cortex::mk3::tui
