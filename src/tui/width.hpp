// src/tui/width.hpp — ANSI-safe visible-width helpers for terminal rows.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "terminal.hpp"

namespace cortex::mk3::tui {

inline bool isWideCodepoint(uint32_t c) {
    if ((c >= 0x1100 && c <= 0x115F) || c == 0x2329 || c == 0x232A ||
        (c >= 0x2E80 && c <= 0xA4CF && c != 0x303F) || (c >= 0xAC00 && c <= 0xD7A3) ||
        (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE10 && c <= 0xFE19) ||
        (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60) ||
        (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x20000 && c <= 0x2FFFD) ||
        (c >= 0x30000 && c <= 0x3FFFD))
        return true;
    return (c >= 0x2500 && c <= 0x257F) || (c >= 0x2580 && c <= 0x259F) ||
           (c >= 0x2190 && c <= 0x21FF);
}

inline uint32_t readUtf8(const std::string& s, size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        ++i;
        return c;
    }
    uint32_t cp = 0;
    int extra = 0;
    if ((c & 0xE0) == 0xC0) {
        cp = c & 0x1F;
        extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
        cp = c & 0x0F;
        extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
        cp = c & 0x07;
        extra = 3;
    } else {
        ++i;
        return c;
    }
    while (extra-- > 0 && i + 1 < s.size()) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[++i]) & 0x3F);
    }
    ++i;
    return cp;
}

inline size_t visibleWidth(const std::string& s) {
    size_t width = 0;
    bool escape = false;
    for (size_t i = 0; i < s.size();) {
        if (escape) {
            if (s[i] == 'm')
                escape = false;
            ++i;
            continue;
        }
        if (static_cast<unsigned char>(s[i]) == 0x1B) {
            escape = true;
            ++i;
            continue;
        }
        uint32_t cp = readUtf8(s, i);
        width += isWideCodepoint(cp) ? 2 : 1;
    }
    return width;
}

inline std::string padRight(const std::string& s, int width) {
    const int used = static_cast<int>(visibleWidth(s));
    if (used >= width)
        return s + ansi::reset();
    return s + std::string(width - used, ' ') + ansi::reset();
}

}  // namespace cortex::mk3::tui
