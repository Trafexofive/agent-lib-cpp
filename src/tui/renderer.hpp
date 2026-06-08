// src/tui/renderer.hpp — TuiRenderer: 3-mode output renderer
// Mode 1 FULL RENDER: protocol events + markdown response (no raw XML)
// Mode 2 SEMI:        raw stream on left, protocol markers on right
// Mode 3 RAW:         raw LLM stream only
// Decoupled: receives text buffers, doesn't know about parser events
// MK3 TUI Framework
#pragma once

#include "components/protocol.hpp"
#include "components/markdown.hpp"
#include "terminal.hpp"
#include <string>
#include <vector>

namespace cortex::mk3::tui {

enum class RenderMode { FULL = 0, SEMI = 1, RAW = 2 };

class TuiRenderer {
public:
    TuiRenderer(int width = 80) : width_(width) { md_.setWidth(width - 4); }

    // ── Set content (called after each prompt) ──
    void setRawStream(const std::string& raw)  { raw_ = raw; }
    void setResponse(const std::string& text)  { response_ = text; }
    void appendResponse(const std::string& text) { response_ += text; }  // streaming
    void setThought(const std::string& text)   { thought_ = text; }

    // ── Box/border styling ──

    // Protocol actions/results (for FULL and SEMI modes)
    void addProtocolAction(const std::string& type, const std::string& name,
                           const std::string& id, const std::string& body, bool sync) {
        ActionType at = ActionType::TOOL;
        if (type == "agent") at = ActionType::AGENT;
        else if (type == "relic") at = ActionType::RELIC;
        else if (type == "feed") at = ActionType::FEED;
        pv_.addAction({at, name, id, body, sync});
    }

    void addProtocolResult(const std::string& id, bool ok,
                           const std::string& summary,
                           const std::string& toolName = "",
                           int exitCode = 0, double elapsedMs = 0,
                           size_t outputBytes = 0) {
        pv_.addResult({id, ok, summary, toolName, exitCode, elapsedMs, outputBytes});
    }

    // ── Mode control ──
    void setMode(RenderMode m) { mode_ = m; }
    RenderMode mode() const { return mode_; }
    static const char* modeName(RenderMode m) {
        switch(m) { case RenderMode::FULL: return "FULL"; case RenderMode::RAW: return "RAW"; }
        return "?";
    }

    // ── Render based on mode ──
    std::vector<std::string> render() {
        switch (mode_) {
            case RenderMode::RAW:  return renderRaw();
            default:               return renderFull();
        }
    }

    void clear() {
        raw_.clear(); response_.clear(); thought_.clear();
        pv_.clear();
    }

    void setWidth(int w) { width_ = w; md_.setWidth(w - 4); }

private:
    std::vector<std::string> renderFull() {
        std::vector<std::string> lines;
        // Model thinking (TTC) — stream dimmed, no header, appears first
        if (!thought_.empty()) {
            std::istringstream ts(thought_);
            std::string tl;
            while (std::getline(ts, tl)) {
                if (!tl.empty() && tl.back() == '\r') tl.pop_back();
                lines.push_back(ansi::dim() + tl + ansi::reset());
            }
        }
        // Protocol events (actions + results)
        for (auto& l : pv_.render(width_)) lines.push_back(l);
        // Response with live markdown rendering
        if (!response_.empty()) {
            md_.setText(response_);
            auto rendered = md_.render();
            bool hasContent = false;
            for (auto& l : rendered) { for (auto c : l) if (c != ' ' && c != '\n' && c != '\r') { hasContent = true; break; } }
            if (hasContent && !rendered.empty()) {
                lines.push_back(ansi::dim() + std::string("── Response ──") + ansi::reset());
                lines.insert(lines.end(), rendered.begin(), rendered.end());
            } else if (!rendered.empty()) {
                // Rendered output exists but was all whitespace — header only
                lines.push_back(ansi::dim() + std::string("── Response ──") + ansi::reset());
            } else if (!response_.empty()) {
                // Markdown rendered nothing — fall back to raw response text
                lines.push_back(ansi::dim() + std::string("── Response ──") + ansi::reset());
                lines.push_back(response_);
            }
        }
        return lines;
    }

    std::vector<std::string> renderRaw() {
        // Clean RAW mode — just show what the LLM generated
        std::vector<std::string> lines;
        std::istringstream rs(raw_);
        std::string rl;
        while (std::getline(rs, rl)) lines.push_back(ansi::dim() + rl + ansi::reset());
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
};

} // namespace cortex::mk3::tui
