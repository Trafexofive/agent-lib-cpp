// src/tui/components/markdown.hpp — Markdown renderer with ANSI styling
// MK3 TUI Framework — block-level + inline formatting + word-wrap
#pragma once

#include "../terminal.hpp"
#include <string>
#include <vector>
#include <regex>
#include <sstream>
#include <algorithm>

namespace cortex::mk3::tui {

enum class BlockState { Normal, FencedCode, Table };

class Markdown {
public:
    struct Theme {
        std::string heading; std::string bold; std::string italic;
        std::string code; std::string codeBlock; std::string quote;
        std::string link; std::string listBullet; std::string hr; std::string reset;
        std::string dim;
        static Theme defaultTheme() {
            Theme t;
            t.heading = ansi::bold(); t.bold = ansi::bold(); t.italic = ansi::italic();
            t.code = ansi::dim(); t.codeBlock = ansi::dim(); t.quote = ansi::dim();
            t.link = ansi::underline(); t.listBullet = ansi::dim(); t.hr = ansi::dim();
            t.reset = ansi::reset(); t.dim = ansi::dim(); return t;
        }
    };

    Markdown() : theme_(Theme::defaultTheme()) {}
    void setText(const std::string& text) { text_ = text; cached_ = false; }
    void setWidth(int cols) { width_ = cols; cached_ = false; }
    void setTheme(const Theme& theme) { theme_ = theme; cached_ = false; }
    void invalidate() { cached_ = false; }

