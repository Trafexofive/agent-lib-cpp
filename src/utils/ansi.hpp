// =============================================================================
// agent-lib-MK3 — ANSI Terminal Escape Helpers
// =============================================================================
// Lightweight, zero-allocation ANSI escape code constants for terminal styling.
// All strings are compile-time constexpr. Use via `ansi::red << "text" << ansi::reset`.
//
// Features:
//   - Foreground/background colors (4-bit + 8-bit 256)
//   - Attributes: bold, dim, italic, underline, strikethrough, blink, reverse
//   - Cursor control: hide/show, move up/down, clear line/screen
//   - Combine: `ansi::bold << ansi::green << "bold green" << ansi::reset`
//
// Style guide for Cortex MK3:
//   - Headers/Banners  → bold + cyan
//   - Success/OK        → green
//   - Errors             → red
//   - Warnings           → yellow
//   - Info/neutral       → blue
//   - Tool calls/debug   → dim
//   - Code blocks        → dim + indent
//   - Inline code        → reverse/dim
//   - User input prompt  → bold + green
//   - Model names        → cyan
//
// ANSI disabled when stdout is not a TTY (checked at runtime via isatty).
// Use `--no-color` flag to force-disable.
// =============================================================================

#pragma once

#include <ostream>
#include <unistd.h>

