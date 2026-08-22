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

Json::Value Agent::handleAgentDelegate(AgentContext &ctx,
                                       const protocol::ParsedAction &action,
                                       const std::string &instruction) {
    const std::string &agentName = action.name;
    auto it = subAgents_.find(agentName);
    if (it == subAgents_.end()) {
        // Manifest may have gained the sub-agent since load (operator edited
        // import.agents live). Try reload once before reporting unknown.
        if (!config_.manifestPath.empty()) {
            ManifestLoader::loadSubAgents(config_.manifestPath, *this,
                                          config_.provider);
            it = subAgents_.find(agentName);
        }
    }
    if (it == subAgents_.end()) {
        Json::Value err;
        err["success"] = false;
        err["error"] = "Unknown sub-agent: " + agentName;
        return err;
    }

    std::mutex* childMu = nullptr;
    {
        std::lock_guard<std::mutex> g(subAgentMuMapMu_);
        auto& slot = subAgentRunMus_[agentName];
        if (!slot) slot = std::make_unique<std::mutex>();
        childMu = slot.get();
    }
    std::lock_guard<std::mutex> childRun(*childMu);

    // op: prompt (default) | inspect | context | history
    // XML attrs land in params (parser extra-attr path), e.g.
    //   <action type="agent" name="reader" op="inspect" last_n="10"/>
    //   <action type="agent" name="reader" inspect="true"/>
    std::string op = "prompt";
    if (action.params.isMember("op") &&
        action.params["op"].isString() &&
        !action.params["op"].asString().empty())
        op = action.params["op"].asString();
    else if (action.params.isMember("inspect")) {
        const auto &iv = action.params["inspect"];
        if ((iv.isBool() && iv.asBool()) ||
            (iv.isString() &&
             (iv.asString() == "true" || iv.asString() == "1")) ||
            (iv.isInt() && iv.asInt() != 0))
            op = "inspect";
    }

    if (op == "inspect" || op == "context" || op == "history") {
        int lastN = action.params.get("last_n", 20).asInt();
        if (lastN <= 0)
            lastN = 20;
        Json::Value snap = it->second->inspectContext(lastN);
        snap["op"] = op;
        snap["agent"] = agentName;
        // Compact summary for the RESULT card — last tools/errors, not only a
        // teaser thought. Operators were polling inspect and seeing the same
        // first sentence forever while the child was stuck.
        std::ostringstream sum;
        sum << agentName << " context: history="
            << snap.get("history_total", 0).asInt()
            << " events=" << snap.get("protocol_events", 0).asInt()
            << " raw=" << snap.get("raw_bytes", 0).asUInt64() << "B";
        // Last few protocol events from the child.
        const auto &evs = it->second->protocolEvents();
        int shown = 0;
        for (auto rit = evs.rbegin(); rit != evs.rend() && shown < 6; ++rit) {
            if (rit->kind == ProtocolEventKind::ACTION) {
                sum << "\n  ▸ " << rit->action.type << ":" << rit->action.name
                    << " #" << rit->action.id;
                ++shown;
            } else if (rit->kind == ProtocolEventKind::RESULT) {
                sum << "\n  " << (rit->result.ok ? "✓" : "✗") << " #"
                    << rit->result.id << " " << rit->result.toolName;
                ++shown;
            } else if (rit->kind == ProtocolEventKind::STATUS) {
                std::string t = rit->text;
                if (t.size() > 120) t = t.substr(0, 118) + "…";
                sum << "\n  status: " << t;
                ++shown;
            }
        }
        if (!snap.get("response_output", "").asString().empty()) {
            std::string ro = snap["response_output"].asString();
            if (ro.size() > 240) ro = ro.substr(0, 238) + "…";
            sum << "\nlast_response: " << ro;
        } else if (!evs.empty() && evs.back().kind == ProtocolEventKind::THOUGHT) {
            std::string t = evs.back().text;
            if (t.size() > 160) t = t.substr(0, 158) + "…";
            sum << "\nlast_thought: " << t;
        }
        snap["output"] = sum.str();
        snap["success"] = true;
        return snap;
    }

    bool forceEphemeral = jsonBool(action.params, "ephemeral", false);
    bool dumpContext = jsonBool(action.params, "dump_context", false);
    std::string childSessionId =
        forceEphemeral
            ? ""
            : deriveSubAgentSessionId(ctx, config_, agentName);
    // Ephemeral child calls must not leak prior in-memory history into
    // this mission (non-ephemeral reuses the same Agent object).
    if (forceEphemeral) {
        it->second->clearHistory();
    }
    // Hard seatbelt: parent-delegated children must not inherit 400–1800
    // iteration budgets (live dump: coder burned minutes until cancel).
    // Cap delegated runs; leave standalone launches alone via setMaxIterations
    // only for this call's agent object (sub-agent is reused — clamp stays,
    // which is correct for nested thrash prevention).
    {
        constexpr int kDelegatedChildIterCap = 32;
        int cur = it->second->config().iterationCap;
        if (cur <= 0 || cur > kDelegatedChildIterCap)
            it->second->setIterationCap(kDelegatedChildIterCap);
        // Wall: action timeout attr or parent actionTimeoutSec, floor 60s ceiling 600s.
        int wall = action.timeout > 0 ? action.timeout : config_.actionTimeoutSec;
        if (wall <= 0) wall = 180;
        if (wall < 60) wall = 60;
        if (wall > 600) wall = 600;
        it->second->setActionTimeoutSec(wall);
    }
    // Stream the child's progress to the parent's UI so it stays alive
    // (byte counter + spinner) during the synchronous sub-agent call.
    // Without this, the child runs for seconds with zero UI updates —
    // the 'freeze after the first thought block ends' symptom. The
    // child's generateStream calls ctx.onToken("") as a heartbeat but
    // the actual bytes land in the CHILD's rawLlOutput_, so we forward
    // the child's raw delta through the parent's onToken. The parent's
    // onToken publishes non-empty tokens directly (see runAgentTurn).
    Agent *childPtr = it->second.get();
    std::shared_ptr<size_t> childSeen = std::make_shared<size_t>(0);
    StreamCallback childProgress = [childPtr, childSeen,
                                    &ctx](const std::string &, bool) {
        if (!ctx.onToken)
            return;
        const std::string &r = childPtr->rawLlOutput();
        if (r.size() > *childSeen) {
            ctx.onToken(r.substr(*childSeen), false);
            *childSeen = r.size();
        }
    };
    // Further prompts reuse the child session id → continuous history.
    // Label source as ParentAgent so the child sees who asked.
    std::string result =
        childSessionId.empty()
            ? it->second->prompt(
                  instruction, childProgress, "", forceEphemeral,
                  PromptSource::ParentAgent, config_.name)
            : it->second->prompt(
                  instruction, childProgress, childSessionId, false,
                  PromptSource::ParentAgent, config_.name);
    // Fold-up economy: shrink child history before parent reuses it next time.
    if (config_.compaction.childBeforeReturn)
        it->second->compactHistoryInPlaceIfConfigured();
    std::string trace;
    if (dumpContext) {
        trace = formatDelegatedTrace(agentName, instruction,
                                     it->second->iterationPrompts(),
                                     it->second->iterationOutputs());
        subAgentTraces_.push_back(trace);
    }
    return makeSubAgentResult(result, trace, dumpContext);
}



}  // namespace cortex::mk3
