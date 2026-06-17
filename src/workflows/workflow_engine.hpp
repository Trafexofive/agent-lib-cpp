// ─────────────────────────────────────────────────────────────────────────────
// Workflow Engine — loads manifest YAML, EXECUTES steps as a runtime sequence.
// Now delegates to sovereign Workflow objects (src/workflows/workflow.hpp).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "workflow.hpp"
#include <string>
#include <vector>
#include <map>
#include <regex>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <functional>
#include <json/json.h>
#include <future>
#include <mutex>

namespace cortex::mk3::workflows {

namespace fs = std::filesystem;

// ── Simple YAML-ish parser ──
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
        for (auto& c : parent.children)
            if (c.key == key) return c.value.empty() ? "" : c.value;
        return def;
    }

    static const Node* find(const Node& parent, const std::string& key) {
        for (auto& c : parent.children)
            if (c.key == key) return &c;
        return nullptr;
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
                Node item; item.isList = true; item.value = trimmed.substr(2);
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
            } else { idx++; }
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

// ═══════════════════════════════════════════════════════════════════════════
// WorkflowEngine — orchestrates Workflow objects, parses YAML, executes steps
// ═══════════════════════════════════════════════════════════════════════════
class WorkflowEngine {
public:
    static WorkflowEngine& instance() {
        static WorkflowEngine e;
        return e;
    }