    std::vector<std::string> render() const {
        if (cached_ && cached_width_ == width_) return cached_lines_;
        cached_lines_ = renderImpl(text_, width_);
        cached_width_ = width_; cached_ = true;
        return cached_lines_;
    }

private:
    // ═══════════════════════════════════════════════════════════════════════
    // Main render pipeline
    // ═══════════════════════════════════════════════════════════════════════
    std::vector<std::string> renderImpl(const std::string& text, int width) const {
        std::vector<std::string> lines;
        mathBlock_ = false;
        tableBuffer_.clear();
        if (text.empty()) return lines;

        BlockState state = BlockState::Normal;
        std::istringstream iss(text);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();

            // ── Block state: fenced code ──
            if (state == BlockState::FencedCode) {
                if (isFence(line)) { state = BlockState::Normal; continue; }
                std::string padded = line;
                size_t vis = 0; bool esc = false;
                for (char c : line) { if (c == '\033') esc = true; else if (esc) { if (c == 'm') esc = false; } else vis++; }
                while (vis++ < (size_t)width_) padded += ' ';
                lines.push_back(ansi::bg(20, 20, 40) + ansi::dim() + ansi::fg(0, 150, 200) + "│ " + ansi::reset() + ansi::bg(20, 20, 40) + ansi::dim() + padded + ansi::reset());
                continue;
            }

            // ── Block state: table ──
            if (state == BlockState::Table) {
                if (isTableRow(line)) {
                    tableBuffer_.push_back(line);
                    continue;
                }
                // Flush table buffer with column alignment
                if (!tableBuffer_.empty()) {
                    renderTableBuffer(lines, width);
                    tableBuffer_.clear();
                }
                state = BlockState::Normal;
            }

            // ── Fenced code start ──
            if (isFence(line)) {
                state = BlockState::FencedCode;
                std::string lang = extractFenceLang(line);
                if (!lang.empty()) lines.push_back(ansi::dim() + ansi::fg(0, 150, 200) + " ── " + lang + " ──" + ansi::reset());
                continue;
            }

            // ── Table ──
            if (isTableRow(line)) {
                state = BlockState::Table;
                tableBuffer_.push_back(line);
                continue;
            }

            // ── Header ──
            if (isHeader(line)) { auto hl = renderHeader(line); lines.insert(lines.end(), hl.begin(), hl.end()); continue; }

            // ── Horizontal rule ──
            if (isHR(line)) {
                lines.push_back(theme_.hr + std::string(std::min(width, 60), '-') + ansi::reset());
                continue;
            }

            // ── Blockquote ──
            if (isBlockquote(line)) { lines.push_back(renderBlockquote(line)); continue; }

            // ── Lists ──
            if (isUnorderedList(line)) {
                auto w = wrapText(renderUnorderedList(line), width);
                lines.insert(lines.end(), w.begin(), w.end()); continue;
            }
            if (isOrderedList(line)) {
                auto w = wrapText(renderOrderedList(line), width);
                lines.insert(lines.end(), w.begin(), w.end()); continue;
            }

            // ── Definition list (: definition) ──
            if (isDefinitionDef(line)) {
                std::string def = renderInline(line.substr(line.find(':') + 1));
                auto w = wrapText(ansi::dim() + "  " + def + ansi::reset(), width);
                lines.insert(lines.end(), w.begin(), w.end()); continue;
            }

            // ── Footnote definition ──
            if (isFootnote(line)) {
                lines.push_back(ansi::dim() + renderInline(line) + ansi::reset());
                continue;
            }

            // ── Math block ──
            if (line.rfind("$$", 0) == 0) {
                mathBlock_ = !mathBlock_;
                if (!mathBlock_) continue;  // closing $$
                continue;  // opening $$
            }
            if (mathBlock_) {
                lines.push_back(ansi::dim() + ansi::italic() + "  " + line + ansi::reset());
                continue;
            }

            // ── Normal paragraph ──
            auto w = wrapText(renderInline(line), width);
            lines.insert(lines.end(), w.begin(), w.end());
        }
        return lines;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Block detectors
    // ═══════════════════════════════════════════════════════════════════════
    static bool isFence(const std::string& line) { return line.rfind("```", 0) == 0; }
    static bool isHeader(const std::string& line) {
        return line.rfind("# ", 0) == 0 || line.rfind("## ", 0) == 0 ||
               line.rfind("### ", 0) == 0 || line.rfind("#### ", 0) == 0;
    }
    static bool isHR(const std::string& line) {
        std::string t = trim(line);
        return t == "---" || t == "***" || t == "___" || t == "- - -" || t == "* * *";
    }
    static bool isBlockquote(const std::string& line) { return line.rfind("> ", 0) == 0 || line.rfind(">", 0) == 0; }
    static bool isUnorderedList(const std::string& line) {
        if (line.length() < 2) return false;
        return (line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ';
    }
    static bool isOrderedList(const std::string& line) {
        size_t i = 0;
        while (i < line.length() && std::isdigit(line[i])) i++;
        return i > 0 && i < line.length() - 1 && line[i] == '.' && line[i + 1] == ' ';
    }
    static bool isTableRow(const std::string& line) { return line.find('|') != std::string::npos; }
    static bool isDefinitionDef(const std::string& line) {
        return line.rfind(": ", 0) == 0;
    }
    static bool isFootnote(const std::string& line) {
        return line.rfind("[^", 0) == 0 && line.find("]:") != std::string::npos;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Block renderers
    // ═══════════════════════════════════════════════════════════════════════
    std::vector<std::string> renderHeader(const std::string& line) const {
        std::vector<std::string> result;
        int level = 0;
        while (level < (int)line.length() && line[level] == '#') level++;
        std::string text = trim(line.substr(level + (level < (int)line.length() ? 1 : 0)));
        text = renderInline(text);
        size_t vis = visibleLen(text);
        if (level == 1) {
            result.push_back(ansi::bold() + ansi::cyanText(text) + ansi::reset());
            result.push_back(ansi::dim() + std::string(std::min((size_t)width_, vis + 4), '-') + ansi::reset());
        } else if (level == 2) {
            result.push_back(ansi::bold() + ansi::cyanText(text) + ansi::reset());
            result.push_back(ansi::dim() + std::string(std::min((size_t)width_/2, vis), '-') + ansi::reset());
        } else if (level >= 3) {
            result.push_back(ansi::boldText(ansi::yellowText(text)));
        } else {
            result.push_back(ansi::boldText(text));
        }
        return result;
    }

    std::string renderBlockquote(const std::string& line) const {
        // Count nesting level (>> text = level 2)
        int level = 0;
        size_t pos = 0;
        while (pos < line.length() && line[pos] == '>') { level++; pos++; }
        if (pos < line.length() && line[pos] == ' ') pos++;
        std::string content = (pos < line.length()) ? line.substr(pos) : "";
        std::string prefix;
        for (int i = 0; i < level; i++) prefix += ansi::dim() + "│ " + ansi::reset();
        return prefix + ansi::dim() + renderInline(content) + ansi::reset();
    }

    std::string renderUnorderedList(const std::string& line) const {
        size_t indent = 0;
        while (indent < line.length() && line[indent] == ' ') indent++;
        char bullet = line[indent];
        std::string content = line.substr(indent + 2);
        // Task list: - [x] or - [ ]
        if (content.rfind("[x] ", 0) == 0 || content.rfind("[X] ", 0) == 0) {
            return std::string(indent, ' ') + ansi::fg(0, 200, 0) + "[✓]" + ansi::reset() + " " + renderInline(content.substr(4));
        }
        if (content.rfind("[ ] ", 0) == 0) {
            return std::string(indent, ' ') + ansi::dim() + "[ ]" + ansi::reset() + " " + renderInline(content.substr(4));
        }
        return std::string(indent, ' ') + ansi::dim() + bullet + " " + ansi::reset() + renderInline(content);
    }

    std::string renderOrderedList(const std::string& line) const {
        size_t indent = 0;
        while (indent < line.length() && line[indent] == ' ') indent++;
        size_t numEnd = line.find(". ", indent);
        if (numEnd == std::string::npos) return renderInline(line);
        std::string number = line.substr(indent, numEnd - indent);
        std::string content = line.substr(numEnd + 2);
        return std::string(indent, ' ') + ansi::dim() + number + ". " + ansi::reset() + renderInline(content);
    }

    void renderTableBuffer(std::vector<std::string>& lines, int width) const {
        // Split all rows into columns and trim
        std::vector<std::vector<std::string>> rows;
        for (auto& row : tableBuffer_) {
            auto cols = split(row, '|');
            std::vector<std::string> trimmed;
            for (auto& c : cols) { std::string t = trim(c); if (!t.empty()) trimmed.push_back(t); }
            if (!trimmed.empty()) rows.push_back(trimmed);
        }
        if (rows.empty()) return;
        // Find max column count
        size_t ncols = 0;
        for (auto& r : rows) ncols = std::max(ncols, r.size());
        // Compute max width per column
        std::vector<size_t> colWidths(ncols, 0);
        for (auto& r : rows)
            for (size_t i = 0; i < r.size(); i++)
                colWidths[i] = std::max(colWidths[i], visibleLen(r[i]));
        // Render rows
        bool headerDone = false;
        for (auto& row : rows) {
            // Check separator row
            bool isSep = true;
            for (auto& c : row) {
                if (c.find_first_not_of("-: ") != std::string::npos) { isSep = false; break; }
            }
            if (isSep) {
                std::string sep;
                for (size_t i = 0; i < ncols; i++)
                    sep += "+" + std::string(colWidths[i] + 2, '-');
                sep += "+";
                lines.push_back(ansi::dim() + sep + ansi::reset());
                headerDone = true;
                continue;
            }
            // Data row
            std::string r = ansi::dim() + "│ " + ansi::reset();
            for (size_t i = 0; i < ncols; i++) {
                std::string cell = (i < row.size()) ? row[i] : "";
                std::string style = (!headerDone) ? ansi::bold() : "";
                r += style + cell + ansi::reset();
                if (i < ncols - 1) {
                    size_t pad = colWidths[i] - visibleLen(cell);
                    r += std::string(pad, ' ') + ansi::dim() + " │ " + ansi::reset();
                }
            }
            lines.push_back(r);
        }
    }

    size_t visibleLen(const std::string& s) const {
        size_t w = 0; bool esc = false;
        for (char c : s) {
            if (c == '\033') { esc = true; }
            else if (esc) { if (c == 'm') esc = false; }
            else if ((unsigned char)c >= 32) w++;
        }
        return w;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Inline formatting
    // ═══════════════════════════════════════════════════════════════════════
    std::string renderInline(const std::string& line) const {
        std::string result = line;
        static const std::regex boldRe(R"(\*\*(.+?)\*\*|__(.+?)__)");
        result = std::regex_replace(result, boldRe, theme_.bold + "$1$2" + theme_.reset);
        static const std::regex italicRe(R"((?:_([^_]+)_)|(?:\*([^*]+)\*))");
        result = std::regex_replace(result, italicRe, theme_.italic + "$1$2" + theme_.reset);
        static const std::regex codeRe(R"(`(.+?)`)");
        result = std::regex_replace(result, codeRe, theme_.code + "$1" + theme_.reset);
        static const std::regex strikeRe(R"(~~(.+?)~~)");
        result = std::regex_replace(result, strikeRe, std::string(ansi::strikethrough()) + "$1" + theme_.reset);
        // Math: $inline$ → dim italic
        static const std::regex mathRe(R"(\$([^$]+?)\$)");
        result = std::regex_replace(result, mathRe, ansi::dim() + ansi::italic() + "$1" + ansi::reset());
        // Links: [text](url) → text (url)
        static const std::regex linkRe(R"(\[([^\]]+)\]\(([^)]+)\))");
        result = std::regex_replace(result, linkRe, theme_.bold + "$1" + theme_.reset + " " + theme_.dim + "($2)" + theme_.reset);
        // Images: ![alt](url) → 🖼 alt
        static const std::regex imgRe(R"(!\[([^\]]*)\]\(([^)]+)\))");
        result = std::regex_replace(result, imgRe, theme_.dim + "🖼 $1" + theme_.reset);
        return result;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Word wrap
    // ═══════════════════════════════════════════════════════════════════════
    std::vector<std::string> wrapText(const std::string& text, int width) const {
        std::vector<std::string> lines;
        if (width <= 0) { lines.push_back(text); return lines; }
        std::string current;
        size_t pos = 0, visibleLen = 0;
        auto visibleWidth = [](const std::string& s) -> size_t {
            size_t w = 0; bool esc = false;
            for (char c : s) { if (c == '\033') esc = true; else if (esc) { if (c == 'm') esc = false; } else w++; }
            return w;
        };
        auto flush = [&]() { if (!current.empty()) { lines.push_back(current); current.clear(); visibleLen = 0; } };
        while (pos < text.size()) {
            if (text[pos] == '\033') {
                size_t end = text.find('m', pos);
                if (end != std::string::npos) { current += text.substr(pos, end - pos + 1); pos = end + 1; }
                else { current += text[pos++]; }
                continue;
            }
            size_t sp = text.find(' ', pos);
            if (sp == std::string::npos) sp = text.size();
            std::string word = text.substr(pos, sp - pos);
            size_t wv = visibleWidth(word);
            if (visibleLen + wv + (visibleLen > 0 ? 1 : 0) > (size_t)width && visibleLen > 0) { flush(); current = word; visibleLen = wv; }
            else { if (visibleLen > 0) { current += ' '; visibleLen++; } current += word; visibleLen += wv; }
            pos = sp + 1;
        }
        flush();
        if (lines.empty()) lines.push_back("");
        return lines;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Utilities
    // ═══════════════════════════════════════════════════════════════════════
    static std::string extractFenceLang(const std::string& line) {
        if (line.length() <= 3) return "";
        return trim(line.substr(3));
    }
    static std::string trim(const std::string& s) {
        size_t a = 0, b = s.length();
        while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
        return s.substr(a, b - a);
    }
    static std::vector<std::string> split(const std::string& s, char d) {
        std::vector<std::string> r; size_t st = 0, e = s.find(d);
        while (e != std::string::npos) { r.push_back(s.substr(st, e - st)); st = e + 1; e = s.find(d, st); }
        r.push_back(s.substr(st)); return r;
    }

    std::string text_;
    int width_ = 80;
    Theme theme_;
    mutable bool cached_ = false;
    mutable int cached_width_ = 0;
    mutable std::vector<std::string> cached_lines_;
    mutable std::vector<std::string> tableBuffer_;
    mutable bool mathBlock_ = false;
};

} // namespace cortex::mk3::tui
