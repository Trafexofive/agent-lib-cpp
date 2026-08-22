#include "src/core/agent.hpp"
#include "src/core/agent_xml.hpp"
#include "src/core/agent_run_helpers.hpp"
#include "src/core/agent_harness.hpp"
#include "src/core/turn_emitter.hpp"
#include "src/core/manifest_loader.hpp"
#include "src/core/dispatch.hpp"

#include "../feeds/feed_engine.hpp"
#include "../protocol/noise.hpp"
#include "../tools/dispatch.hpp"
#include "../utils/ansi.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace cortex::mk3 {

workflows::WorkflowResult Agent::handleWorkflowDelegate(
    AgentContext &ctx, const std::string &workflowName, const Json::Value &params) {
    auto wf =
        workflows::WorkflowEngine::instance().getCached(workflowName);
    workflows::WorkflowRuntime rt;

    // Tool callback: dispatch a tool by name with params
    rt.executeTool = [this](const std::string &name,
                            const Json::Value &p) -> Json::Value {
        protocol::ParsedAction a;
        a.name = name;
        a.type = protocol::ActionType::TOOL;
        a.params = p;
        return dispatchTool(a);
    };

    // Agent callback: delegate to a sub-agent. Honors the
    // ephemeral / dump_context modifiers from the workflow step so
    // workflow agent actions match the behavior of direct
    // <action type="agent" ...> actions.
    rt.executeAgent =
        [this, &ctx](const workflows::WorkflowAgentInvocation &inv)
        -> Json::Value {
        auto it = subAgents_.find(inv.name);
        if (it == subAgents_.end() && !config_.manifestPath.empty()) {
            ManifestLoader::loadSubAgents(config_.manifestPath, *this,
                                          config_.provider);
            it = subAgents_.find(inv.name);
        }
        if (it == subAgents_.end()) {
            Json::Value err;
            err["success"] = false;
            err["error"] = "Unknown sub-agent: " + inv.name;
            return err;
        }
        std::string childSessionId =
            inv.ephemeral
                ? std::string()
                : deriveSubAgentSessionId(ctx, config_, inv.name);
        std::string result =
            childSessionId.empty()
                ? it->second->prompt(inv.instruction, "", inv.ephemeral)
                : it->second->prompt(inv.instruction, childSessionId,
                                     false);
        if (inv.dumpContext) {
            std::string trace =
                formatDelegatedTrace(inv.name, inv.instruction,
                                     it->second->iterationPrompts(),
                                     it->second->iterationOutputs());
            subAgentTraces_.push_back(trace);
            Json::Value r;
            r["success"] = true;
            r["output"] = result;
            r["trace"] = trace;
            return r;
        }
        Json::Value r;
        r["success"] = true;
        r["output"] = result;
        return r;
    };

    // Slice 2: human callback — defaults to the step's default value.
    rt.executeHuman = [](const std::string &id,
                         const Json::Value &prompt) -> Json::Value {
        (void)id;
        return prompt.isMember("default")
                   ? Json::Value(prompt["default"])
                   : Json::Value("");
    };

    // Slice 2: relic callback — uses Reliquary singleton.
    rt.executeRelic = [](const std::string &name,
                         const std::string &action,
                         const Json::Value &params) -> Json::Value {
        auto &rel = relics::Reliquary::instance();
        if (!rel.has(name)) {
            Json::Value err;
            err["success"] = false;
            err["error"] = "unknown relic: " + name;
            return err;
        }
        auto result = rel.dispatch(name, action, params);
        Json::Value out;
        out["success"] = result.success;
        if (!result.error.empty())
            out["error"] = result.error;
        if (!result.data.isNull())
            out["data"] = result.data;
        return out;
    };

    // Slice 2: feed callback — uses FeedEngine singleton.
    // If step.action is set, calls it as a feed tool.
    // Otherwise refreshes the feed and returns the latest value.
    rt.executeFeed = [](const std::string &name,
                        const Json::Value &query) -> Json::Value {
        auto &eng = feeds::FeedEngine::instance();
        std::string action =
            query.isMember("action") ? query["action"].asString() : "";
        if (!action.empty())
            return eng.callFeedTool(name, action, query);
        auto *feed = const_cast<feeds::Feed *>(eng.getFeed(name));
        if (!feed) {
            Json::Value err;
            err["success"] = false;
            err["error"] = "unknown feed: " + name;
            return err;
        }
        auto result = feed->refresh();
        Json::Value out;
        out["success"] = result.ok;
        out["name"] = result.name;
        out["summary"] = result.summary;
        out["json"] = result.json;
        return out;
    };

    // Slice 2: emit — no-op by default
    rt.executeEmit = [](const std::string &event,
                        const Json::Value &payload) {
        (void)event;
        (void)payload;
    };

    // Slice 4: checkpoint — uses the agent's CheckpointHandler if set.
    rt.executeCheckpoint = [this](const std::string &id,
                                  const Json::Value &state) {
        if (checkpointHandler_)
            checkpointHandler_(id, state);
    };

    // Slice 6: parallel_race — fire all in parallel, first to succeed
    // wins. Implementation: launch all steps on background threads,
    // wait for the first success, then return that result. Losers
    // continue running in the background but their results are
    // discarded.
    rt.executeParallelRace =
        [this, &rt](const std::vector<workflows::WorkflowStep> &steps,
                    const std::map<std::string, Json::Value> & /*symbols*/)
        -> Json::Value {
        std::mutex mtx;
        std::condition_variable cv;
        Json::Value winner(Json::objectValue);
        std::atomic<bool> hasWinner{false};
        std::vector<std::thread> workers;
        workers.reserve(steps.size());
        for (const auto &s : steps) {
            workers.emplace_back([&, s]() {
                Json::Value p;
                for (const auto &k : s.params.getMemberNames())
                    p[k] = s.params[k];
                Json::Value r;
                if (s.type == "tool" && rt.executeTool)
                    r = rt.executeTool(s.tool, p);
                else if (s.type == "agent" && rt.executeAgent) {
                    workflows::WorkflowAgentInvocation inv;
                    inv.name = s.agent;
                    inv.instruction = p.toStyledString();
                    r = rt.executeAgent(inv);
                } else {
                    r["success"] = true;
                    r["note"] = "no runtime for type=" + s.type;
                }
                // Was the winner taken while we were running?
                if (!hasWinner.load() && r.isObject() &&
                    r.get("success", false).asBool()) {
                    std::lock_guard<std::mutex> lock(mtx);
                    if (!hasWinner.load()) {
                        winner = r;
                        winner["_winner_id"] = s.id;
                        hasWinner.store(true);
                        cv.notify_all();
                    }
                }
            });
        }
        // Wait briefly for the first success (1s timeout for now).
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait_for(lock, std::chrono::milliseconds(1000),
                        [&] { return hasWinner.load(); });
        }
        Json::Value out(Json::objectValue);
        if (hasWinner.load()) {
            out["winner"] = winner;
            out["race_strategy"] = "first-success";
        } else {
            out["winner"] = Json::Value(Json::objectValue);
            out["race_strategy"] = "timeout-no-winner";
        }
        // Detach workers — they finish in the background; results
        // discarded
        for (auto &w : workers)
            w.detach();
        return out;
    };

    // Recursive workflow call — builds its own runtime, not a copy of
    // rt
    rt.executeWorkflow =
        [this,
         &ctx](const std::string &name,
               const Json::Value &p) -> workflows::WorkflowResult {
        auto subWf =
            workflows::WorkflowEngine::instance().getCached(name);
        workflows::WorkflowRuntime subRt;
        subRt.executeTool =
            [this](const std::string &tn,
                   const Json::Value &tp) -> Json::Value {
            protocol::ParsedAction a;
            a.name = tn;
            a.type = protocol::ActionType::TOOL;
            a.params = tp;
            return dispatchTool(a);
        };
        subRt.executeAgent =
            [this, &ctx](const workflows::WorkflowAgentInvocation &inv)
            -> Json::Value {
            auto it = subAgents_.find(inv.name);
            if (it == subAgents_.end() && !config_.manifestPath.empty()) {
                ManifestLoader::loadSubAgents(config_.manifestPath, *this,
                                              config_.provider);
                it = subAgents_.find(inv.name);
            }
            if (it == subAgents_.end()) {
                Json::Value err;
                err["success"] = false;
                err["error"] = "Unknown sub-agent: " + inv.name;
                return err;
            }
            std::string childSessionId =
                inv.ephemeral
                    ? std::string()
                    : deriveSubAgentSessionId(ctx, config_, inv.name);
            std::string result =
                childSessionId.empty()
                    ? it->second->prompt(inv.instruction, "",
                                         inv.ephemeral)
                    : it->second->prompt(inv.instruction,
                                         childSessionId, false);
            if (inv.dumpContext) {
                std::string trace = formatDelegatedTrace(
                    inv.name, inv.instruction,
                    it->second->iterationPrompts(),
                    it->second->iterationOutputs());
                subAgentTraces_.push_back(trace);
                Json::Value r;
                r["success"] = true;
                r["output"] = result;
                r["trace"] = trace;
                return r;
            }
            Json::Value r;
            r["success"] = true;
            r["output"] = result;
            return r;
        };
        return workflows::WorkflowEngine::instance().execute(subWf,
                                                             subRt, p);
    };

    return workflows::WorkflowEngine::instance().execute(wf, rt,
                                                         params);
}

}  // namespace cortex::mk3
