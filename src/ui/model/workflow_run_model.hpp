#pragma once
// Workflow run state — hub live rail + worker bridge.
// Thread-safe hub; UI always reads snapshots. No agent coupling.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <json/json.h>

#include "src/workflows/workflow.hpp"

namespace cortex::mk3::ui::model {

enum class StepStatus : uint8_t { Pending = 0, Running, Ok, Fail, Skip };

enum class RunStatus : uint8_t {
    Idle = 0,
    Starting,
    Running,
    Hitl,
    Succeeded,
    Failed,
    Cancelled
};

inline const char* stepStatusGlyph(StepStatus s) {
    switch (s) {
        case StepStatus::Pending:
            return "○";
        case StepStatus::Running:
            return "●";
        case StepStatus::Ok:
            return "✓";
        case StepStatus::Fail:
            return "✗";
        case StepStatus::Skip:
            return "⊘";
    }
    return "?";
}

inline const char* stepStatusLabel(StepStatus s) {
    switch (s) {
        case StepStatus::Pending:
            return "pending";
        case StepStatus::Running:
            return "running";
        case StepStatus::Ok:
            return "ok";
        case StepStatus::Fail:
            return "fail";
        case StepStatus::Skip:
            return "skipped";
    }
    return "?";
}

inline const char* runStatusLabel(RunStatus s) {
    switch (s) {
        case RunStatus::Idle:
            return "idle";
        case RunStatus::Starting:
            return "starting";
        case RunStatus::Running:
            return "running";
        case RunStatus::Hitl:
            return "hitl";
        case RunStatus::Succeeded:
            return "ok";
        case RunStatus::Failed:
            return "fail";
        case RunStatus::Cancelled:
            return "cancelled";
    }
    return "?";
}

inline bool runStatusActive(RunStatus s) {
    return s == RunStatus::Starting || s == RunStatus::Running || s == RunStatus::Hitl;
}

struct WorkflowStepView {
    std::string id;
    std::string name;
    std::string type;
    std::string ref;  // tool/agent/relic/event target
    std::string summary;
    StepStatus status = StepStatus::Pending;
    double ms = 0.0;
    bool human = false;
    bool checkpoint = false;
};

struct WorkflowRunEvent {
    int64_t tMs = 0;
    std::string kind;  // step.enter|step.ok|step.fail|emit|checkpoint|hitl|done|fail|cancel
    std::string text;
};

struct WorkflowTopology {
    int stepCount = 0;
    int branchCount = 0;
    bool hasHitl = false;
    bool hasCheckpoint = false;
};

struct WorkflowRunState {
    std::string path;
    std::string name;
    std::string version;
    std::string summary;
    std::string runId;
    RunStatus status = RunStatus::Idle;
    std::vector<WorkflowStepView> steps;
    int currentIdx = -1;
    int selectedStep = 0;  // inspector focus
    int64_t startedAtMs = 0;
    double elapsedMs = 0.0;
    std::string lastError;
    std::vector<WorkflowRunEvent> events;
    WorkflowTopology topo;
    Json::Value lastOutputs;
    bool live = false;  // true while worker owns the run
};

inline int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline std::string makeRunId(const std::string& name) {
    auto ms = steadyNowMs();
    std::string base = name.empty() ? "wf" : name;
    for (char& c : base) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_'))
            c = '-';
    }
    return base + "-" + std::to_string(ms % 1000000);
}

inline std::string stepRefOf(const workflows::WorkflowStep& s) {
    if (!s.tool.empty()) return s.tool;
    if (!s.agent.empty()) return s.agent;
    if (!s.relic.empty()) return s.relic + (s.action.empty() ? "" : "." + s.action);
    if (!s.feed.empty()) return s.feed;
    if (!s.emitEvent.empty()) return s.emitEvent;
    if (!s.workflow.empty()) return s.workflow;
    if (!s.condition.empty()) return s.condition;
    if (!s.switchOn.empty()) return s.switchOn;
    if (!s.prompt.empty()) return s.prompt;
    return s.type;
}

