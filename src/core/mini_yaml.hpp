// ─────────────────────────────────────────────────────────────────────────────
// Mini YAML parser — supports the manifest subset we use (no anchors, aliases)
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
            if (line.empty() || line[0] == '#') continue;
            lines.push_back(line);
        }
        Node root;
        root.key = "root";
        parseLines(root, lines, 0, 0);
        return root;
    }

    static std::string get(const Node& parent, const std::string& key, const std::string& def = "") {
        for (auto& c : parent.children) {
            if (c.key == key) return c.value.empty() ? "" : c.value;
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
        // Check direct children first
        for (auto& c : node->children) {
            if (!c.value.empty()) result.push_back(c.value);
        }
        // If no children, look for sibling list items after this key
        if (result.empty()) {
            bool found = false;
            for (auto& c : parent.children) {
                if (&c == node) { found = true; continue; }
                if (found && c.key.empty() && !c.value.empty()) {
                    result.push_back(c.value);
                } else if (found && !c.key.empty()) {
                    break; // Next key — stop collecting
                }
            }
        }
        return result;
    }

private:
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
                if (colon != std::string::npos) {
                    kv.key = trimmed.substr(0, colon);
                    kv.value = trimQuotes(trimmed.substr(colon + 2));
                } else {
                    kv.key = trimmed.substr(0, trimmed.size() - 1);
                }
                if (idx + 1 < lines.size()) {
                    int nextIndent = 0;
                    while (nextIndent < (int)lines[idx+1].size() && lines[idx+1][nextIndent] == ' ') nextIndent++;
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
