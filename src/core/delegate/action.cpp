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

Json::Value Agent::handleActionExecute(
    AgentContext &ctx, dispatch::ActionDispatcher &d,
    std::string &iterationRuntimeOutput, bool finalizationTurn,
    const protocol::ParsedAction &action) {
    // Finalization turn: refuse all side-effecting actions so the
    // model must close with <response final="true">.
    if (finalizationTurn) {
        Json::Value denied;
        denied["success"] = false;
        denied["error"] =
            "finalization turn: actions disabled — emit "
            "<response final=\"true\"> only"; // OR whatever we will treat it as such regardless
        denied["output"] = denied["error"];
        return denied;
    }

    protocol::ParsedAction expandedAction = action;
    expandedAction.params =
        expandValueRefs(action.params, actionResults_);
    if (!action.content.empty()) {
        Json::Value contentVal(action.content);
        Json::Value expandedContent =
            expandValueRefs(contentVal, actionResults_);
        if (expandedContent.isString())
            expandedAction.content = expandedContent.asString();
    }

    // Dedup by (name + resolved params + resolved content).
    // Mutating actions must not use or preserve cache: a successful
    // write invalidates prior reads/tests.
    bool mutatesState =
        (expandedAction.type == protocol::ActionType::TOOL &&
         expandedAction.name == "fs_write");
    std::string key = dispatch::dedupKey(expandedAction);
    if (!mutatesState) {
        auto it = executedActions_.find(key);
        if (it != executedActions_.end()) {
            Json::Value cached;
            Json::CharReaderBuilder r;
            std::string errs;
            std::istringstream ss(it->second);
            if (Json::parseFromStream(r, ss, &cached, &errs)) {
                actionResults_[expandedAction.id] =
                    expansionResultView(cached);
                return cached;
            }
        }
    }

    Json::Value result;
    auto t0 = std::chrono::steady_clock::now();
    // Route TOOL actions through agent (supports script tools +
    // sandbox) Other action types (agent/relic/feed) go through the
    // dispatcher
    if (expandedAction.type == protocol::ActionType::TOOL) {
        result = this->dispatchTool(expandedAction);
    } else {
        result = d.dispatch(expandedAction);
    }
    auto t1 = std::chrono::steady_clock::now();
    double elapsedMs =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 -
                                                              t0)
            .count() /
        1000.0;
    result["_elapsed_ms"] = elapsedMs; // metadata for renderer
    actionResults_[expandedAction.id] = expansionResultView(result);
    if (mutatesState && result.get("success", false).asBool()) {
        executedActions_.clear();
    } else if (!mutatesState) {
        executedActions_[key] =
            Json::writeString(Json::StreamWriterBuilder(), result);
    }

    // Inject runtime result into cumulative trace and this
    // iteration's trace.
    {
        std::string resultTag = buildResultTag(action.id, result);
        rawLlOutput_ += "\n" + resultTag + "\n";
        iterationRuntimeOutput += resultTag + "\n";
    }

    // Store protocol result for TUI timeline. Debug mode must still
    // show action/result cards; only raw mode suppresses structured
    // UI.
    if (!ctx.raw) {
        bool ok = result.get("success", false).asBool();
        std::string summary;
        if (ok) {
            std::string out = result.get("stdout", "").asString();
            if (!out.empty())
                summary = out;
            // Check multiple common result field names
            else if (result.isMember("result") &&
                     result["result"].isString())
                summary = result["result"].asString();
            else if (result.isMember("results") &&
                     result["results"].isString())
                summary = result["results"].asString();
            else if (result.isMember("output") &&
                     result["output"].isString())
                summary = result["output"].asString();
            else if (result.isMember("content") &&
                     result["content"].isString())
                summary = result["content"].asString();
            else if (result.isMember("data") &&
                     result["data"].isString())
                summary = result["data"].asString();
            // tree tool (manifests/tools/tree) puts the ascii map in
            // "tree", not "output". Without this, UI shows "tree" 4B.
            else if (result.isMember("tree") &&
                     result["tree"].isString())
                summary = result["tree"].asString();
            else if (result.isMember("tree") &&
                     result["tree"].isObject()) {
                Json::StreamWriterBuilder wb;
                wb["indentation"] = "";
                summary = Json::writeString(wb, result["tree"]);
            }
            // context_peek / pin / unpin return structured JSON
            // without an "output" string — synthesize a scannable
            // summary so RESULT cards are not empty tool-name
            // stubs.
            if (summary.empty() &&
                (expandedAction.name == "context_peek" ||
                 expandedAction.name == "context_pin" ||
                 expandedAction.name == "context_unpin" ||
                 expandedAction.name == "context_manage")) {
                std::ostringstream ss;
                if (result.isMember("path"))
                    ss << result["path"].asString();
                if (result.isMember("mode"))
                    ss << (ss.str().empty() ? "" : " · ")
                       << result["mode"].asString();
                if (result.isMember("bytes"))
                    ss << (ss.str().empty() ? "" : " · ")
                       << result["bytes"].asUInt64() << "B";
                if (result.isMember("cycles_remaining"))
                    ss << (ss.str().empty() ? "" : " · ")
                       << "cycles="
                       << result["cycles_remaining"].asInt();
                if (result.isMember("note") &&
                    result["note"].isString())
                    ss << (ss.str().empty() ? "" : "\n")
                       << result["note"].asString();
                if (result.isMember("error") &&
                    result["error"].isString())
                    ss << (ss.str().empty() ? "" : "\n")
                       << "error: " << result["error"].asString();
                summary = ss.str();
            }
            if (summary.empty())
                summary = action.name;
        } else {
            summary = action.name + " — " +
                      result.get("error", "?").asString();
        }
        ProtocolResult protocolResult{
            action.id,
            ok,
            summary,
            action.name,
            result.get("exit_code", 0).asInt(),
            result.get("_elapsed_ms", 0.0).asDouble(),
            (size_t)summary.size()};
        protocolResults_.push_back(protocolResult);
        protocol_.push(
            {ProtocolEventKind::RESULT, "", {}, protocolResult});
        // Notify callback so TUI can stream tool results
        // immediately
        if (ctx.onToken && ctx.streaming)
            ctx.onToken("", false);
    }

    return result;
}


}  // namespace cortex::mk3
