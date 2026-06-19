// src/tui/status_prompt.hpp — status bar + prompt line rendering for MK3 REPL
#pragma once

#include <chrono>
#include <sstream>
#include <string>

#include "input.hpp"
#include "renderer.hpp"
#include "terminal.hpp"

namespace cortex::mk3::tui {

struct StatusBarState {
    bool dialogActive = false;
    bool streaming = false;
    int spinnerFrame = 0;
    std::chrono::steady_clock::time_point streamStart{};
    std::string phase = "idle";
    size_t actionCount = 0;
    size_t resultCount = 0;
    size_t responseBytes = 0;
    RenderMode mode = RenderMode::FULL;
    std::string provider;
    std::string model;
};

class StatusPromptRenderer {
   public:
    static std::string statusBar(const StatusBarState& state) {
        if (state.dialogActive)
            return ansi::dim() + "  Esc to cancel" + ansi::reset();

        std::ostringstream out;
        if (state.streaming) {
            static const char* spinnerFrames[] = {"⠋", "⠙", "⠹", "⠸", "⠼",
                                                   "⠴", "⠦", "⠧", "⠇", "⠏"};
            int idx = state.spinnerFrame % 10;
            out << ansi::fg(255, 200, 50) << spinnerFrames[idx] << ansi::reset() << " ";

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - state.streamStart)
                               .count();
            if (elapsed >= 1000)
                out << (elapsed / 1000) << "." << ((elapsed % 1000) / 100) << "s";
            else if (elapsed >= 100)
                out << "0." << (elapsed / 100) << "s";

            out << " " << state.phase << " act=" << state.actionCount
                << " done=" << state.resultCount << " " << state.responseBytes << "b";
        }

        out << ansi::dim() << "  " << TuiRenderer::modeName(state.mode) << " · "
            << state.provider << "/" << state.model << ansi::reset();
        return out.str();
    }

    static std::string promptLine(const Input& input, bool dialogActive) {
        std::ostringstream out;
        if (dialogActive) {
            out << ansi::dim() << "  Enter to submit" << ansi::reset();
            return out.str();
        }
        out << ansi::bold() << "▸ " << ansi::reset() << ansi::dim();
        if (input.searching()) {
            out << ansi::fg(255, 200, 0) << input.searchLine();
        } else {
            size_t cp = input.cursorPos();
            std::string line = input.line();
            out << line.substr(0, cp);
            out << "\033[7m" << (cp < line.size() ? std::string(1, line[cp]) : " ")
                << "\033[27m";
            if (cp < line.size())
                out << line.substr(cp + 1);
        }
        out << ansi::reset() << " ";
        return out.str();
    }
};

}  // namespace cortex::mk3::tui
