#pragma once
// Reusable raw-mode single-select picker built on inkcell primitives.
// Owns terminal raw mode + input decoding + navigation + viewport paint.
// Decoupled from domain: render() decides what each row looks like.

#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "inkcell/ansi.hpp"
#include "inkcell/key.hpp"

namespace cortex::mk3::cli {

struct ListPickerConfig {
    std::string title;  // header line (bold cyan), may be empty
    std::string hint;   // dim key-bind hint under the title, may be empty
    int view_h = 20;    // visible rows (viewport scrolls beyond this)
    int initial = 0;    // starting selection
};

// Interactive single-select over `count` rows. `render(row, selected)` returns
// the display line for that row (may embed ANSI). Navigation: j / ↓ next,
// k / ↑ prev, 1-9 jump, d / u page down/up, g / G top/bottom, Enter select,
// Esc / Ctrl-C / Ctrl-D cancel. Returns selected index, or -1 on cancel.
// stdout must already be a TTY (caller checks isatty).
int run_list_picker(int count, const std::function<std::string(int row, bool selected)>& render,
                    const ListPickerConfig& cfg = {}) {
    if (count <= 0)
        return -1;

    struct termios oldt, raw;
    tcgetattr(STDIN_FILENO, &oldt);
    raw = oldt;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    ::printf("%s%s%s", inkcell::ansi::alt_screen_on().c_str(), inkcell::ansi::hide_cursor().c_str(),
             inkcell::ansi::clear_screen().c_str());
    ::fflush(stdout);

    std::function<void()> restore = [&]() {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
        ::printf("%s%s%s%s\n", inkcell::ansi::show_cursor().c_str(),
                 inkcell::ansi::clear_screen().c_str(), inkcell::ansi::move_to(1, 1).c_str(),
                 inkcell::ansi::alt_screen_off().c_str());
        ::fflush(stdout);
    };
    struct RestoreGuard {
        const std::function<void()>* f;
        ~RestoreGuard() {
            if (f)
                (*f)();
        }
    } guard{&restore};

    inkcell::KeyDecoder decoder;
    int sel = std::max(0, std::min(cfg.initial, count - 1));
    int offset = 0;
    int view_h = std::max(1, cfg.view_h);

    auto paint = [&]() {
        std::ostringstream out;
        out << inkcell::ansi::move_to(1, 1) << inkcell::ansi::clear_screen();
        if (!cfg.title.empty())
            out << inkcell::ansi::bold() << "\033[36m" << cfg.title << inkcell::ansi::reset()
                << "\r\n";
        if (!cfg.hint.empty())
            out << inkcell::ansi::dim() << cfg.hint << inkcell::ansi::reset() << "\r\n";
        int end = std::min(offset + view_h, count);
        for (int i = offset; i < end; ++i)
            out << render(i, i == sel) << "\r\n";
        if (count > view_h)
            out << inkcell::ansi::dim() << "─ " << (offset + 1) << "–" << end << " of " << count
                << inkcell::ansi::reset();
        else
            out << inkcell::ansi::dim() << "─ " << count << " item" << (count == 1 ? "" : "s")
                << inkcell::ansi::reset();
        ::fputs(out.str().c_str(), stdout);
        ::fflush(stdout);
    };

    auto read_event = [&]() -> inkcell::KeyEvent {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        select(STDIN_FILENO + 1, &fds, nullptr, nullptr, nullptr);  // block
        char buf[64];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0)
            return {};
        auto evs = decoder.feed(std::string(buf, buf + n));
        if (!evs.empty())
            return evs.front();
        if (!decoder.has_pending())
            return {};
        // Escape sequence may span reads — wait briefly for continuation.
        struct timeval tv{0, 20000};
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
            char buf2[64];
            ssize_t n2 = read(STDIN_FILENO, buf2, sizeof(buf2));
            auto evs2 = decoder.feed(std::string(buf2, n2 > 0 ? n2 : 0));
            if (!evs2.empty())
                return evs2.front();
        }
        auto flushed = decoder.flush();
        return flushed.empty() ? inkcell::KeyEvent{} : flushed.front();
    };

    while (true) {
        if (sel < offset)
            offset = sel;
        if (sel >= offset + view_h)
            offset = sel - view_h + 1;
        paint();
        auto ev = read_event();
        using inkcell::KeyCode;
        const auto is_char = ev.code == KeyCode::Character;
        if (ev.code == KeyCode::Enter)
            break;
        if (ev.code == KeyCode::Escape || ev.code == KeyCode::CtrlC || ev.code == KeyCode::CtrlD ||
            (is_char && (ev.ch == 'q' || ev.ch == 'Q'))) {
            sel = -1;  // cancel
            break;
        }
        if (ev.code == KeyCode::ArrowDown || (is_char && ev.ch == 'j')) {
            sel = (sel + 1) % count;
        } else if (ev.code == KeyCode::ArrowUp || (is_char && ev.ch == 'k')) {
            sel = (sel - 1 + count) % count;
        } else if (is_char && ev.ch == 'd') {
            sel = std::min(count - 1, sel + view_h);
        } else if (is_char && ev.ch == 'u') {
            sel = std::max(0, sel - view_h);
        } else if (is_char && ev.ch >= '1' && ev.ch <= '9' && ev.ch - '1' < count) {
            sel = ev.ch - '1';
        } else if (is_char && ev.ch == 'g') {
            sel = 0;
        } else if (is_char && ev.ch == 'G') {
            sel = count - 1;
        }
    }

    restore();
    return sel;
}

}  // namespace cortex::mk3::cli