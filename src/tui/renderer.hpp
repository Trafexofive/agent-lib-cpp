// src/tui/renderer.hpp — TuiRenderer: 3-mode output renderer
// Mode 1 FULL RENDER: ordered transcript events (thought/action/result/response)
// Mode 2 SEMI:        raw stream on left, protocol markers on right
// Mode 3 RAW:         raw LLM stream only
// Stateless transcript rendering: the runtime owns the event order, the
// renderer just draws what actually happened.
#pragma once

#include <sstream>
#include <string>
#include <vector>

#include "../core/agent.hpp"
#include "components/markdown.hpp"
#include "components/protocol.hpp"
#include "terminal.hpp"

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
    return {actionTypeFromName(a.type), a.name, a.id, a.body, a.sync};
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
    TuiRenderer(int width = 80) : width_(width) {
        md_.setWidth(width - 4);
    }

    // ── Set content (called after each prompt) ──
    void setRawStream(const std::string& raw) {
        raw_ = raw;
    }
    void setResponse(const std::string& text) {
        response_ = text;
    }
    void appendResponse(const std::string& text) {
        response_ += text;
    }  // streaming
    bool setThought(const std::string& text) {
        if (thought_ == text)
            return false;
        thought_ = text;
        return true;
    }

    void setToolAnsiPassthrough(bool enabled) {
        pv_.setAnsiPassthrough(enabled);
    }

    // ── Mode control ──
    void setMode(RenderMode m) {
        mode_ = m;
    }
    RenderMode mode() const {
        return mode_;
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

    // ── Render based on mode ──
    std::vector<std::string> render() {
        return renderTranscript({}, response_, width_);
    }

    // Stateless ordered transcript render. The agent runtime owns the order.
    std::vector<std::string> renderTranscript(const std::vector<ProtocolEvent>& events,
                                             const std::string& responseText, int width) const {
        // Subtle transcript background — same for thought and response blocks.
        // Action/result cards draw their own (darker) background, so this shows
        // through only on non-card areas.
        const std::string bg = "\033[48;2;18;22;32m";
        const std::string bgReset = "\033[49m";
        const std::string pad = bg + std::string(width, ' ') + bgReset;

        std::vector<std::string> lines;
        if (events.empty() && responseText.empty())
            return lines;
        lines.push_back(pad);

        for (const auto& ev : events) {
            switch (ev.kind) {
                case ProtocolEventKind::THOUGHT: {
                    std::istringstream ts(ev.text);
                    std::string tl;
                    while (std::getline(ts, tl)) {
                        if (!tl.empty() && tl.back() == '\r')
                            tl.pop_back();
                        if (tl.empty())
                            continue;
                        std::string line = bg + ansi::dim() + tl + ansi::reset() + bgReset;
                        lines.push_back(padRight(line, width));
                    }
                    break;
                }
                case ProtocolEventKind::ACTION: {
                    auto card = pv_.renderActionCard(toActionEvent(ev.action), width);
                    lines.insert(lines.end(), card.begin(), card.end());
                    break;
                }
                case ProtocolEventKind::RESULT: {
                    ResultEvent re = toResultEvent(ev.result);
                    if (re.sourceType != ActionType::AGENT) {
                        for (const auto& e : events) {
                            if (e.kind == ProtocolEventKind::ACTION && e.action.id == re.id) {
                                re.sourceType = actionTypeFromName(e.action.type);
                                break;
                            }
                        }
                    }
                    auto card = pv_.renderResultCard(re, width);
                    lines.insert(lines.end(), card.begin(), card.end());
                    break;
                }
                case ProtocolEventKind::RESPONSE: {
                    // Rendered at the end if responseText non-empty; skip here.
                    break;
                }
            }
        }
        if (!responseText.empty()) {
            Markdown localMd;
            localMd.setWidth(width - 4);
            localMd.setText(responseText);
            auto rendered = localMd.render();
            bool hasContent = false;
            for (auto& l : rendered) {
                for (auto c : l)
                    if (c != ' ' && c != '\n' && c != '\r') {
                        hasContent = true;
                        break;
                    }
            }
            if (hasContent && !rendered.empty()) {
                lines.push_back(bg + ansi::dim() + std::string("── Response ──") + ansi::reset() +
                                bgReset);
                for (auto& l : rendered) {
                    lines.push_back(bg + l + bgReset);
                }
            } else if (!response_.empty()) {
                lines.push_back(bg + ansi::dim() + std::string("── Response ──") + ansi::reset() +
                                bgReset);
            } else if (!responseText.empty()) {
                lines.push_back(bg + ansi::dim() + std::string("── Response ──") + ansi::reset() +
                                bgReset);
                lines.push_back(bg + responseText + bgReset);
            }
        }
        lines.push_back(pad);
        return lines;
    }

    void clear() {
        raw_.clear();
        response_.clear();
        thought_.clear();
        lastMarkdownText_.clear();
        pv_.clear();
    }

    void setWidth(int w) {
        width_ = w;
        md_.setWidth(w - 4);
    }

   private:
    std::vector<std::string> renderRaw() {
        std::vector<std::string> lines;
        std::istringstream rs(raw_);
        std::string rl;
        while (std::getline(rs, rl))
            lines.push_back(ansi::dim() + rl + ansi::reset());
        if (lines.empty() && !raw_.empty())
            lines.push_back(ansi::dim() + raw_ + ansi::reset());
        return lines;
    }

    ProtocolView pv_;
    Markdown md_;
    int width_ = 80;
    RenderMode mode_ = RenderMode::FULL;
    std::string raw_;       // raw LLM output
    std::string response_;  // sanitized response
    std::string thought_;   // thought content
    std::string lastMarkdownText_;
};

}  // namespace cortex::mk3::tui
