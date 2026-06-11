// ─────────────────────────────────────────────────────────────────────────────
// Mini YAML parser — supports the manifest subset we use (no anchors, aliases)
// Handles: block-style maps, block lists, inline flow maps { k: v, k2: v2 },
// block scalars (>  fold,  |  literal), and trailing inline comments.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <string>
#include <vector>
#include <sstream>

namespace cortex {
namespace mk3 {

class ManifestYaml {
public:
    struct Node {
        std::string key;
        std::string value;
        std::vector<Node> children;
        bool isList = false;
    };

    static Node parse(const std::string& yaml) {
        std::vector<std::string> lines;
        std::istringstream ss(yaml);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t firstNonWs = line.find_first_not_of(" \t");
            if (firstNonWs == std::string::npos) continue;       // blank
            if (line[firstNonWs] == '#') continue;               // full-line comment
            lines.push_back(stripInlineComment(line));
        }
        Node root;
        root.key = "root";
        parseLines(root, lines, 0, 0);
        return root;
    }

    static std::string get(const Node& parent, const std::string& key, const std::string& def = "") {
        for (auto& c : parent.children) {
            if (c.key == key) return c.value.empty() ? def : c.value;
        }
        return def;
    }

    static const Node* find(const Node& parent, const std::string& key) {
        for (auto& c : parent.children) {
            if (c.key == key) return &c;
        }
        return nullptr;
    }

    static std::vector<std::string> getList(const Node& parent, const std::string& key) {
        std::vector<std::string> result;
        auto* node = find(parent, key);
        if (!node) return result;
        for (auto& c : node->children) {
            if (!c.value.empty()) result.push_back(c.value);
        }
        if (result.empty()) {
            bool found = false;
            for (auto& c : parent.children) {
                if (&c == node) { found = true; continue; }
                if (found && c.key.empty() && !c.value.empty()) {
                    result.push_back(c.value);
                } else if (found && !c.key.empty()) {
                    break;
                }
            }
        }
        return result;
    }

private:
    // Strip ` #comment` from the line, respecting "..." and '...' quotes.
    static std::string stripInlineComment(const std::string& line) {
        bool inSingle = false, inDouble = false;
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (c == '\\' && i + 1 < line.size()) { ++i; continue; }
            if (!inSingle && c == '"') { inDouble = !inDouble; continue; }
            if (!inDouble && c == '\'') { inSingle = !inSingle; continue; }
            if (!inSingle && !inDouble && c == '#') {
                if (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t') {
                    size_t end = i;
                    while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\t')) --end;
                    return line.substr(0, end);
                }
            }
        }
        return line;
    }

    static std::string trimWs(const std::string& s) {
        size_t a = s.find_first_not_of(" \t");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t");
        return s.substr(a, b - a + 1);
    }