inline void countTopo(const std::vector<workflows::WorkflowStep>& steps, WorkflowTopology& t) {
    for (const auto& s : steps) {
        ++t.stepCount;
        if (s.type == "human") t.hasHitl = true;
        if (s.type == "checkpoint") t.hasCheckpoint = true;
        if (s.type == "condition" || s.type == "switch" || s.type == "try_catch") ++t.branchCount;
        countTopo(s.thenSteps, t);
        countTopo(s.elseSteps, t);
        countTopo(s.body, t);
        countTopo(s.steps, t);
        countTopo(s.tryBody, t);
        countTopo(s.catchBody, t);
        countTopo(s.finallyBody, t);
        countTopo(s.switchDefault, t);
        for (const auto& kv : s.switchCases) countTopo(kv.second, t);
    }
}

inline std::vector<WorkflowStepView> flattenTopLevelSteps(
    const std::vector<workflows::WorkflowStep>& steps) {
    std::vector<WorkflowStepView> out;
    out.reserve(steps.size());
    for (const auto& s : steps) {
        WorkflowStepView v;
        v.id = s.id.empty() ? s.type : s.id;
        v.name = s.name.empty() ? v.id : s.name;
        v.type = s.type;
        v.ref = stepRefOf(s);
        v.human = (s.type == "human");
        v.checkpoint = (s.type == "checkpoint");
        v.status = StepStatus::Pending;
        out.push_back(std::move(v));
    }
    return out;
}

// Sensible defaults from input_schema so hub Enter can run without a form.
inline Json::Value defaultInputFromSchema(const Json::Value& schema) {
    Json::Value out(Json::objectValue);
    if (!schema.isObject()) return out;
    const Json::Value props = schema.get("properties", Json::Value(Json::objectValue));
    if (!props.isObject()) return out;
    for (const auto& name : props.getMemberNames()) {
        const Json::Value& p = props[name];
        if (p.isMember("default")) {
            out[name] = p["default"];
            continue;
        }
        std::string ty = p.get("type", "string").asString();
        if (ty == "string") {
            if (p.isMember("enum") && p["enum"].isArray() && !p["enum"].empty())
                out[name] = p["enum"][0];
            else if (name == "target" || name == "path" || name == "file")
                out[name] = "hub";
            else if (name == "environment" || name == "env")
                out[name] = "staging";
            else if (name == "topic")
                out[name] = "terminal-native content systems";
            else if (name == "out_dir")
                out[name] = "/tmp/cortex-content";
            else if (name == "audience")
                out[name] = "builders";
            else if (name == "angle")
                out[name] = "first-principles";
            else
                out[name] = name;
        } else if (ty == "integer" || ty == "number") {
            out[name] = 0;
        } else if (ty == "boolean") {
            out[name] = false;
        } else if (ty == "array") {
            out[name] = Json::Value(Json::arrayValue);
        } else if (ty == "object") {
            out[name] = Json::Value(Json::objectValue);
        }
    }
    return out;
}

// Thread-safe controller shared by REPL worker + hub render.
class WorkflowRunHub {
   public:
    WorkflowRunState snapshot() const {
        std::lock_guard<std::mutex> lk(mu_);
        return st_;
    }

    bool isLive() const {
        std::lock_guard<std::mutex> lk(mu_);
        return st_.live;
    }

    bool isActive() const {
        std::lock_guard<std::mutex> lk(mu_);
        return runStatusActive(st_.status);
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        st_ = WorkflowRunState{};
        cancel_.store(false, std::memory_order_release);
    }

