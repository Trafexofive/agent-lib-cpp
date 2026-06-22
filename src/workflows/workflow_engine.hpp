// ─────────────────────────────────────────────────────────────────────────────
// Workflow Engine — loads manifest YAML, EXECUTES steps as a runtime sequence.
// Now delegates to sovereign Workflow objects (src/workflows/workflow.hpp).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <json/json.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "workflow.hpp"

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
            if (line.empty() || line[0] == '#')
                continue;
            lines.push_back(line);
        }
        Node root;
        root.key = "root";
        parseLines(root, lines, 0, 0);
        return root;
    }

    static std::string get(const Node& parent, const std::string& key,
                           const std::string& def = "") {
        for (auto& c : parent.children)
            if (c.key == key)
                return c.value.empty() ? "" : c.value;
        return def;
    }

    static const Node* find(const Node& parent, const std::string& key) {
        for (auto& c : parent.children)
            if (c.key == key)
                return &c;
        return nullptr;
    }

   private:
    static size_t parseLines(Node& parent, const std::vector<std::string>& lines, size_t idx,
                             int indent) {
        while (idx < lines.size()) {
            const auto& line = lines[idx];
            int lineIndent = 0;
            while (lineIndent < (int)line.size() && line[lineIndent] == ' ')
                lineIndent++;
            if (lineIndent < indent)
                return idx;

            std::string trimmed = line.substr(lineIndent);
            if (trimmed.rfind("- ", 0) == 0) {
                Node item;
                item.isList = true;
                item.value = trimmed.substr(2);
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
                    while (nextIndent < (int)lines[idx + 1].size() &&
                           lines[idx + 1][nextIndent] == ' ')
                        nextIndent++;
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
        if (s.size() >= 2 &&
            ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')))
            return s.substr(1, s.size() - 2);
        return s;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// ═══════════════════════════════════════════════════════════════════════════
// SchemaValidator — slice 1: minimal JSON Schema validator
//
// Supports: type, required, properties, items, enum.
// Does NOT support: $ref, allOf, oneOf, anyOf, pattern, format, etc.
// Add features incrementally as needed.
// ═══════════════════════════════════════════════════════════════════════════
struct ValidationError {
    std::string path;     // e.g. "input.environment"
    std::string message;  // e.g. "must be one of [staging, production]"
};

class SchemaValidator {
   public:
    // Validate `value` against `schema`. Returns empty vector on success.
    static std::vector<ValidationError> validate(const Json::Value& value,
                                                  const Json::Value& schema,
                                                  const std::string& basePath = "") {
        std::vector<ValidationError> errors;
        validateInto(value, schema, basePath, errors);
        return errors;
    }

   private:
    static void validateInto(const Json::Value& v, const Json::Value& s,
                              const std::string& p, std::vector<ValidationError>& e) {
        if (s.isNull() || !s.isObject())
            return;

        if (s.isMember("type")) {
            std::string expected = s["type"].asString();
            if (!typeMatches(v, expected))
                e.push_back({p, "expected type " + expected});
        }

        if (s.isMember("enum") && s["enum"].isArray()) {
            bool found = false;
            for (const auto& opt : s["enum"]) {
                if (v == opt) { found = true; break; }
            }
            if (!found) {
                std::string opts;
                for (Json::ArrayIndex i = 0; i < s["enum"].size(); i++) {
                    if (i) opts += ", ";
                    opts += s["enum"][i].asString();
                }
                e.push_back({p, "must be one of [" + opts + "]"});
            }
        }

        if (v.isObject() && s.isMember("required") && s["required"].isArray()) {
            for (const auto& req : s["required"]) {
                std::string key = req.asString();
                if (!v.isMember(key))
                    e.push_back({p.empty() ? key : p + "." + key, "is required"});
            }
        }

        if (v.isObject() && s.isMember("properties") && s["properties"].isObject()) {
            for (const auto& key : v.getMemberNames()) {
                if (s["properties"].isMember(key))
                    validateInto(v[key], s["properties"][key],
                                  p.empty() ? key : p + "." + key, e);
            }
        }

        if (v.isArray() && s.isMember("items") && s["items"].isObject()) {
            for (Json::ArrayIndex i = 0; i < v.size(); i++) {
                validateInto(v[i], s["items"],
                              p + "[" + std::to_string(i) + "]", e);
            }
        }
    }

    static bool typeMatches(const Json::Value& v, const std::string& t) {
        if (t == "object") return v.isObject();
        if (t == "array") return v.isArray();
        if (t == "string") return v.isString();
        if (t == "number") return v.isNumeric() && !v.isBool();
        if (t == "integer") return v.isIntegral() && !v.isBool();
        if (t == "boolean") return v.isBool();
        if (t == "null") return v.isNull();
        return true;
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
        if (cacheIt != manifestCache_.end())
            return cacheIt->second;

        WorkflowManifest wf;
        std::ifstream f(path);
        if (!f) {
            static Workflow empty;
            return empty;
        }

        std::string yaml((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto root = MiniYaml::parse(yaml);
        wf.name = MiniYaml::get(root, "name");
        wf.version = MiniYaml::get(root, "version");
        wf.summary = MiniYaml::get(root, "summary");
        wf.description = MiniYaml::get(root, "description");

        auto* stepsNode = MiniYaml::find(root, "steps");
        if (stepsNode)
            for (auto& step : stepsNode->children)
                wf.steps.push_back(parseStep(step));

        // Slice 1: input/output schemas
        wf.inputSchema = parseYamlValue(MiniYaml::find(root, "input_schema"));
        wf.outputSchema = parseYamlValue(MiniYaml::find(root, "output_schema"));

        // Slice 13: inheritance
        wf.extends = MiniYaml::get(root, "extends");

        // Slice 10: auto-checkpoint
        std::string ac = MiniYaml::get(root, "auto_checkpoint");
        wf.autoCheckpoint = (ac == "true" || ac == "1" || ac == "yes");

        // Slice 9: feed triggers (top-level)
        auto* triggersNode = MiniYaml::find(root, "triggers");
        if (triggersNode)
            for (auto& t : triggersNode->children)
                if (!t.value.empty())
                    wf.triggers.push_back(t.value);

        auto* importNode = MiniYaml::find(root, "import");
        if (importNode) {
            auto* toolsNode = MiniYaml::find(*importNode, "tools");
            if (toolsNode)
                for (auto& t : toolsNode->children)
                    if (!t.value.empty())
                        wf.importTools.push_back(t.value);
            auto* relicsNode = MiniYaml::find(*importNode, "relics");
            if (relicsNode)
                for (auto& r : relicsNode->children)
                    if (!r.value.empty())
                        wf.importRelics.push_back(r.value);
        }
        auto* tagsNode = MiniYaml::find(root, "tags");
        if (tagsNode)
            for (auto& t : tagsNode->children)
                if (!t.value.empty())
                    wf.tags.push_back(t.value);

        manifestCache_.emplace(path, Workflow(wf));
        nameIndex_[wf.name] = path;
        return manifestCache_.at(path);
    }

    // ── Execute a loaded workflow ──
    WorkflowResult execute(const WorkflowManifest& wf, const WorkflowRuntime& rt,
                           const Json::Value& inputParams = Json::Value()) {
        WorkflowResult result;
        result.workflowName = wf.name;
        result.success = true;

        // Slice 1: input schema validation
        if (!wf.inputSchema.isNull() && wf.inputSchema.isObject()) {
            auto errs = SchemaValidator::validate(inputParams, wf.inputSchema, "input");
            if (!errs.empty()) {
                result.success = false;
                for (auto& e : errs)
                    result.diagnostics.push_back("input schema: " + e.path + " " + e.message);
                result.error = "input validation failed (" + std::to_string(errs.size()) + " errors)";
                return result;
            }
        }

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

                // Slice 1: per-step params schema validation
                if (!step.paramsSchema.isNull() && step.paramsSchema.isObject()) {
                    auto errs = SchemaValidator::validate(resolvedParams, step.paramsSchema, step.id);
                    if (!errs.empty()) {
                        for (auto& e : errs)
                            res.diagnostics.push_back("step " + step.id + " params: " + e.path + " " + e.message);
                        if (step.onError == "abort") {
                            res.success = false;
                            res.error = "step " + step.id + " params validation failed";
                            return false;
                        }
                    }
                }

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
                        if (step.onError == "skip")
                            continue;
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
                        if (step.onError == "skip")
                            continue;
                    }
                } else if (step.type == "condition") {
                    bool condMet = evalCondition(step.condition, symbols);
                    if (condMet && !step.thenSteps.empty()) {
                        if (!execSteps(step.thenSteps, res))
                            return false;
                    } else if (!condMet && !step.elseSteps.empty()) {
                        if (!execSteps(step.elseSteps, res))
                            return false;
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
                        if (!step.condition.empty() && evalCondition(step.condition, symbols))
                            break;
                        if (!execSteps(step.body, res))
                            return false;
                        iter++;
                    }
                }
            }
            return true;
        };

        execSteps(wf.steps, result);

        auto t1 = std::chrono::steady_clock::now();
        result.elapsedMs =
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;

        return result;
    }

    // ── Execute by Workflow object (convenience) ──
    WorkflowResult execute(const Workflow& workflow, const WorkflowRuntime& rt,
                           const Json::Value& params = Json::Value()) {
        return execute(workflow.manifest(), rt, params);
    }

    // ── Cache access ──
    WorkflowManifest getCached(const std::string& pathOrName) {
        // Try exact path match first
        auto it = manifestCache_.find(pathOrName);
        if (it != manifestCache_.end())
            return it->second.manifest();
        // Try name-based lookup (for dispatch by workflow name)
        auto nameIt = nameIndex_.find(pathOrName);
        if (nameIt != nameIndex_.end()) {
            auto cacheIt = manifestCache_.find(nameIt->second);
            if (cacheIt != manifestCache_.end())
                return cacheIt->second.manifest();
        }
        return WorkflowManifest{};
    }

    /// Get the Workflow object by path or name
    const Workflow* findWorkflow(const std::string& pathOrName) {
        auto it = manifestCache_.find(pathOrName);
        if (it != manifestCache_.end())
            return &it->second;
        auto nameIt = nameIndex_.find(pathOrName);
        if (nameIt != nameIndex_.end()) {
            auto cacheIt = manifestCache_.find(nameIt->second);
            if (cacheIt != manifestCache_.end())
                return &cacheIt->second;
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
    WorkflowResult run(const std::string& nameOrPath, const WorkflowRuntime& rt,
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
    struct StepOutcome {
        bool success = false;
        std::string error;
    };

    // ── Variable resolution: ${step_id.field} ──
    static std::string resolveString(const std::string& s,
                                     const std::map<std::string, Json::Value>& symbols) {
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
                    Json::StreamWriterBuilder w;
                    w["indentation"] = "";
                    buf += Json::writeString(w, val);
                } else if (val.isMember(field)) {
                    if (val[field].isString())
                        buf += val[field].asString();
                    else {
                        Json::StreamWriterBuilder w;
                        w["indentation"] = "";
                        buf += Json::writeString(w, val[field]);
                    }
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

    static Json::Value resolveParams(const Json::Value& params,
                                     const std::map<std::string, Json::Value>& symbols) {
        if (params.isNull() || params.empty())
            return params;

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
    // Read an optional bool field from a JSON params blob, defaulting to
    // false. Accepts `true`/`false` strings and real booleans.
    static bool readBoolParam(const Json::Value& params, const std::string& key) {
        if (!params.isMember(key))
            return false;
        const auto& v = params[key];
        if (v.isBool())
            return v.asBool();
        if (v.isString()) {
            const auto& s = v.asString();
            return s == "true" || s == "1" || s == "yes";
        }
        return false;
    }

    // Build a WorkflowAgentInvocation from a step's resolved params. The
    // instruction field falls back to "query" or "Execute task" so existing
    // step definitions keep working.
    static WorkflowAgentInvocation makeAgentInvocation(const std::string& name,
                                                        const Json::Value& params) {
        WorkflowAgentInvocation inv;
        inv.name = name;
        if (params.isMember("instruction") && params["instruction"].isString())
            inv.instruction = params["instruction"].asString();
        else if (params.isMember("query") && params["query"].isString())
            inv.instruction = params["query"].asString();
        else
            inv.instruction = "Execute task";
        inv.ephemeral = readBoolParam(params, "ephemeral");
        inv.dumpContext = readBoolParam(params, "dump_context");
        return inv;
    }

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
            WorkflowAgentInvocation inv = makeAgentInvocation(step.agent, params);
            Json::Value result = rt.executeAgent(inv);
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
        const std::vector<WorkflowStep>& steps, const WorkflowRuntime& rt,
        std::map<std::string, Json::Value>& symbols) {
        std::map<std::string, Json::Value> results;
        std::vector<std::future<std::pair<std::string, Json::Value>>> futures;

        // Symbols is captured by value (a const copy) so parallel tasks
        // can read it without contending on the parent's map. Writes to
        // symbols happen only after every future has completed and the
        // results have been collected into the local `results` map.
        for (auto& step : steps) {
            futures.push_back(std::async(std::launch::async, [&rt, step, symbols]() {
                Json::Value resolved = resolveParams(step.params, symbols);
                if (step.type == "tool" && rt.executeTool) {
                    Json::Value r = rt.executeTool(step.tool, resolved);
                    return std::make_pair(step.id, r);
                } else if (step.type == "agent" && rt.executeAgent) {
                    WorkflowAgentInvocation inv = makeAgentInvocation(step.agent, resolved);
                    Json::Value r = rt.executeAgent(inv);
                    return std::make_pair(step.id, r);
                }
                Json::Value err;
                err["success"] = false;
                err["error"] = "unsupported parallel step type";
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
        if (condition.empty())
            return true;
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
                if (it != symbols.end())
                    resolved = it->second.asString();
            }

            if (op == "==")
                return resolved == val;
            if (op == "!=")
                return resolved != val;
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
            if (node.key == key)
                return node.value;
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
        try {
            s.maxRetries = std::stoi(getAttr("max_retries", "0"));
        } catch (...) {
            s.maxRetries = 0;
        }
        try {
            s.timeout = std::stoi(getAttr("timeout", "30"));
        } catch (...) {
            s.timeout = 30;
        }

        auto* paramsNode = MiniYaml::find(node, "params");
        if (paramsNode)
            for (auto& p : paramsNode->children)
                if (!p.key.empty())
                    s.params[p.key] = p.value;

        // Slices 2-6: per-step options
        s.prompt = getAttr("prompt");
        s.defaultValue = getAttr("default");
        s.responseVar = getAttr("response_var");
        try { s.humanTimeoutSec = std::stoi(getAttr("timeout_sec", "0")); } catch (...) {}
        s.relic = getAttr("relic");
        s.action = getAttr("action");
        s.feed = getAttr("feed");
        s.emitEvent = getAttr("event");
        if (MiniYaml::find(node, "payload"))
            s.emitPayload = parseYamlValue(MiniYaml::find(node, "payload"));
        s.over = getAttr("over");
        s.asVar = getAttr("as", "item");
        s.accVar = getAttr("acc_var", "acc");
        s.initial = getAttr("initial");
        s.switchOn = getAttr("on");
        s.checkpointState = getAttr("state");
        s.checkpointMessage = getAttr("message");
        s.returnValue = getAttr("value");

        // Slice 1: per-step params schema
        s.paramsSchema = parseYamlValue(MiniYaml::find(node, "params_schema"));

        auto* thenNode = MiniYaml::find(node, "then");
        if (thenNode)
            for (auto& st : thenNode->children)
                s.thenSteps.push_back(parseStep(st));
        auto* elseNode = MiniYaml::find(node, "else");
        if (elseNode)
            for (auto& st : elseNode->children)
                s.elseSteps.push_back(parseStep(st));
        auto* bodyNode = MiniYaml::find(node, "body");
        if (bodyNode)
            for (auto& st : bodyNode->children)
                s.body.push_back(parseStep(st));
        auto* stepsNode = MiniYaml::find(node, "steps");
        if (stepsNode)
            for (auto& st : stepsNode->children)
                s.steps.push_back(parseStep(st));

        // Slice 5: try_catch
        auto* tryNode = MiniYaml::find(node, "try_catch");
        if (tryNode) {
            auto* bodyN = MiniYaml::find(*tryNode, "body");
            if (bodyN) for (auto& st : bodyN->children) s.tryBody.push_back(parseStep(st));
            auto* catchN = MiniYaml::find(*tryNode, "catch");
            if (catchN) for (auto& st : catchN->children) s.catchBody.push_back(parseStep(st));
            auto* finallyN = MiniYaml::find(*tryNode, "finally");
            if (finallyN) for (auto& st : finallyN->children) s.finallyBody.push_back(parseStep(st));
        }

        // Slice 3: switch cases
        auto* casesNode = MiniYaml::find(node, "cases");
        if (casesNode) {
            for (auto& c : casesNode->children) {
                if (c.key.empty()) continue;
                std::vector<WorkflowStep> caseSteps;
                for (auto& st : c.children)
                    caseSteps.push_back(parseStep(st));
                s.switchCases.push_back({c.key, caseSteps});
            }
        }
        auto* defaultNode = MiniYaml::find(node, "default");
        if (defaultNode)
            for (auto& st : defaultNode->children)
                s.switchDefault.push_back(parseStep(st));

        return s;
    }

    // ── Convert a MiniYaml node tree to a Json::Value (slice 1) ──
    // Used for input_schema, output_schema, params_schema, payload, etc.
    // Supports: strings, numbers, booleans, null, lists, maps.
    static Json::Value parseYamlValue(const MiniYaml::Node* node) {
        if (!node) return Json::Value();
        return yamlNodeToJson(*node);
    }

    static Json::Value yamlNodeToJson(const MiniYaml::Node& n) {
        if (n.isList) {
            Json::Value arr(Json::arrayValue);
            for (const auto& c : n.children) arr.append(yamlNodeToJson(c));
            return arr;
        }
        // Scalar: try to parse as bool, number, or fall back to string
        if (n.children.empty()) {
            const std::string& v = n.value;
            if (v == "true") return Json::Value(true);
            if (v == "false") return Json::Value(false);
            if (v == "null" || v == "~") return Json::Value();
            try { size_t pos; long long ll = std::stoll(v, &pos);
                  if (pos == v.size()) return Json::Value(static_cast<Json::Int64>(ll)); } catch (...) {}
            try { size_t pos; double d = std::stod(v, &pos);
                  if (pos == v.size()) return Json::Value(d); } catch (...) {}
            return Json::Value(v);
        }
        // Map
        Json::Value obj(Json::objectValue);
        for (const auto& c : n.children) {
            if (!c.key.empty()) obj[c.key] = yamlNodeToJson(c);
        }
        return obj;
    }

    // ── Cache ──
    std::map<std::string, Workflow> manifestCache_;  // path → Workflow
    std::map<std::string, std::string> nameIndex_;   // workflow name → path
};

}  // namespace cortex::mk3::workflows