    // ── Load workflow manifest from YAML path ──
    Workflow& load(const std::string& path) {
        // Check cache first
        auto cacheIt = manifestCache_.find(path);
        if (cacheIt != manifestCache_.end()) return cacheIt->second;

        WorkflowManifest wf;
        std::ifstream f(path);
        if (!f) { static Workflow empty; return empty; }

        std::string yaml((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
        auto root = MiniYaml::parse(yaml);
        wf.name = MiniYaml::get(root, "name");
        wf.version = MiniYaml::get(root, "version");
        wf.summary = MiniYaml::get(root, "summary");
        wf.description = MiniYaml::get(root, "description");

        auto* stepsNode = MiniYaml::find(root, "steps");
        if (stepsNode) for (auto& step : stepsNode->children) wf.steps.push_back(parseStep(step));

        auto* importNode = MiniYaml::find(root, "import");
        if (importNode) {
            auto* toolsNode = MiniYaml::find(*importNode, "tools");
            if (toolsNode) for (auto& t : toolsNode->children) if (!t.value.empty()) wf.importTools.push_back(t.value);
            auto* relicsNode = MiniYaml::find(*importNode, "relics");
            if (relicsNode) for (auto& r : relicsNode->children) if (!r.value.empty()) wf.importRelics.push_back(r.value);
        }
        auto* tagsNode = MiniYaml::find(root, "tags");
        if (tagsNode) for (auto& t : tagsNode->children) if (!t.value.empty()) wf.tags.push_back(t.value);

        manifestCache_.emplace(path, Workflow(wf));
        nameIndex_[wf.name] = path;
        return manifestCache_.at(path);
    }

    // ── Execute a loaded workflow ──
    WorkflowResult execute(const WorkflowManifest& wf,
                           const WorkflowRuntime& rt,
                           const Json::Value& inputParams = Json::Value()) {
        WorkflowResult result;
        result.workflowName = wf.name;
        result.success = true;

        auto t0 = std::chrono::steady_clock::now();

        // Symbol table: step_id → result JSON (populated as steps execute)
        std::map<std::string, Json::Value> symbols;

        // Seed with any input params under "input" key
        if (!inputParams.isNull() && !inputParams.empty()) {
            symbols["input"] = inputParams;
        }

        std::function<bool(const std::vector<WorkflowStep>&, WorkflowResult&)> execSteps;
        execSteps = [&](const std::vector<WorkflowStep>& steps, WorkflowResult& res) -> bool {
            for (auto& step : steps) {
                // Resolve params against current symbol table
                Json::Value resolvedParams = resolveParams(step.params, symbols);

                if (step.type == "tool") {
                    auto out = executeToolStep(step, resolvedParams, rt, symbols);
                    if (out.success) {
                        res.outputs[step.id] = symbols[step.id];
                        res.stepIds.push_back(step.id);
                        res.stepOutputs.push_back(symbols[step.id]);
                    }
                    if (!out.success) {
                        if (step.onError == "abort") {
                            res.success = false;
                            res.error = out.error;
                            return false;
                        }
                        res.diagnostics.push_back("step " + step.id + " failed: " + out.error);
                        if (step.onError == "skip") continue;
                    }
                } else if (step.type == "agent") {
                    auto out = executeAgentStep(step, resolvedParams, rt, symbols);
                    if (out.success) {
                        res.outputs[step.id] = symbols[step.id];
                        res.stepIds.push_back(step.id);
                        res.stepOutputs.push_back(symbols[step.id]);
                    }
                    if (!out.success) {
                        if (step.onError == "abort") {
                            res.success = false;
                            res.error = out.error;
                            return false;
                        }
                        res.diagnostics.push_back("step " + step.id + " failed: " + out.error);
                        if (step.onError == "skip") continue;
                    }
                } else if (step.type == "condition") {
                    bool condMet = evalCondition(step.condition, symbols);
                    if (condMet && !step.thenSteps.empty()) {
                        if (!execSteps(step.thenSteps, res)) return false;
                    } else if (!condMet && !step.elseSteps.empty()) {
                        if (!execSteps(step.elseSteps, res)) return false;
                    }
                } else if (step.type == "parallel") {
                    auto out = executeParallelSteps(step.steps, rt, symbols);
                    Json::Value combined;
                    for (auto& [id, v] : out) {
                        symbols[id] = v;
                        combined[id] = v;
                    }
                    symbols[step.id] = combined;
                    res.outputs[step.id] = combined;
                    res.stepIds.push_back(step.id);
                    res.stepOutputs.push_back(combined);
                } else if (step.type == "workflow") {
                    if (rt.executeWorkflow) {
                        auto subResult = rt.executeWorkflow(step.workflow, resolvedParams);
                        Json::Value wfVal;
                        wfVal["success"] = subResult.success;
                        wfVal["outputs"] = Json::Value(Json::objectValue);
                        for (auto& [k, v] : subResult.outputs)
                            wfVal["outputs"][k] = v;
                        symbols[step.id] = wfVal;
                        res.outputs[step.id] = wfVal;
                        res.stepIds.push_back(step.id);
                        res.stepOutputs.push_back(wfVal);
                        if (!subResult.success && step.onError == "abort") {
                            res.success = false;
                            res.error = subResult.error;
                            return false;
                        }
                    }
                } else if (step.type == "loop") {
                    int iter = 0;
                    while (iter < 100) {
                        if (!step.condition.empty() && evalCondition(step.condition, symbols)) break;
                        if (!execSteps(step.body, res)) return false;
                        iter++;
                    }
                }
            }
            return true;
        };

        execSteps(wf.steps, result);

        auto t1 = std::chrono::steady_clock::now();
        result.elapsedMs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;

        return result;
    }

    // ── Execute by Workflow object (convenience) ──
    WorkflowResult execute(const Workflow& workflow,
                           const WorkflowRuntime& rt,
                           const Json::Value& params = Json::Value()) {
        return execute(workflow.manifest(), rt, params);
    }

    // ── Cache access ──
    WorkflowManifest getCached(const std::string& pathOrName) {
        // Try exact path match first
        auto it = manifestCache_.find(pathOrName);
        if (it != manifestCache_.end()) return it->second.manifest();
        // Try name-based lookup (for dispatch by workflow name)
        auto nameIt = nameIndex_.find(pathOrName);
        if (nameIt != nameIndex_.end()) {
            auto cacheIt = manifestCache_.find(nameIt->second);
            if (cacheIt != manifestCache_.end()) return cacheIt->second.manifest();
        }
        return WorkflowManifest{};
    }

    /// Get the Workflow object by path or name
    const Workflow* findWorkflow(const std::string& pathOrName) {
        auto it = manifestCache_.find(pathOrName);
        if (it != manifestCache_.end()) return &it->second;
        auto nameIt = nameIndex_.find(pathOrName);
        if (nameIt != nameIndex_.end()) {
            auto cacheIt = manifestCache_.find(nameIt->second);
            if (cacheIt != manifestCache_.end()) return &cacheIt->second;
        }
        return nullptr;
    }

    /// List cached workflow names
    std::vector<std::string> listWorkflows() const {
        std::vector<std::string> names;
        for (const auto& [path, wf] : manifestCache_)
            names.push_back(wf.name());
        return names;
    }

    // ── Convenience: execute by name (load + run) ──
    WorkflowResult run(const std::string& nameOrPath,
                       const WorkflowRuntime& rt,
                       const Json::Value& params = Json::Value()) {
        WorkflowManifest wf = getCached(nameOrPath);
        if (wf.name.empty()) {
            // Try to load from filesystem
            if (fs::exists(nameOrPath)) {
                auto& loaded = load(nameOrPath);
                if (loaded.isValid()) {
                    return execute(loaded.manifest(), rt, params);
                }
            }
            WorkflowResult r;
            r.success = false;
            r.error = "Workflow not found: " + nameOrPath;
            return r;
        }
        return execute(wf, rt, params);
    }

    // ── Convert workflow to XML for prompt injection ──
    std::string toXml(const WorkflowManifest& wf) {
        return Workflow(wf).toXml();
    }

private:
    struct StepOutcome { bool success = false; std::string error; };

    // ── Variable resolution: ${step_id.field} ──
    static std::string resolveString(const std::string& s, const std::map<std::string, Json::Value>& symbols) {
        static const std::regex varRe(R"(\$\{(\w+)\.?(\w*)\})");
        std::smatch m;
        std::string res = s;
        std::string::const_iterator start = res.cbegin();
        std::string buf;
        while (std::regex_search(start, res.cend(), m, varRe)) {
            buf += m.prefix();
            std::string stepId = m[1];
            std::string field = m[2];
            auto it = symbols.find(stepId);
            if (it != symbols.end()) {
                const Json::Value& val = it->second;
                if (field.empty()) {
                    Json::StreamWriterBuilder w; w["indentation"] = "";
                    buf += Json::writeString(w, val);
                } else if (val.isMember(field)) {
                    if (val[field].isString()) buf += val[field].asString();
                    else { Json::StreamWriterBuilder w; w["indentation"] = ""; buf += Json::writeString(w, val[field]); }
                } else {
                    buf += "null";
                }
            } else {
                buf += "null";
            }
            start = m.suffix().first;
        }
        buf.append(start, res.cend());
        return buf;
    }

    static Json::Value resolveParams(const Json::Value& params, const std::map<std::string, Json::Value>& symbols) {
        if (params.isNull() || params.empty()) return params;

        Json::Value resolved;
        for (auto& key : params.getMemberNames()) {
            const Json::Value& val = params[key];
            if (val.isString()) {
                std::string expanded = resolveString(val.asString(), symbols);
                if (!expanded.empty() && (expanded.front() == '{' || expanded.front() == '[')) {
                    Json::Value parsed;
                    Json::CharReaderBuilder r;
                    std::string errs;
                    std::istringstream ss(expanded);
                    if (Json::parseFromStream(r, ss, &parsed, &errs))
                        resolved[key] = parsed;
                    else
                        resolved[key] = expanded;
                } else {
                    resolved[key] = expanded;
                }
            } else {
                resolved[key] = val;
            }
        }
        return resolved;
    }

    // ── Execute a single tool step ──
    StepOutcome executeToolStep(const WorkflowStep& step, const Json::Value& params,
                                 const WorkflowRuntime& rt,
                                 std::map<std::string, Json::Value>& symbols) {
        int attempts = 1 + step.maxRetries;
        StepOutcome out;
        for (int attempt = 0; attempt < attempts; attempt++) {
            if (!rt.executeTool) {
                out.error = "no tool executor configured";
                return out;
            }
            Json::Value result = rt.executeTool(step.tool, params);
            bool ok = result.get("success", false).asBool();

            if (ok) {
                symbols[step.id] = result;
                out.success = true;
                return out;
            }

            out.error = result.get("error", "tool failed").asString();
        }
        return out;
    }

    // ── Execute a single agent step ──
    StepOutcome executeAgentStep(const WorkflowStep& step, const Json::Value& params,
                                  const WorkflowRuntime& rt,
                                  std::map<std::string, Json::Value>& symbols) {
        int attempts = 1 + step.maxRetries;
        StepOutcome out;
        for (int attempt = 0; attempt < attempts; attempt++) {
            if (!rt.executeAgent) {
                out.error = "no agent executor configured";
                return out;
            }
            std::string instruction =
                params.isMember("instruction") ? params["instruction"].asString() :
                params.isMember("query") ? params["query"].asString() :
                "Execute task";
            Json::Value result = rt.executeAgent(step.agent, instruction);
            bool ok = result.get("success", false).asBool();

            if (ok) {
                symbols[step.id] = result;
                out.success = true;
                return out;
            }

            out.error = result.get("error", "agent failed").asString();
        }
        return out;
    }

    // ── Execute steps in parallel ──
    std::map<std::string, Json::Value> executeParallelSteps(
        const std::vector<WorkflowStep>& steps,
        const WorkflowRuntime& rt,
        std::map<std::string, Json::Value>& symbols) {
        std::map<std::string, Json::Value> results;
        std::vector<std::future<std::pair<std::string, Json::Value>>> futures;

        for (auto& step : steps) {
            futures.push_back(std::async(std::launch::async, [&rt, step, &symbols]() {
                Json::Value resolved = resolveParams(step.params, symbols);
                if (step.type == "tool" && rt.executeTool) {
                    Json::Value r = rt.executeTool(step.tool, resolved);
                    return std::make_pair(step.id, r);
                } else if (step.type == "agent" && rt.executeAgent) {
                    std::string instr = resolved.get("instruction", "Execute task").asString();
                    Json::Value r = rt.executeAgent(step.agent, instr);
                    return std::make_pair(step.id, r);
                }
                Json::Value err; err["success"] = false; err["error"] = "unsupported parallel step type";
                return std::make_pair(step.id, err);
            }));
        }

        for (auto& f : futures) {
            auto [id, result] = f.get();
            results[id] = result;
        }
        return results;
    }

    // ── Condition evaluation ──
    bool evalCondition(const std::string& condition,
                       const std::map<std::string, Json::Value>& symbols) {
        if (condition.empty()) return true;
        std::string cond = resolveString(condition, symbols);

        static const std::regex condRe(R"((\w+\.?\w*)\s*(==|!=|>=|<=|>|<)\s*(.+))");
        std::smatch m;
        if (std::regex_match(cond, m, condRe)) {
            std::string var = m[1];
            std::string op = m[2];
            std::string val = m[3];

            std::string resolved;
            size_t dot = var.find('.');
            if (dot != std::string::npos) {
                std::string stepId = var.substr(0, dot);
                std::string field = var.substr(dot + 1);
                auto it = symbols.find(stepId);
                if (it != symbols.end() && it->second.isMember(field))
                    resolved = it->second[field].asString();
            } else {
                auto it = symbols.find(var);
                if (it != symbols.end()) resolved = it->second.asString();
            }

            if (op == "==") return resolved == val;
            if (op == "!=") return resolved != val;
            return !resolved.empty();
        }

        std::string trimmed = cond;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\"'"));
        trimmed.erase(trimmed.find_last_not_of(" \t\"'") + 1);
        return !trimmed.empty() && trimmed != "false" && trimmed != "0";
    }

    // ── Parse step from mini-YAML node ──
    WorkflowStep parseStep(const MiniYaml::Node& node) {
        WorkflowStep s;
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
        try { s.maxRetries = std::stoi(getAttr("max_retries", "0")); } catch (...) { s.maxRetries = 0; }
        try { s.timeout = std::stoi(getAttr("timeout", "30")); } catch (...) { s.timeout = 30; }

        auto* paramsNode = MiniYaml::find(node, "params");
        if (paramsNode) for (auto& p : paramsNode->children) if (!p.key.empty()) s.params[p.key] = p.value;

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

    // ── Cache ──
    std::map<std::string, Workflow> manifestCache_;  // path → Workflow
    std::map<std::string, std::string> nameIndex_;    // workflow name → path
};

} // namespace cortex::mk3::workflows
