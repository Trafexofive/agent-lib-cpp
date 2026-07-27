#pragma once
// Shared XML / text helpers for prompt building and protocol emission.

#include <sstream>
#include <string>

namespace cortex::mk3 {

inline std::string xmlAttr(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
        case '"':
            out += "&quot;";
            break;
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

inline std::string indentText(const std::string &text, int spaces) {
    std::ostringstream out;
    std::istringstream in(text);
    std::string line;
    std::string pad(spaces, ' ');
    while (std::getline(in, line)) {
        if (!line.empty())
            out << pad << line;
        out << '\n';
    }
    return out.str();
}


}  // namespace cortex::mk3
