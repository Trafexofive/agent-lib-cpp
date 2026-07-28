// src/tui/frame_clock.hpp — TUI frame pacing, dirty state, and spinner clock
#pragma once

#include <chrono>

namespace cortex::mk3::tui {

class FrameClock {
   public:
    explicit FrameClock(int minFrameMs = 16, int heartbeatMs = 80, int spinnerFrames = 10)
        : minFrameMs_(minFrameMs), heartbeatMs_(heartbeatMs), spinnerFrames_(spinnerFrames) {}

    void requestFrame() { dirty_ = true; }

    bool heartbeatDue(bool streaming) {
        if (!streaming)
            return false;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFrame_).count();
        if (elapsed < heartbeatMs_)
            return false;
        dirty_ = true;
        return true;
    }

    bool shouldRender(bool streaming, bool force) const {
        if (!streaming)
            return dirty_ || force;
        if (!dirty_ && !force)
            return false;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFrame_).count();
        return force || elapsed >= minFrameMs_;
    }

    void didRender(bool streaming) {
        dirty_ = false;
        if (!streaming)
            return;
        lastFrame_ = std::chrono::steady_clock::now();
        if (spinnerFrames_ > 0)
            spinnerFrame_ = (spinnerFrame_ + 1) % spinnerFrames_;
    }

    int spinnerFrame() const { return spinnerFrame_; }
    bool dirty() const { return dirty_; }

   private:
    int minFrameMs_ = 50;
    int heartbeatMs_ = 100;
    int spinnerFrames_ = 10;
    bool dirty_ = true;
    int spinnerFrame_ = 0;
    std::chrono::steady_clock::time_point lastFrame_ = std::chrono::steady_clock::now();
};

}  // namespace cortex::mk3::tui
