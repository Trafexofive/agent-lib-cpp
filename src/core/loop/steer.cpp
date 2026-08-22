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

void Agent::steerIncompleteGeneration(AgentContext &ctx,
                                      const std::string &iterationRaw,
                                      int workCap, bool &incompleteNoted,
                                      std::string &lastSalvage) {
    const bool hadActions = iterationRaw.find("<action") != std::string::npos;
    const bool hadNonFinalResp = !trimCopy(responseOutput_).empty();
    const std::string leftover = trimCopy(stripThoughtTags(iterationRaw));
    const std::string lvl = config_.thinkingLevel.empty() ? std::string("medium")
                                                          : config_.thinkingLevel;
    if (hadActions || incompleteNoted)
        return;
    incompleteNoted = true;
    if (hadNonFinalResp) {
        lastSalvage = trimCopy(responseOutput_);
        history_.push_back(
            "System: " +
            buildRuntimeHarness(
                "NONFINAL", ctx.iteration, workCap, lvl,
                "You emitted <response> without final=\"true\" and no "
                "<action>. That is a progress note, not completion.",
                "The <response> body is kept. The turn stays open (" +
                    std::to_string(ctx.iteration) + "/" +
                    std::to_string(workCap) + ").",
                "Next: emit <action> to work, or close with "
                "<response final=\"true\">. If this note is the "
                "answer, re-emit it with final=\"true\".",
                "Do not repeat the same non-final <response>. "
                "Untagged prose is not a final."));
        protocol_.push({ProtocolEventKind::STATUS,
                       "[NONFINAL] response without final — loop continues",
                       {}, {}});
    } else {
        lastSalvage.clear();
        history_.push_back(
            "System: " +
            buildRuntimeHarness(
                "BARE_TEXT", ctx.iteration, workCap, lvl,
                leftover.empty()
                    ? ("This generation produced no <action> and no "
                       "<response>. Provider thinking may have run; "
                       "zero protocol tokens. thinking_level=" +
                       lvl + ".")
                    : ("This generation had no <action> and no "
                       "<response final=\"true\">. Untagged tokens "
                       "are <thought>, not an answer. thinking_level=" +
                       lvl + "."),
                "Thought is kept. No fake <response> was created. "
                "Turn still open (" +
                    std::to_string(ctx.iteration) + "/" +
                    std::to_string(workCap) + ").",
                "Next generation must emit protocol: "
                "<action type=\"tool|agent\" name=\"…\" id=\"…\">"
                "…</action> and/or <response final=\"true\">…"
                "</response>. Put the action in that emit, not "
                "another thinking-only turn.",
                "Do not ping the operator with untagged prose. "
                "Do not duplicate native thinking as extra "
                "<thought> turns. Do not assume the user got a "
                "finished answer."));
        protocol_.push(
            {ProtocolEventKind::STATUS,
             "[BARE_TEXT] untagged kept as thought — loop continues",
             {}, {}});
    }
    if (ctx.onToken) ctx.onToken("", false);
}


}  // namespace cortex::mk3
