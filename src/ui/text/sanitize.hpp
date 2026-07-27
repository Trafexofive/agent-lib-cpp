#pragma once
// =============================================================================
// Terminal-safe display text (foundation F2).
// Pure: no Agent, no inkcell Surface, no session I/O.
// =============================================================================

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace cortex::mk3::ui {

inline std::vector<std::string> splitDisplayLines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) out.push_back(line);
    if (out.empty()) out.push_back("");
    return out;
}

// Terminal-safe display sanitizer.
// - Strips C0 controls (keeps \n)
// - Validates UTF-8; replaces invalid / overlong / surrogate sequences with U+FFFD
// - Caps at 16 KiB head+tail
// - If >30% of sampled bytes are non-text (NUL/C0/invalid UTF-8), collapse to a
//   one-line binary marker so ELF/symbol dumps never paint the chat surface.
// - Collapses printable nm/c++filt symbol tables (Itanium mangling / libstdc++
//   dumps) — they pass the binary gate but stall the chat renderer.
inline std::string sanitizeForDisplay(const std::string& text) {
    constexpr std::size_t kCap = 16 * 1024;
    constexpr std::size_t kHead = 8 * 1024;
    constexpr std::size_t kTail = 4 * 1024;
    constexpr std::size_t kSample = 4096;

    // Printable symbol-table dump (nm / readelf / c++filt). Cheap scan.
    if (text.size() >= 160) {
        int mangled = 0;
        int cxx11 = 0;
        for (size_t i = 0; i + 3 < text.size(); ++i) {
            if (text[i] == '_' && text[i + 1] == 'Z' &&
                (text[i + 2] == 'N' || text[i + 2] == 'K' || text[i + 2] == 'T' ||
                 text[i + 2] == 'S' || text[i + 2] == 'I'))
                ++mangled;
            if (i + 14 <= text.size() && text.compare(i, 14, "std::__cxx11::") == 0)
                ++cxx11;
            if (mangled >= 4 || cxx11 >= 8) break;
        }
        if (mangled >= 4 || cxx11 >= 8) {
            return "  … [symbol dump · " + std::to_string(text.size()) +
                   " bytes · collapsed for chat] …";
        }
    }

    auto isNonTextByte = [](unsigned char c) -> bool {
        if (c == 0) return true;
        if (c < 0x09) return true;
        if (c >= 0x0B && c < 0x20) return true;
        if (c == 0x7F) return true;
        return false;
    };
    std::size_t sampleN = std::min(text.size(), kSample);
    std::size_t bad = 0;
    for (std::size_t i = 0; i < sampleN; ++i) {
        if (isNonTextByte(static_cast<unsigned char>(text[i]))) ++bad;
    }
    if (text.size() > kSample * 2) {
        std::size_t mid = text.size() / 2;
        for (std::size_t i = 0; i < 256 && mid + i < text.size(); ++i) {
            ++sampleN;
            if (isNonTextByte(static_cast<unsigned char>(text[mid + i]))) ++bad;
        }
    }
    if (sampleN > 32 && bad * 100 / sampleN > 30) {
        return "  … [binary/non-text data · " + std::to_string(text.size()) +
               " bytes · not shown in chat] …";
    }

    std::string out;
    out.reserve(std::min<std::size_t>(text.size(), kCap + 64));
    bool cut = false;
    std::size_t invalidUtf8 = 0;
    for (std::size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\r') {
            ++i;
            continue;
        }
        if (c == '\t') {
            out.push_back(' ');
            ++i;
            if (out.size() >= kCap) {
                cut = true;
                break;
            }
            continue;
        }
        if (c == '\n') {
            out.push_back('\n');
            ++i;
            if (out.size() >= kCap) {
                cut = true;
                break;
            }
            continue;
        }
        if (c < 0x20 || c == 0x7F) {
            out.push_back(' ');
            ++i;
            if (out.size() >= kCap) {
                cut = true;
                break;
            }
            continue;
        }
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            if (out.size() >= kCap) {
                cut = true;
                break;
            }
            continue;
        }
        int need = 0;
        uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) {
            need = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            need = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            need = 4;
            cp = c & 0x07;
        } else {
            out.append("\xEF\xBF\xBD");
            ++invalidUtf8;
            ++i;
            if (out.size() >= kCap) {
                cut = true;
                break;
            }
            continue;
        }
        if (i + static_cast<std::size_t>(need) > text.size()) {
            out.append("\xEF\xBF\xBD");
            ++invalidUtf8;
            break;
        }
        bool ok = true;
        for (int k = 1; k < need; ++k) {
            unsigned char cc = static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
            if ((cc & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (ok) {
            if (need == 2 && cp < 0x80) ok = false;
            if (need == 3 && cp < 0x800) ok = false;
            if (need == 4 && cp < 0x10000) ok = false;
            if (cp >= 0xD800 && cp <= 0xDFFF) ok = false;
            if (cp > 0x10FFFF) ok = false;
        }
        if (!ok) {
            out.append("\xEF\xBF\xBD");
            ++invalidUtf8;
            ++i;
            if (out.size() >= kCap) {
                cut = true;
                break;
            }
            continue;
        }
        out.append(text, i, static_cast<std::size_t>(need));
        i += static_cast<std::size_t>(need);
        if (out.size() >= kCap) {
            cut = true;
            break;
        }
    }

    if (invalidUtf8 > 32 && invalidUtf8 * 4 > out.size()) {
        return "  … [binary/non-text data · " + std::to_string(text.size()) +
               " bytes · not shown in chat] …";
    }

    if (!cut) return out;

    const std::size_t dropped = text.size() > out.size() ? text.size() - out.size() : 0;
    std::string trimmed;
    trimmed.reserve(kCap + 96);
    if (kHead < out.size())
        trimmed.append(out, 0, kHead);
    else
        trimmed = out;
    trimmed.append("\n  … [sanitize: dropped ");
    trimmed.append(std::to_string(dropped));
    trimmed.append(" bytes] …\n");
    if (out.size() > kHead) {
        std::size_t tailStart = out.size() > kTail ? out.size() - kTail : 0;
        trimmed.append(out, tailStart, out.size() - tailStart);
    }
    return trimmed;
}

// Truncate to a width with a unicode-aware ellipsis when needed.
// Display width is a best-effort byte heuristic; multi-byte UTF-8 counts as one cell.
inline std::string safeTruncate(const std::string& text, int maxCells) {
    if (maxCells <= 0) return {};
    int cells = 0;
    size_t i = 0;
    while (i < text.size() && cells < maxCells) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        size_t step = 1;
        if ((c & 0x80) == 0x80) {
            if ((c & 0xE0) == 0xC0)
                step = 2;
            else if ((c & 0xF0) == 0xE0)
                step = 3;
            else if ((c & 0xF8) == 0xF0)
                step = 4;
        }
        if (i + step > text.size()) step = text.size() - i;
        i += step;
        ++cells;
    }
    if (i >= text.size()) return text;
    if (maxCells - cells >= 1) return text + "…";
    return text.substr(0, i) + "…";
}

}  // namespace cortex::mk3::ui
