// ─────────────────────────────────────────────────────────────────────────────
// Workflow Engine — loads manifest YAML, injects as structured prompt context
// Workflows are agent-readable plans, NOT rigid DAG executors.
// The agent follows the plan using its normal <action>/<result> protocol.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <json/json.h>

namespace cortex {
namespace mk3 {
namespace workflows {

namespace fs = std::filesystem;

// ── Workflow step (parsed from YAML-ish manifest) ──
struct WorkflowStep {
    std::string id;
    std::string type;      // tool, agent, condition, loop, parallel, workflow
    std::string name;      // display name
    std::string tool;      // tool name (for type: tool)
    std::string agent;     // agent name (for type: agent)
    std::string condition; // for type: condition
    std::string workflow;  // for type: workflow
    Json::Value params;
    std::vector<WorkflowStep> thenSteps;
    std::vector<WorkflowStep> elseSteps;
    std::vector<WorkflowStep> body;      // loop body
    std::vector<WorkflowStep> steps;      // parallel/workflow steps
    std::string onError = "abort";
    int maxRetries = 0;
    int timeout = 30;
};

// ── Workflow manifest ──
struct WorkflowManifest {
    std::string name;
    std::string version;
    std::string summary;
    std::string description;
    std::vector<WorkflowStep> steps;
    std::vector<std::string> importTools;
    std::vector<std::string> importRelics;
    std::vector<std::string> tags;
};

// ── Simple YAML-ish parser (supports the workflow subset we need) ──
// We parse a simplified YAML: key: value, nested lists with -, indentation-based
class MiniYaml {
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

private:
    static size_t parseLines(Node& parent, const std::vector<std::string>& lines,
                             size_t idx, int indent) {
        while (idx < lines.size()) {
            const auto& line = lines[idx];
            int lineIndent = 0;
            while (lineIndent < (int)line.size() && line[lineIndent] == ' ') lineIndent++;
            if (lineIndent < indent) return idx; // back up a level

            std::string trimmed = line.substr(lineIndent);

            if (trimmed.rfind("- ", 0) == 0) {
                // List item
                Node item;
                item.isList = true;
                item.value = trimmed.substr(2);
                // Check for inline key:
                size_t colon = item.value.find(": ");
                if (colon != std::string::npos) {
                    item.key = item.value.substr(0, colon);
                    item.value = item.value.substr(colon + 2);
                }
                // Check next lines for indented children
                idx = parseLines(item, lines, idx + 1, lineIndent + 2);
                parent.children.push_back(item);
            } else if (trimmed.find(": ") != std::string::npos || trimmed.back() == ':') {
                // Key-value
                Node kv;
                size_t colon = trimmed.find(": ");
                if (colon != std::string::npos) {
                    kv.key = trimmed.substr(0, colon);
                    kv.value = trimQuotes(trimmed.substr(colon + 2));
                } else {
                    kv.key = trimmed.substr(0, trimmed.size() - 1);
                }
                // Check next lines for indented children
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
                // Value continuation or bare text
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

// ── Workflow loader ──
class WorkflowEngine {
public:
    static WorkflowEngine& instance() {
        static WorkflowEngine e;
        return e;
    }

    WorkflowManifest load(const std::string& path) {
        WorkflowManifest wf;
        std::ifstream f(path);
        if (!f) return wf;

        std::string yaml((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());

        auto root = MiniYaml::parse(yaml);

        wf.name = MiniYaml::get(root, "name");
        wf.version = MiniYaml::get(root, "version");
        wf.summary = MiniYaml::get(root, "summary");
        wf.description = MiniYaml::get(root, "description");

        // Parse steps
        auto* stepsNode = MiniYaml::find(root, "steps");
        if (stepsNode) {
            for (auto& step : stepsNode->children) {
                wf.steps.push_back(parseStep(step));
            }
        }

        // Import
        auto* importNode = MiniYaml::find(root, "import");
        if (importNode) {
            auto* toolsNode = MiniYaml::find(*importNode, "tools");
            if (toolsNode) {
                for (auto& t : toolsNode->children) {
                    if (!t.value.empty()) wf.importTools.push_back(t.value);
                }
            }
            auto* relicsNode = MiniYaml::find(*importNode, "relics");
            if (relicsNode) {
                for (auto& r : relicsNode->children) {
                    if (!r.value.empty()) wf.importRelics.push_back(r.value);
                }
            }
        }

        // Tags
        auto* tagsNode = MiniYaml::find(root, "tags");
        if (tagsNode) {
            for (auto& t : tagsNode->children) {
                if (!t.value.empty()) wf.tags.push_back(t.value);
            }
        }

        return wf;
    }

    // Convert workflow to XML for prompt injection
    std::string toXml(const WorkflowManifest& wf) {
        std::ostringstream ss;
        ss << "<workflow name=\"" << wf.name << "\" version=\"" << wf.version << "\">\n";
        if (!wf.summary.empty()) ss << "  <summary>" << wf.summary << "</summary>\n";
        if (!wf.description.empty()) ss << "  <description>" << wf.description << "</description>\n";
        if (!wf.steps.empty()) {
            ss << "  <steps>\n";
            for (auto& s : wf.steps) {
                stepToXml(ss, s, 4);
            }
            ss << "  </steps>\n";
        }
        ss << "</workflow>\n";
        return ss.str();
    }

private:
    WorkflowStep parseStep(const MiniYaml::Node& node) {
        WorkflowStep s;
        // For list items ("- id: discover"), the key is on the node itself
        // For regular nodes, keys are in children. Handle both.
        auto getAttr = [&](const std::string& key, const std::string& def = "") {
            if (node.key == key) return node.value;
            return MiniYaml::get(node, key, def);
        };
        s.id = getAttr("id");
        s.type = getAttr("type");
        s.name = getAttr("name");
        s.tool = getAttr("tool");
        s.agent = getAttr("agent");
        s.condition = getAttr("condition");
        s.workflow = getAttr("workflow");
        s.onError = getAttr("on_error", "abort");
        s.maxRetries = std::stoi(getAttr("max_retries", "0"));
        s.timeout = std::stoi(getAttr("timeout", "30"));

        // Parse params as JSON
        auto* paramsNode = MiniYaml::find(node, "params");
        if (paramsNode) {
            for (auto& p : paramsNode->children) {
                if (!p.key.empty()) s.params[p.key] = p.value;
            }
        }

        // Sub-steps
        auto* thenNode = MiniYaml::find(node, "then");
        if (thenNode) for (auto& st : thenNode->children) s.thenSteps.push_back(parseStep(st));
        auto* elseNode = MiniYaml::find(node, "else");
        if (elseNode) for (auto& st : elseNode->children) s.elseSteps.push_back(parseStep(st));
        auto* bodyNode = MiniYaml::find(node, "body");
        if (bodyNode) for (auto& st : bodyNode->children) s.body.push_back(parseStep(st));
        auto* stepsNode = MiniYaml::find(node, "steps");
        if (stepsNode) for (auto& st : stepsNode->children) s.steps.push_back(parseStep(st));

        return s;
    }

    void stepToXml(std::ostringstream& ss, const WorkflowStep& s, int indent) {
        std::string pad(indent, ' ');
        ss << pad << "<step id=\"" << s.id << "\" type=\"" << s.type << "\"";
        if (!s.name.empty()) ss << " name=\"" << s.name << "\"";
        if (!s.tool.empty()) ss << " tool=\"" << s.tool << "\"";
        if (!s.agent.empty()) ss << " agent=\"" << s.agent << "\"";
        if (!s.condition.empty()) ss << " condition=\"" << s.condition << "\"";
        if (s.onError != "abort") ss << " on_error=\"" << s.onError << "\"";
        if (s.maxRetries > 0) ss << " max_retries=\"" << s.maxRetries << "\"";
        ss << ">\n";

        // Render params if present
        if (!s.params.empty()) {
            Json::StreamWriterBuilder w;
            w["indentation"] = std::string(indent + 4, ' ');
            ss << Json::writeString(w, s.params) << "\n";
        }

        if (!s.thenSteps.empty()) {
            ss << pad << "  <then>\n";
            for (auto& st : s.thenSteps) stepToXml(ss, st, indent + 4);
            ss << pad << "  </then>\n";
        }
        if (!s.elseSteps.empty()) {
            ss << pad << "  <else>\n";
            for (auto& st : s.elseSteps) stepToXml(ss, st, indent + 4);
            ss << pad << "  </else>\n";
        }
        if (!s.body.empty()) {
            ss << pad << "  <body>\n";
            for (auto& st : s.body) stepToXml(ss, st, indent + 4);
            ss << pad << "  </body>\n";
        }
        if (!s.steps.empty()) {
            for (auto& st : s.steps) stepToXml(ss, st, indent + 2);
        }

        ss << pad << "</step>\n";
    }
};

} // namespace workflows
} // namespace mk3
} // namespace cortex