namespace cortex {
namespace mk3 {
namespace ansi {

// ─────────────────────────────────────────────────────────────────────────────
// Runtime check
// ─────────────────────────────────────────────────────────────────────────────

/// Returns true if the output stream is a terminal and color should be used.
/// Respects NO_COLOR env var and TERM=dumb.
inline bool isTTY(FILE* fp = stdout) {
    static const char* term    = std::getenv("TERM");
    static const char* noColor = std::getenv("NO_COLOR");
    if (noColor || (term && std::string_view(term) == "dumb")) return false;
    return isatty(fileno(fp));
}

/// Force-disable colors globally (e.g., --no-color flag).
inline bool& colorEnabled() {
    static bool enabled = isTTY();
    return enabled;
}

// ─────────────────────────────────────────────────────────────────────────────
// Attributes
// ─────────────────────────────────────────────────────────────────────────────

constexpr const char* reset         = "\033[0m";
constexpr const char* bold          = "\033[1m";
constexpr const char* dim           = "\033[2m";
constexpr const char* italic        = "\033[3m";
constexpr const char* underline     = "\033[4m";
constexpr const char* blink         = "\033[5m";
constexpr const char* reverse       = "\033[7m";
constexpr const char* strikethrough = "\033[9m";
constexpr const char* boldOff       = "\033[22m";
constexpr const char* italicOff     = "\033[23m";
constexpr const char* underlineOff  = "\033[24m";

// ─────────────────────────────────────────────────────────────────────────────
// Foreground colors (4-bit)
// ─────────────────────────────────────────────────────────────────────────────

constexpr const char* black   = "\033[30m";
constexpr const char* red     = "\033[31m";
constexpr const char* green   = "\033[32m";
constexpr const char* yellow  = "\033[33m";
constexpr const char* blue    = "\033[34m";
constexpr const char* magenta = "\033[35m";
constexpr const char* cyan    = "\033[36m";
constexpr const char* white   = "\033[37m";

// Bright foreground
constexpr const char* brightBlack   = "\033[90m";
constexpr const char* brightRed     = "\033[91m";
constexpr const char* brightGreen   = "\033[92m";
constexpr const char* brightYellow  = "\033[93m";
constexpr const char* brightBlue    = "\033[94m";
constexpr const char* brightMagenta = "\033[95m";
constexpr const char* brightCyan    = "\033[96m";
constexpr const char* brightWhite   = "\033[97m";

// ─────────────────────────────────────────────────────────────────────────────
// Background colors (4-bit)
// ─────────────────────────────────────────────────────────────────────────────

constexpr const char* bgBlack   = "\033[40m";
constexpr const char* bgRed     = "\033[41m";
constexpr const char* bgGreen   = "\033[42m";
constexpr const char* bgYellow  = "\033[43m";
constexpr const char* bgBlue    = "\033[44m";
constexpr const char* bgMagenta = "\033[45m";
constexpr const char* bgCyan    = "\033[46m";
constexpr const char* bgWhite   = "\033[47m";

// ─────────────────────────────────────────────────────────────────────────────
// Cursor control
// ─────────────────────────────────────────────────────────────────────────────

constexpr const char* cursorHide  = "\033[?25l";
constexpr const char* cursorShow  = "\033[?25h";
constexpr const char* clearLine   = "\033[2K";
constexpr const char* clearScreen = "\033[2J\033[H";

inline std::string cursorUp(int n)    { return "\033[" + std::to_string(n) + "A"; }
inline std::string cursorDown(int n)  { return "\033[" + std::to_string(n) + "B"; }
inline std::string cursorRight(int n) { return "\033[" + std::to_string(n) + "C"; }
inline std::string cursorLeft(int n)  { return "\033[" + std::to_string(n) + "D"; }

/// Move cursor to column (1-indexed).
inline std::string cursorCol(int col) { return "\033[" + std::to_string(col) + "G"; }

// ─────────────────────────────────────────────────────────────────────────────
// 256-color helpers
// ─────────────────────────────────────────────────────────────────────────────

/// 256-color foreground (0-255).
inline std::string fg256(int code) {
    return "\033[38;5;" + std::to_string(code) + "m";
}

/// 256-color background (0-255).
inline std::string bg256(int code) {
    return "\033[48;5;" + std::to_string(code) + "m";
}

/// Truecolor (24-bit) foreground.
inline std::string fgRGB(int r, int g, int b) {
    return "\033[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
}

/// Truecolor (24-bit) background.
inline std::string bgRGB(int r, int g, int b) {
    return "\033[48;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
}

// ─────────────────────────────────────────────────────────────────────────────
// Streaming helper — wraps a string_view with a color prefix
// ─────────────────────────────────────────────────────────────────────────────

/// RAII-style color guard: streams prefix on construction, reset on destruction.
/// Usage: `os << ColorGuard(ansi::red) << "error text" << ColorGuard::end();`
struct ColorGuard {
    const char* code;
    explicit ColorGuard(const char* c) : code(c) {}
    friend std::ostream& operator<<(std::ostream& os, const ColorGuard& g) {
        if (colorEnabled()) os << g.code;
        return os;
    }
    static ColorGuard end() { return ColorGuard(reset); }
};

// ─────────────────────────────────────────────────────────────────────────────
// Convenience: wrap a string in color if enabled
// ─────────────────────────────────────────────────────────────────────────────

/// Return a color-wrapped string. If color is disabled, returns original.
inline std::string wrap(const char* code, const std::string& text) {
    if (!colorEnabled()) return text;
    return std::string(code) + text + reset;
}

inline std::string redText(const std::string& t)    { return wrap(red, t); }
inline std::string greenText(const std::string& t)  { return wrap(green, t); }
inline std::string yellowText(const std::string& t)  { return wrap(yellow, t); }
inline std::string blueText(const std::string& t)   { return wrap(blue, t); }
inline std::string cyanText(const std::string& t)   { return wrap(cyan, t); }
inline std::string magentaText(const std::string& t){ return wrap(magenta, t); }
inline std::string dimText(const std::string& t)    { return wrap(dim, t); }
inline std::string boldText(const std::string& t)   { return wrap(bold, t); }
inline std::string reverseText(const std::string& t){ return wrap(reverse, t); }

// ─────────────────────────────────────────────────────────────────────────────
// Spinner frames
// ─────────────────────────────────────────────────────────────────────────────

constexpr const char* spinnerFrames[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
constexpr int spinnerFrameCount = 10;

/// Advance spinner frame index (0-9). Returns next frame index for use with spinnerFrames.
inline int nextSpinnerFrame(int current) { return (current + 1) % spinnerFrameCount; }

} // namespace ansi
} // namespace mk3
} // namespace cortex