    // Split on `delim` at top level (skipping inside {}, [], "...", '...').
    static std::vector<std::string> splitTopLevel(const std::string& s, char delim) {
        std::vector<std::string> out;
        std::string cur;
        int depth = 0;
        bool inSingle = false, inDouble = false;
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (c == '\\' && i + 1 < s.size()) { cur += c; cur += s[++i]; continue; }
            if (!inSingle && c == '"') { inDouble = !inDouble; cur += c; continue; }
            if (!inDouble && c == '\'') { inSingle = !inSingle; cur += c; continue; }
            if (!inSingle && !inDouble) {
                if (c == '{' || c == '[') depth++;
                else if (c == '}' || c == ']') depth--;
                else if (c == delim && depth == 0) { out.push_back(cur); cur.clear(); continue; }
            }
            cur += c;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    // Parse inline flow map body `provider: x, model: y` → children of `kv`.
    static void parseInlineMap(Node& kv, const std::string& inner) {
        for (auto& entry : splitTopLevel(inner, ',')) {
            std::string e = trimWs(entry);
            if (e.empty()) continue;
            size_t c = e.find(':');
            if (c == std::string::npos) continue;
            Node child;
            child.key = trimWs(e.substr(0, c));
            child.value = trimQuotes(trimWs(e.substr(c + 1)));
            kv.children.push_back(child);
        }
    }

    static size_t parseLines(Node& parent, const std::vector<std::string>& lines,
                             size_t idx, int indent) {
        while (idx < lines.size()) {
            const auto& line = lines[idx];
            int lineIndent = 0;
            while (lineIndent < (int)line.size() && line[lineIndent] == ' ') lineIndent++;
            if (lineIndent < indent) return idx;

            std::string trimmed = line.substr(lineIndent);

            if (trimmed.rfind("- ", 0) == 0) {
                Node item;
                item.isList = true;
                item.value = trimQuotes(trimmed.substr(2));
                size_t colon = item.value.find(": ");
                if (colon != std::string::npos) {
                    item.key = item.value.substr(0, colon);
                    item.value = item.value.substr(colon + 2);
                }
                idx = parseLines(item, lines, idx + 1, lineIndent + 2);
                parent.children.push_back(item);
            } else if (trimmed.find(": ") != std::string::npos || trimmed.back() == ':') {
                Node kv;
                size_t colon = trimmed.find(": ");
                std::string rawValue;
                if (colon != std::string::npos) {
                    kv.key = trimmed.substr(0, colon);
                    rawValue = trimWs(trimmed.substr(colon + 2));
                } else {
                    kv.key = trimmed.substr(0, trimmed.size() - 1);
                }

                // Y02 — block scalars: `>` (fold) or `|` (literal)
                if (rawValue == ">" || rawValue == "|") {
                    char marker = rawValue[0];
                    std::string folded;
                    size_t j = idx + 1;
                    while (j < lines.size()) {
                        int jIndent = 0;
                        while (jIndent < (int)lines[j].size() && lines[j][jIndent] == ' ') jIndent++;
                        if (jIndent <= lineIndent) break;
                        std::string content = lines[j].substr(jIndent);
                        if (marker == '>') {
                            if (!folded.empty()) folded += ' ';
                            folded += content;
                        } else {
                            if (!folded.empty()) folded += '\n';
                            folded += content;
                        }
                        ++j;
                    }
                    kv.value = folded;
                    parent.children.push_back(kv);
                    idx = j;
                    continue;
                }

                // Y01 — inline flow map `{ a: b, c: d }`
                if (rawValue.size() >= 2 && rawValue.front() == '{' && rawValue.back() == '}') {
                    parseInlineMap(kv, rawValue.substr(1, rawValue.size() - 2));
                    parent.children.push_back(kv);
                    idx++;
                    continue;
                }

                // Y01b — inline flow list `[ a, b, c ]`
                if (rawValue.size() >= 2 && rawValue.front() == '[' && rawValue.back() == ']') {
                    std::string inner = rawValue.substr(1, rawValue.size() - 2);
                    for (auto& entry : splitTopLevel(inner, ',')) {
                        std::string e = trimQuotes(trimWs(entry));
                        if (e.empty()) continue;
                        Node child;
                        child.value = e;
                        kv.children.push_back(child);
                    }
                    parent.children.push_back(kv);
                    idx++;
                    continue;
                }

                kv.value = trimQuotes(rawValue);

                if (idx + 1 < lines.size()) {
                    int nextIndent = 0;
                    while (nextIndent < (int)lines[idx + 1].size() && lines[idx + 1][nextIndent] == ' ') nextIndent++;
                    if (nextIndent > lineIndent) {
                        idx = parseLines(kv, lines, idx + 1, nextIndent);
                        parent.children.push_back(kv);
                        continue;
                    }
                }
                parent.children.push_back(kv);
                idx++;
            } else {
                idx++;
            }
        }
        return idx;
    }

    static std::string trimQuotes(const std::string& s) {
        if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                               (s.front() == '\'' && s.back() == '\'')))
            return s.substr(1, s.size() - 2);
        return s;
    }
};

} // namespace mk3
} // namespace cortex
