#pragma once
// =============================================================================
// agent-lib-MK3 — Workflow Sovereign Class
// Single-responsibility: a Workflow owns its definition (name, version, steps),
// execution context, and result. The WorkflowEngine orchestrates Workflow
// objects — it doesn't duplicate workflow state.
// =============================================================================

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <chrono>
#include <json/json.h>

namespace cortex::mk3::workflows {

// ── Forward declarations ──
struct WorkflowStep;
struct WorkflowResult;

// ── Workflow step ──
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
    std::vector<WorkflowStep> steps;     // parallel/workflow steps
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

// ── Workflow execution result ──
struct WorkflowResult {
    bool success = false;
    std::string workflowName;
    std::vector<std::string> stepIds;
    std::vector<Json::Value> stepOutputs;
    std::map<std::string, Json::Value> outputs;  // keyed by step id
    std::vector<std::string> diagnostics;
    double elapsedMs = 0.0;
    std::string error;

    Json::Value toJson() const {
        Json::Value j;
        j["success"] = success;
        j["workflow"] = workflowName;
        j["elapsed_ms"] = elapsedMs;
        if (!error.empty()) j["error"] = error;
        Json::Value steps(Json::arrayValue);
        for (const auto& s : stepIds) steps.append(s);
        j["steps"] = steps;
        Json::Value outs(Json::objectValue);
        for (const auto& [id, val] : outputs) outs[id] = val;
        j["outputs"] = outs;
        return j;
    }
};

// ── Workflow runtime callbacks ──
// Wired by the agent at dispatch time — the Workflow doesn't own tools/agents.
struct WorkflowRuntime {
    using ToolFn = std::function<Json::Value(const std::string& name, const Json::Value& params)>;
    using AgentFn = std::function<Json::Value(const std::string& name, const std::string& instruction)>;
    using WorkflowFn = std::function<WorkflowResult(const std::string& name, const Json::Value& params)>;

    ToolFn executeTool;
    AgentFn executeAgent;
    WorkflowFn executeWorkflow;
};

// ═══════════════════════════════════════════════════════════════════════════
// Workflow — sovereign class for one workflow definition
// ═══════════════════════════════════════════════════════════════════════════
class Workflow {
public:
    // ── Constructors ──

    /// Default — invalid workflow
    Workflow() = default;

    /// Construct from a manifest
    explicit Workflow(const WorkflowManifest& manifest)
        : manifest_(manifest) {}

    /// Construct with name only (for dynamic workflows)
    explicit Workflow(const std::string& name)
        : manifest_({name, "1.0", {}, {}, {}, {}, {}, {}}) {}

    /// Move constructor/assignment
    Workflow(Workflow&&) = default;
    Workflow& operator=(Workflow&&) = default;

    // ── No copying ──
    Workflow(const Workflow&) = delete;
    Workflow& operator=(const Workflow&) = delete;

    // ── Accessors ──

    const std::string& name() const noexcept { return manifest_.name; }
    const std::string& version() const noexcept { return manifest_.version; }
    const std::string& summary() const noexcept { return manifest_.summary; }
    const std::string& description() const noexcept { return manifest_.description; }
    const std::vector<WorkflowStep>& steps() const noexcept { return manifest_.steps; }
    const std::vector<std::string>& importTools() const noexcept { return manifest_.importTools; }
    const std::vector<std::string>& importRelics() const noexcept { return manifest_.importRelics; }

    bool isValid() const noexcept { return !manifest_.name.empty(); }

    // ── Mutation (for building workflows programmatically) ──

    void setName(const std::string& n) { manifest_.name = n; }
    void setVersion(const std::string& v) { manifest_.version = v; }
    void setSummary(const std::string& s) { manifest_.summary = s; }
    void setDescription(const std::string& d) { manifest_.description = d; }

    void addStep(const WorkflowStep& step) { manifest_.steps.push_back(step); }
    void setSteps(const std::vector<WorkflowStep>& steps) { manifest_.steps = steps; }
    void addImportTool(const std::string& tool) { manifest_.importTools.push_back(tool); }
    void addImportRelic(const std::string& relic) { manifest_.importRelics.push_back(relic); }

    // ── Manifest access ──

    const WorkflowManifest& manifest() const noexcept { return manifest_; }
    WorkflowManifest& manifest() noexcept { return manifest_; }

    // ── Serialization ──

    /// Convert to XML for prompt injection
    std::string toXml() const {
        std::ostringstream ss;
        ss << "<workflow name=\"" << manifest_.name << "\" version=\""
           << manifest_.version << "\">\n";
        if (!manifest_.summary.empty())
            ss << "  <summary>" << manifest_.summary << "</summary>\n";
        if (!manifest_.description.empty())
            ss << "  <description>" << manifest_.description << "</description>\n";
        if (!manifest_.steps.empty()) {
            ss << "  <steps>\n";
            for (const auto& s : manifest_.steps)
                stepToXml(ss, s, 4);
            ss << "  </steps>\n";
        }
        ss << "</workflow>\n";
        return ss.str();
    }

    /// Metadata as JSON
    Json::Value toJson() const {
        Json::Value j;
        j["name"] = manifest_.name;
        j["version"] = manifest_.version;
        j["summary"] = manifest_.summary;
        j["description"] = manifest_.description;
        j["step_count"] = (int)manifest_.steps.size();
        Json::Value tools(Json::arrayValue);
        for (const auto& t : manifest_.importTools) tools.append(t);
        j["import_tools"] = tools;
        Json::Value relics(Json::arrayValue);
        for (const auto& r : manifest_.importRelics) relics.append(r);
        j["import_relics"] = relics;
        return j;
    }

private:
    WorkflowManifest manifest_;

    // ── XML serialization helper ──
    static void stepToXml(std::ostringstream& ss, const WorkflowStep& s, int indent) {
        std::string pad(indent, ' ');
        ss << pad << "<step id=\"" << s.id << "\" type=\"" << s.type << "\"";
        if (!s.name.empty()) ss << " name=\"" << s.name << "\"";
        if (!s.tool.empty()) ss << " tool=\"" << s.tool << "\"";
        if (!s.agent.empty()) ss << " agent=\"" << s.agent << "\"";
        if (!s.condition.empty()) ss << " condition=\"" << s.condition << "\"";
        if (!s.workflow.empty()) ss << " workflow=\"" << s.workflow << "\"";
        if (s.onError != "abort") ss << " on_error=\"" << s.onError << "\"";
        ss << ">\n";

        if (!s.params.empty()) {
            Json::StreamWriterBuilder w;
            w["indentation"] = std::string(indent + 4, ' ');
            ss << Json::writeString(w, s.params) << "\n";
        }
        if (!s.thenSteps.empty()) {
            ss << pad << "  <then>\n";
            for (const auto& st : s.thenSteps) stepToXml(ss, st, indent + 4);
            ss << pad << "  </then>\n";
        }
        if (!s.elseSteps.empty()) {
            ss << pad << "  <else>\n";
            for (const auto& st : s.elseSteps) stepToXml(ss, st, indent + 4);
            ss << pad << "  </else>\n";
        }
        if (!s.body.empty()) {
            ss << pad << "  <body>\n";
            for (const auto& st : s.body) stepToXml(ss, st, indent + 4);
            ss << pad << "  </body>\n";
        }
        if (!s.steps.empty())
            for (const auto& st : s.steps) stepToXml(ss, st, indent + 2);
        ss << pad << "</step>\n";
    }
};

} // namespace cortex::mk3::workflows
