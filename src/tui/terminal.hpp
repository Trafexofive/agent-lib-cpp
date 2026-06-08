// src/tui/terminal.hpp — Terminal abstraction with raw ANSI escape sequences
// MK3 TUI Framework
#pragma once

#include <string>

namespace cortex::mk3::tui {

// ═══════════════════════════════════════════════════════════════════════
// ANSI escape sequence generators — pure string output
// ═══════════════════════════════════════════════════════════════════════
namespace ansi {

// Cursor visibility
inline const char* hideCursor()   { return "\033[?25l"; }
inline const char* showCursor()   { return "\033[?25h"; }

// Alternate screen buffer
inline const char* altScreenOn()  { return "\033[?1049h"; }
inline const char* altScreenOff() { return "\033[?1049l"; }

// Clear operations
inline const char* clearScreen()  { return "\033[2J"; }
inline const char* clearLine()    { return "\033[2K"; }
inline const char* clearToEnd()   { return "\033[0J"; }

// Cursor positioning (1-based row, 1-based col)
inline std::string moveTo(int row, int col) {
    return "\033[" + std::to_string(row) + ";" + std::to_string(col) + "H";
}
inline std::string moveUp(int n)    { return "\033[" + std::to_string(n) + "A"; }
inline std::string moveDown(int n)  { return "\033[" + std::to_string(n) + "B"; }
inline std::string moveRight(int n) { return "\033[" + std::to_string(n) + "C"; }
inline std::string moveLeft(int n)  { return "\033[" + std::to_string(n) + "D"; }

// True color (24-bit)
inline std::string fg(int r, int g, int b) {
    return "\033[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
}
inline std::string bg(int r, int g, int b) {
    return "\033[48;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
}

// 16-color backgrounds
inline const char* bgBlack()   { return "\033[40m"; }
inline const char* bgRed()     { return "\033[41m"; }
inline const char* bgGreen()   { return "\033[42m"; }
inline const char* bgYellow()  { return "\033[43m"; }
inline const char* bgBlue()    { return "\033[44m"; }
inline const char* bgMagenta() { return "\033[45m"; }
inline const char* bgCyan()    { return "\033[46m"; }
inline const char* bgWhite()   { return "\033[47m"; }
inline const char* bgDefault() { return "\033[49m"; }

// Text styling
inline const char* reset()         { return "\033[0m"; }
inline const char* bold()          { return "\033[1m"; }
inline const char* italic()        { return "\033[3m"; }
inline const char* underline()     { return "\033[4m"; }
inline const char* strikethrough() { return "\033[9m"; }
inline std::string dim()           { return "\033[2m"; }  // std::string: commonly chained with other ANSI calls

// Text wrappers (bold+reset etc)
inline std::string boldText(const std::string& s)   { return bold() + s + reset(); }
inline std::string dimText(const std::string& s)    { return dim() + s + reset(); }
inline std::string cyanText(const std::string& s)   { return "\033[36m" + s + reset(); }
inline std::string yellowText(const std::string& s) { return "\033[33m" + s + reset(); }
inline std::string reverseText(const std::string& s){ return "\033[7m" + s + reset(); }

} // namespace ansi

} // namespace cortex::mk3::tui