    void prepare(const workflows::WorkflowManifest& wf, const std::string& path) {
        std::lock_guard<std::mutex> lk(mu_);
        st_ = WorkflowRunState{};
        st_.path = path;
        st_.name = wf.name;
        st_.version = wf.version;
        st_.summary = wf.summary;
        st_.runId = makeRunId(wf.name);
        st_.status = RunStatus::Starting;
        st_.steps = flattenTopLevelSteps(wf.steps);
        st_.topo = WorkflowTopology{};
        countTopo(wf.steps, st_.topo);
        st_.startedAtMs = steadyNowMs();
        st_.live = true;
        st_.events.push_back({st_.startedAtMs, "start", "run " + st_.runId});
        cancel_.store(false, std::memory_order_release);
    }

    void markRunning() {
        std::lock_guard<std::mutex> lk(mu_);
        if (st_.status == RunStatus::Starting || st_.status == RunStatus::Hitl)
            st_.status = RunStatus::Running;
        touchElapsedUnlocked();
    }

    void markHitl(const std::string& stepId, const std::string& prompt) {
        std::lock_guard<std::mutex> lk(mu_);
        st_.status = RunStatus::Hitl;
        pushEventUnlocked("hitl", stepId + ": " + prompt);
        touchElapsedUnlocked();
    }

    void onProgress(const workflows::StepProgress& p) {
        std::lock_guard<std::mutex> lk(mu_);
        touchElapsedUnlocked();
        int idx = findStepUnlocked(p.id);
        if (idx < 0 && !p.id.empty()) {
            // Nested step — still surface as event; don't invent rail rows.
            std::string kind = "step.";
            switch (p.phase) {
                case workflows::StepProgress::Phase::Enter:
                    kind += "enter";
                    break;
                case workflows::StepProgress::Phase::Ok:
                    kind += "ok";
                    break;
                case workflows::StepProgress::Phase::Fail:
                    kind += "fail";
                    break;
                case workflows::StepProgress::Phase::Skip:
                    kind += "skip";
                    break;
            }
            pushEventUnlocked(kind, p.id + (p.summary.empty() ? "" : " · " + p.summary));
            return;
        }
        if (idx < 0) return;
        auto& step = st_.steps[static_cast<size_t>(idx)];
        switch (p.phase) {
            case workflows::StepProgress::Phase::Enter:
                step.status = StepStatus::Running;
                st_.currentIdx = idx;
                st_.selectedStep = idx;
                if (st_.status != RunStatus::Hitl) st_.status = RunStatus::Running;
                pushEventUnlocked("step.enter", step.id + " · " + step.type);
                break;
            case workflows::StepProgress::Phase::Ok:
                step.status = StepStatus::Ok;
                step.ms = p.elapsedMs;
                if (!p.summary.empty()) step.summary = p.summary;
                pushEventUnlocked("step.ok", step.id + " · " + formatMs(p.elapsedMs));
                break;
            case workflows::StepProgress::Phase::Fail:
                step.status = StepStatus::Fail;
                step.ms = p.elapsedMs;
                if (!p.error.empty()) {
                    step.summary = p.error;
                    st_.lastError = p.error;
                }
                pushEventUnlocked("step.fail", step.id + " · " + p.error);
                break;
            case workflows::StepProgress::Phase::Skip:
                step.status = StepStatus::Skip;
                step.ms = p.elapsedMs;
                pushEventUnlocked("step.skip", step.id);
                break;
        }
    }

    void finish(const workflows::WorkflowResult& result) {
        std::lock_guard<std::mutex> lk(mu_);
        st_.elapsedMs = result.elapsedMs > 0 ? result.elapsedMs : st_.elapsedMs;
        st_.lastOutputs = Json::Value(Json::objectValue);
        for (const auto& kv : result.outputs) st_.lastOutputs[kv.first] = kv.second;
        // Apply metrics to top-level steps when ids match
        for (const auto& m : result.stepMetrics) {
            int idx = findStepUnlocked(m.id);
            if (idx < 0) continue;
            auto& step = st_.steps[static_cast<size_t>(idx)];
            step.ms = m.elapsedMs;
            if (step.status == StepStatus::Pending || step.status == StepStatus::Running)
                step.status = m.success ? StepStatus::Ok : StepStatus::Fail;
        }
        if (cancel_.load(std::memory_order_acquire) && !result.success) {
            st_.status = RunStatus::Cancelled;
            st_.lastError = result.error.empty() ? "cancelled" : result.error;
            pushEventUnlocked("cancel", st_.lastError);
        } else if (result.success) {
            st_.status = RunStatus::Succeeded;
            // Pending top-level steps that never entered stay pending (branches)
            pushEventUnlocked("done", "ok · " + formatMs(st_.elapsedMs));
        } else {
            st_.status = RunStatus::Failed;
            st_.lastError = result.error;
            pushEventUnlocked("fail", result.error.empty() ? "failed" : result.error);
        }
        st_.live = false;
        st_.currentIdx = -1;
    }

    void fail(const std::string& err) {
        std::lock_guard<std::mutex> lk(mu_);
        st_.status = RunStatus::Failed;
        st_.lastError = err;
        st_.live = false;
        pushEventUnlocked("fail", err);
        touchElapsedUnlocked();
    }

    void requestCancel() { cancel_.store(true, std::memory_order_release); }

    bool cancelRequested() const { return cancel_.load(std::memory_order_acquire); }

    void note(const std::string& kind, const std::string& text) {
        std::lock_guard<std::mutex> lk(mu_);
        pushEventUnlocked(kind, text);
        touchElapsedUnlocked();
    }

    void selectStep(int delta) {
        std::lock_guard<std::mutex> lk(mu_);
        if (st_.steps.empty()) return;
        int n = static_cast<int>(st_.steps.size());
        st_.selectedStep = (st_.selectedStep + delta % n + n) % n;
    }

    void setSelectedStep(int idx) {
        std::lock_guard<std::mutex> lk(mu_);
        if (st_.steps.empty()) return;
        if (idx < 0) idx = 0;
        if (idx >= static_cast<int>(st_.steps.size()))
            idx = static_cast<int>(st_.steps.size()) - 1;
        st_.selectedStep = idx;
    }

    // Preview-only (no live run): fill steps from manifest for hub detail.
    static WorkflowRunState previewFromManifest(const workflows::WorkflowManifest& wf,
                                                const std::string& path) {
        WorkflowRunState st;
        st.path = path;
        st.name = wf.name;
        st.version = wf.version;
        st.summary = wf.summary;
        st.status = RunStatus::Idle;
        st.steps = flattenTopLevelSteps(wf.steps);
        countTopo(wf.steps, st.topo);
        return st;
    }

   private:
    int findStepUnlocked(const std::string& id) const {
        for (int i = 0; i < static_cast<int>(st_.steps.size()); ++i)
            if (st_.steps[static_cast<size_t>(i)].id == id) return i;
        return -1;
    }

    void pushEventUnlocked(const std::string& kind, const std::string& text) {
        WorkflowRunEvent ev;
        ev.tMs = steadyNowMs();
        ev.kind = kind;
        ev.text = text;
        st_.events.push_back(std::move(ev));
        constexpr size_t kMax = 48;
        if (st_.events.size() > kMax)
            st_.events.erase(st_.events.begin(),
                             st_.events.begin() + static_cast<long>(st_.events.size() - kMax));
    }

    void touchElapsedUnlocked() {
        if (st_.startedAtMs > 0)
            st_.elapsedMs = static_cast<double>(steadyNowMs() - st_.startedAtMs);
    }

    static std::string formatMs(double ms) {
        if (ms < 1000.0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.0fms", ms);
            return buf;
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1fs", ms / 1000.0);
        return buf;
    }

    mutable std::mutex mu_;
    WorkflowRunState st_;
    std::atomic<bool> cancel_{false};
};

}  // namespace cortex::mk3::ui::model
