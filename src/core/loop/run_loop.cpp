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

std::string Agent::runLoop(AgentContext &ctx) {
    // TlsRunGuard in prompt() already armed this agent's RunControl.
    // g_hardKill (SIGINT/SIGTERM) is left alone.
    std::string fullResponse;
    std::string rawOutput;
    // Continuation = this Agent already has transcript from a prior prompt().
    // Sub-agents are reused in-process; wiping protocolEvents_ here made every
    // drilldown show only the latest call, and broke the illusion (and the
    // contract) of a continuous non-ephemeral child conversation.
    const bool continuation = !history_.empty();
    rawLlOutput_.clear();
    responseOutput_.clear();
    thoughtOutput_.clear();
    iterationPrompts_.clear();
    iterationOutputs_.clear();
    subAgentTraces_.clear();
    // Vet-fix: bound the "tap back if same kind" merge window. Previously
    // on a continuation, protocolEvents_ was preserved across turns and
    // the next prompt's stream-merge logic did
    // `protocolEvents_.back().text += ev.content` against an event from
    // the *previous* turn — making thoughts appear to "stream into" an
    // already-finalized THOUGHT block. Track an epoch so onEvent only
    // merges with events from THIS runLoop.
    size_t runEpochStart = continuation ? protocol_.size() : size_t(0);
    if (!continuation) {
        protocolActions_.clear();
        protocolResults_.clear();
        protocol_.clear();
        runEpochStart = 0; // re-derive after clear
    }

    // Push initiator input once at start (NOT per-iteration). Parent-agent
    // delegates are labeled so the child can distinguish them from the human.
    // Vet-fix: skip when the submitter pre-seeded history_ with the same
    // text — submitComposer can write a saveSession immediately on first
    // non-empty turn (so a TUI that crashes before prompt() runs still has
    // something on disk), but that gives the prompt path a duplicate User:
    // entry. Detect the trailing-equal-or-prefix and skip the second push.
    if (ctx.source == PromptSource::ParentAgent) {
        std::string from = ctx.sourceName.empty() ? "parent" : ctx.sourceName;
        history_.push_back("Parent(" + from + "): " + ctx.userInput);
    } else if (ctx.source == PromptSource::Internal) {
        history_.push_back("System: " + ctx.userInput);
    } else if (ctx.userInput.empty() && continuation) {
        // Silent /continue — resume the loop from existing history without
        // injecting a User: line (no "continue", no ".").
    } else {
        // Vet-fix: submitComposer pre-seeds history_ with the same user
        // text so a TUI exit before this method runs still lands the
        // typed prompt on disk. Skip a trailing-equal User: line to
        // avoid duplicating records on the next prompt.
        const std::string needle = "User: " + ctx.userInput;
        bool alreadyLast = !history_.empty() && history_.back() == needle;
        if (!alreadyLast) {
            history_.push_back(needle);
        }
    }

    // Parser lives across iterations — usedActionIds_ and finalResponseSeen_
    // persist so duplicate-ID and post-final enforcement works cross-turn.
    protocol::Parser parser;

    // Completion policy: how bare / non-final model output is handled.
    // Derived from runtime.mode + optional runtime.completion_policy.
    enum class CompPolicy { Recover, Promote, Strict };
    auto resolveCompPolicy = [&]() -> CompPolicy {
        if (config_.completionPolicy == "strict")
            return CompPolicy::Strict;
        if (config_.completionPolicy == "promote")
            return CompPolicy::Promote;
        if (config_.completionPolicy == "recover")
            return CompPolicy::Recover;
        if (config_.runtimeMode == "autonomous")
            return CompPolicy::Promote;
        return CompPolicy::Recover; // normal default
    };
    const CompPolicy compPolicy = resolveCompPolicy();
    std::string lastSalvage; // non-final <response> body this turn (cap salvage)
    std::string lastThoughtContent;
    bool incompleteNoted = false;  // at most one BARE_TEXT/NONFINAL per prompt()


    const int workCap = std::max(1, config_.iterationCap);
    bool finalizationTurn = false;
    bool finalizationDone = false;
    std::string limitReason; // set when we enter finalization due to a cap

    TurnEmitter emit{history_, protocol_, ctx.onToken, ctx.iteration,
                     workCap, config_.thinkingLevel, runEpochStart};

    // Work turns 1..workCap, then at most one FINALIZATION turn (tools
    // disabled) so the model always gets an honest last chance to emit
    // final=true.
    for (ctx.iteration = 1;; ctx.iteration++) {
        liveIteration_.store(ctx.iteration, std::memory_order_relaxed);
        lastLiveIteration_.store(ctx.iteration, std::memory_order_relaxed);
        if (!g_running) {
            const auto sk = currentRunStopKind();
            if (sk == RunStopKind::ExternalSignal) {
                fullResponse = "[timed out]";
                emit.harness("TIMEOUT",
                            "External signal (SIGTERM / wall timeout / kill) stopped "
                            "the process. This is NOT an operator Ctrl-C cancel. "
                            "Pending tools/children were interrupted.",
                            "limit");
                emit.finish("timeout", "[timed out · external signal]");
            } else {
                fullResponse = "[cancelled]";
                emit.harness("CANCEL",
                            "Operator stopped the turn (Ctrl-C/X). Pending "
                            "tools/children should halt; do not continue the "
                            "cancelled plan.",
                            "limit");
                emit.finish("cancel", "[cancelled by operator]");
            }
            break;
        }

        if (!finalizationTurn && ctx.iteration > workCap) {
            // Exhausted work budget without a final response → dedicated
            // finalization turn (does not consume another "work" slot).
            finalizationTurn = true;
            ctx.iteration = workCap + 1;
            limitReason = "max_iterations=" + std::to_string(workCap);
            emit.harness(limitReason,
                          "iteration budget exhausted without "
                          "<response final=\"true\">. Entering FINALIZATION "
                          "turn — tools disabled; emit the best honest final "
                          "answer now.",
                          "limit");
        }

        if (finalizationTurn && finalizationDone)
            break;

        ChatMessages msgs = buildChatPrompt(ctx);
        // Save full prompt for /prompts toggle
        lastPrompt_ = msgs.size() > 0 ? msgs[0].content : "";
        {
            std::ostringstream pd;
            for (size_t i = 0; i < msgs.size(); i++) {
                const char *role = ChatMessage::roleName(msgs[i].role);
                if (msgs[i].role == ChatRole::SYSTEM) {
                    if (i == 0) {
                        pd << msgs[i].content;
                        if (!msgs[i].content.empty() &&
                            msgs[i].content.back() != '\n')
                            pd << '\n';
                    } else {
                        pd << "<dynamic_context role=\"system\">\n"
                           << msgs[i].content;
                        if (!msgs[i].content.empty() &&
                            msgs[i].content.back() != '\n')
                            pd << '\n';
                        pd << "</dynamic_context>\n";
                    }
                } else if (msgs[i].role == ChatRole::USER) {
                    pd << "<user current=\"true\" iteration=\"" << ctx.iteration
                       << "\"";
                    if (!ctx.sessionId.empty())
                        pd << " session=\"" << xmlAttr(ctx.sessionId) << "\"";
                    pd << ">\n" << msgs[i].content;
                    if (!msgs[i].content.empty() &&
                        msgs[i].content.back() != '\n')
                        pd << '\n';
                    pd << "</user>\n";
                } else {
                    pd << "<message role=\"" << role << "\">\n"
                       << msgs[i].content;
                    if (!msgs[i].content.empty() &&
                        msgs[i].content.back() != '\n')
                        pd << '\n';
                    pd << "</message>\n";
                }
            }
            iterationPrompts_.push_back(pd.str());
        }

        // Soft warning on last WORK turn (tools still allowed).
        if (!finalizationTurn && ctx.iteration == workCap) {
            msgs.push_back(ChatMessage::user(
                "<harness note=\"limit_warning\" limit=\"max_iterations=" +
                std::to_string(workCap) + "\">\n"
                "This is work iteration " + std::to_string(workCap) + "/" +
                std::to_string(workCap) +
                ". After this turn the runtime will force a FINALIZATION "
                "turn with tools disabled. Prefer <response final=\"true\"> "
                "now if you have enough evidence; otherwise finish critical "
                "tools quickly.\n</harness>"));
        }
        // Hard finalization prompt — no tools, must close.
        if (finalizationTurn) {
            // Injected as a runtime <harness> tag so the model sees the limit
            // as harness-side state (harness.md), not a generic user message.
            std::ostringstream fin;
            fin << "<harness limit=\"" << limitReason
                << "\" status=\"finalization\">\n"
                << limitReason
                << " exhausted. Tools are DISABLED this turn.\n"
                << "Emit exactly:\n"
                << "<response final=\"true\">\n"
                << "…concise answer from evidence already gathered…\n"
                << "</response>\n";
            if (!lastSalvage.empty()) {
                fin << "\nIf useful, you may reuse this salvaged draft:\n"
                    << "----- BEGIN SALVAGE -----\n";
                const size_t kMax = 8000;
                if (lastSalvage.size() > kMax)
                    fin << lastSalvage.substr(0, kMax) << "\n…[truncated]";
                else
                    fin << lastSalvage;
                fin << "\n----- END SALVAGE -----\n";
            }
            fin << "\n</harness>";
            msgs.push_back(ChatMessage::user(fin.str()));
        }

        // Terminal logs only when operator asked (-V) AND TUI has not silenced us.
        // Full prompt always lands in .cortex/dev/*/iterations.md under dev_mode.
        if (!silenceTerminal_ && (ctx.debug || ctx.verbose)) {
            std::cerr << "[MK3:DEBUG] iter " << ctx.iteration << " — "
                      << msgs.size() << " msgs";
        }

        if (!silenceTerminal_ && ctx.verbose) {
            std::cerr << "\n─── PROMPT iter " << ctx.iteration << " ───\n";
            for (size_t i = 0; i < msgs.size(); i++) {
                const char *role = ChatMessage::roleName(msgs[i].role);
                std::string content = msgs[i].content;
                std::cerr << "[" << role << "] " << content << "\n";
            }
            std::cerr << "─── END PROMPT ───\n";
        }

        dispatch::ActionDispatcher d;
        // Wire agent delegation to sub-agent prompt
        d.agentDelegate =
            [this, &ctx](const protocol::ParsedAction &action,
                         const std::string &instruction) -> Json::Value {
                return handleAgentDelegate(ctx, action, instruction);
            };


        // Wire workflow execution — creates a WorkflowRuntime with tool + agent
        // callbacks
        d.workflowDelegate =
            [this, &ctx](const std::string &workflowName,
                         const Json::Value &params) -> workflows::WorkflowResult {
                return handleWorkflowDelegate(ctx, workflowName, params);
            };


        std::string iterationRawOutput;
        std::string iterationRuntimeOutput;

        parser.setExecutor(
            [this, &d, &ctx, &iterationRuntimeOutput, finalizationTurn](
                const protocol::ParsedAction &action) -> Json::Value {
                return handleActionExecute(ctx, d, iterationRuntimeOutput,
                                           finalizationTurn, action);
            });


        // Tracking state for this iteration's protocol stream.
        // Reset per-iteration response/thought accumulators so a stale
        // non-final <response> from an earlier iteration can never win the
        // final answer over newer content (or a thought-only final). Only the
        // current iteration's response output is considered for the result.
        responseOutput_.clear();
        thoughtOutput_.clear();
        ProtocolStreamState st;
        st.runEpochStart = runEpochStart;

        parser.onEvent([this, &ctx, &st](const protocol::TokenEvent &ev) {
            handleProtocolEvent(ctx, st, ev);
        });


        // Call LLM with exponential-backoff retry on transient empty/filtered
        // responses. Network exceptions are surfaced immediately (existing
        // behavior); only successful-but-empty streams are retried.
        //
        // RETRY ISOLATION (vet-fix): every retry rebuilds the visible stream
        // from scratch. Two independent observers must agree the iteration is
        // fresh: the protocol/parser state on this side AND the UI's previous
        // event baseline on the other. Without an out-of-band signal, the UI
        // would replay the failed attempt's partial protocol rows on top of
        // the retry attempt's real ones, causing visual flicker and stale
        // "running" badges. We emit a ProtocolEventKind::RETRY marker so the
        // bridge can reset its previousEvent baseline coherently.
        StreamAttempt sa;
        sa.st = &st;
        sa.parser = &parser;
        sa.msgs = &msgs;
        sa.emit = &emit;
        sa.iterationRawOutput = &iterationRawOutput;
        sa.iterationRuntimeOutput = &iterationRuntimeOutput;
        sa.rawOutput = &rawOutput;
        sa.fullResponse = &fullResponse;
        sa.runEpochStart = runEpochStart;
        streamUntilSettled(ctx, sa);
        ILlmProvider::StreamStats streamStats = sa.stats;
        // Cancelled mid-stream / provider hard-fail: leave the outer iteration
        // loop cleanly. Without this we fall into empty-response / salvage
        // recovery and paint a fake "empty response" error after Ctrl-X.
        if (!g_running || fullResponse == "[cancelled]" ||
            fullResponse == "[timed out]" || fullResponse == "[aborted]" ||
            fullResponse.rfind("[timed out", 0) == 0 ||
            fullResponse.rfind("[cancelled", 0) == 0 ||
            fullResponse.rfind("[aborted", 0) == 0) {
            if (fullResponse.empty() || fullResponse == "[cancelled]") {
                if (currentRunStopKind() == RunStopKind::ExternalSignal)
                    fullResponse = "[timed out]";
                else if (fullResponse.empty())
                    fullResponse = "[cancelled]";
            }
            break;
        }
        if (fullResponse.rfind("Error: ", 0) == 0) {
            // finishTurn already ran in the catch path.
            break;
        }

        // Join async tool futures. Floor 120s — 30s was killing parent turns
        // while a child/agent or slow tool still ran (operator saw TIMEOUT +
        // child still burning). Ctrl-C still aborts via g_running slices.
        {
            int joinSec = config_.actionTimeoutSec;
            if (joinSec > 0 && joinSec < 120) joinSec = 120;
            if (joinSec <= 0) joinSec = 600;
            if (!parser.waitForActions(std::chrono::seconds(joinSec))) {
                const std::string detail =
                    "Actions did not complete within " +
                    std::to_string(joinSec) +
                    "s. Pending async work was abandoned for this turn. "
                    "Prefer mode=sync agents (joined) or tool wait{ids}. "
                    "Do not sleep-poll children.";
                emit.harness("TIMEOUT", detail, "limit");
                const std::string to = "[TIMEOUT] " + detail;
                emit.finish("action_timeout", to);
                fullResponse = to;
                break;
            }
        }
        parser.flush();

        // Collect context feeds
        auto feeds = parser.contextFeeds();
        for (auto &feed : feeds)
            contextFeeds_.push_back(feed);

        // Determine completion
        auto results = parser.allResults();

        if (!silenceTerminal_ && (ctx.debug || ctx.verbose)) {
            std::cerr << " | actions=" << results.size()
                      << " complete=" << st.taskComplete
                      << " resp=" << responseOutput_.size() << "b"
                      << " text=" << st.llmOutput.size() << "b";
            if (ctx.verbose && !st.llmOutput.empty()) {
                std::cerr << " \"" << st.llmOutput << "\"";
            }
            std::cerr << "\n";
        }

        if (results.empty() && !st.taskComplete) {
            // No parsed actions and no final response. This is NOT completion.
            // Either the upstream returned no content, or the model emitted
            // bare/non-protocol text (or a non-final <response>). Recover
            // per runtime.mode / completion_policy — never silently finish.
            if (!streamStats.anyContent) {
                std::string detail;
                if (!streamStats.finishReason.empty())
                    detail +=
                        " (finish_reason=" + streamStats.finishReason + ")";
                if (!streamStats.lastError.empty())
                    detail += " — " + streamStats.lastError.substr(0, 200);
                if (streamStats.httpStatus > 0)
                    detail += " [http " +
                              std::to_string(streamStats.httpStatus) + "]";
                detail += " after " + std::to_string(sa.attempt + 1) + "/" +
                          std::to_string(sa.maxAttempts) + " attempt(s)";
                std::string visibleError =
                    "⚠ Model returned an empty response" + detail +
                    ". The agent loop is aborting this turn rather than "
                    "silently finishing. "
                    "Retry with a different model if this persists.";
                emit.status("[EMPTY RESPONSE]" + detail);
                emit.finish("empty_response", visibleError);
                responseOutput_ = visibleError;
                fullResponse = visibleError;
                st.taskComplete = true; // runtime failure, not model final
            } else if (sa.toolPlanCut) {
                std::string names;
                for (const auto& kv : tools_) {
                    if (!names.empty()) names += ", ";
                    names += kv.first;
                }
                if (names.empty()) names = "(none imported)";
                emit.harness(
                    "TOOL_PLAN",
                    "This generation dictated tools as native thinking "
                    "(list path / grep path / fs_read path …) with no <action> "
                    "tags. Those words are not executed. imported: [" + names +
                    "]",
                    "runtime");
                // Count as the one incomplete note so a second thought-only
                // gen still stalls. Do NOT stall THIS gen — next must emit
                // <action> with imported names.
                incompleteNoted = true;
            } else if (incompleteNoted && !finalizationTurn &&
                       ctx.iteration < workCap) {
                emit.harness(
                    "THOUGHT_STALL",
                    "Second thought-only generation after BARE_TEXT/NONFINAL. "
                    "No <action> and no <response final=\"true\">. "
                    "Stopping instead of burning the iteration cap.",
                    "limit");
                emit.finish("thought_stall",
                           "[THOUGHT_STALL] two thought-only gens — stopped");
                fullResponse = "[THOUGHT_STALL]";
                st.taskComplete = true;
                break;
            } else {
                steerIncompleteGeneration(ctx, iterationRawOutput, workCap,
                                          incompleteNoted, lastSalvage);
            }
        }

        // Capture exact model output plus runtime-injected results for
        // debugging. Do not add nested markdown headings here; iterations.md
        // already marks sections.
        {
            std::ostringstream os;
            // Provider reasoning first (SOH-routed thoughts) — visible here
            // so iterations.md shows what the model actually thought.
            if (!thoughtOutput_.empty()) {
                os << "<thought>" << thoughtOutput_ << "</thought>\n";
            }
            os << iterationRawOutput;
            if (!iterationRawOutput.empty() &&
                iterationRawOutput.back() != '\n')
                os << "\n";
            os << iterationRuntimeOutput;
            iterationOutputs_.push_back(os.str());
        }
        // Track the last substantive thought — used by the bounded
        // thought-to-final recovery at turn end (a reasoning model may put
        // its real answer in <thought> and forget final="true").
        if (!thoughtOutput_.empty() &&
            trimCopy(thoughtOutput_).size() > 80) {
            lastThoughtContent = trimCopy(thoughtOutput_);
        } else {
            // Protocol <thought> tag in the raw output — extract its body.
            std::string raw = iterationRawOutput;
            size_t open = raw.rfind("<thought>");
            if (open != std::string::npos) {
                size_t close = raw.find("</thought>", open);
                if (close != std::string::npos) {
                    std::string body =
                        trimCopy(raw.substr(open + 9, close - open - 9));
                    if (body.size() > 80) lastThoughtContent = body;
                }
            }
        }
        // DEV_MODE / verbose: rewrite dumps after every iteration so a crash
        // mid-turn still leaves the last LLM-facing prompt on disk.
        if (devMode_ || verbose_ || raw_ || ctx.debug)
            dumpSessionArtifacts();

        // Never force a follow-up after finalization — that turn is one-shot.
        bool forceResultFollowup =
            !finalizationTurn && st.taskComplete && !results.empty() &&
            iterationRawOutput.find("<action") != std::string::npos;

        // If the model emits action(s) and a final response in the same
        // generation, it cannot have seen the real runtime results yet. Keep
        // only the action transcript for the follow-up prompt; discard
        // premature response text and any model-owned result/prose.
        std::string historyOutput =
            forceResultFollowup ? st.actionTranscriptOutput : st.llmOutput;
        // Provider reasoning tokens (thinking/reasoning blocks) are routed
        // to the UI via SOH prefix but NOT to history. Wrap them as <thought>
        // so the next turn sees its own thinking inline.
        if (!thoughtOutput_.empty())
            historyOutput = "<thought>" + thoughtOutput_ + "</thought>\n" +
                            historyOutput;
        if (forceResultFollowup) {
            // The model cannot consume a sync action result in the same
            // generation that emitted the action. Force one follow-up turn with
            // the real <result> in context instead of accepting a premature
            // final. Also drop the premature response from history so the next
            // turn sees only the action it actually took plus the runtime
            // result.
            st.taskComplete = false;
            responseOutput_.clear();
        }

        if (st.taskComplete) {
            Json::Value expandedResponse =
                expandValueRefs(Json::Value(responseOutput_), actionResults_);
            fullResponse = expandedResponse.isString()
                               ? expandedResponse.asString()
                               : responseOutput_;
            // Vet-fix: final-turn used to `break` without appending the
            // Agent line. history_ then stayed User-only (plus any mid-turn
            // tool System lines), saveSession wrote records:[user], and
            // resume showed only the prompt. Always persist the final
            // answer (or the last action transcript if response is empty).
            {
                std::string finalHist =
                    !fullResponse.empty() ? fullResponse : historyOutput;
                if (!finalHist.empty()) {
                    const std::string needle = "Agent: " + finalHist;
                    if (history_.empty() || history_.back() != needle)
                        history_.push_back(needle);
                }
            }
            break;
        }

        // Prepare next iteration — push agent output, then system results.
        // Bare/non-final protocol retries already pushed the raw model output
        // plus a strict system correction above; don't add an empty duplicate.
        if (!st.nonFinalProtocolRetry)
            history_.push_back(
                "Agent: " + collapseDuplicateActionTags(historyOutput));
        if (!results.empty()) {
            for (auto &[id, result] : results) {
                std::ostringstream sysMsg;
                // FULL result into history — never Preview/compact. Starving
                // the next generation of tool/subagent truth is forbidden.
                sysMsg << buildResultTag(id, result, /*compact=*/false);
                history_.push_back("System: " + sysMsg.str());
            }
        }
        parser.clearResults(); // prevent result leakage to next iteration
        tickContextCycles();   // decrement peek cycles; auto-evict at 0

        // Operator steering — inject between iterations (soonest safe boundary).
        // Policy is embedded so the model keeps the plot unless told to drop it.
        {
            std::string steer = takeSteer();
            if (!steer.empty()) {
                history_.push_back("User: [STEER] " + steer);
                history_.push_back(
                    "System: [STEER POLICY] Operator guidance just arrived mid-turn.\n"
                    "- Incorporate it at the next natural step.\n"
                    "- Do NOT abandon current work unless the steer explicitly says "
                    "to stop/drop/leave it immediately (e.g. 'stop', 'drop that', "
                    "'forget the previous task', 'do this instead now').\n"
                    "- If you must switch immediately, park a one-line resume note "
                    "of what you were doing and return to it after the steer is done.\n"
                    "- Never treat the steer as a completed answer; keep using tools "
                    "and finish with <response final=\"true\"> when the whole job is done.");
                emit.status("[STEER] operator guidance injected");
                if (ctx.onToken)
                    ctx.onToken("", false);
            }
        }

        // Finalization is exactly one shot — never loop forever after cap.
        if (finalizationTurn) {
            finalizationDone = true;
            break;
        }
    }

    // Drain any leftover steer into history so it is not lost if the turn ended
    // before the next iteration (cancel / final / error).
    {
        std::string steer = takeSteer();
        if (!steer.empty()) {
            history_.push_back("User: [STEER · deferred] " + steer);
            history_.push_back(
                "System: [STEER] Guidance arrived as the previous turn ended. "
                "Address it on the next turn; resume prior unfinished work unless "
                "the steer explicitly cancels it.");
        }
    }

    if (fullResponse.empty()) {
        // Iteration budget exhausted (or cancelled path cleared response).
        // Per policy: promote salvage when allowed so small-model turns still
        // leave a usable answer + an honest recovery note in history.
        // Bounded thought-to-final: if the model put its real answer in a
        // substantive <thought> but forgot final="true", surface that content
        // as the result rather than a generic 'stopped without final' error.
        // This is turn-ended recovery, not mid-loop auto-promotion.
        if (compPolicy != CompPolicy::Strict && !lastSalvage.empty() &&
            !isThoughtEcho(lastSalvage, lastThoughtContent)) {
            emit.status(
                "[AUTO-PROMOTED @ CAP] No <response final=\"true\"> "
                "before iteration cap. Promoted salvaged content under "
                "runtime.mode=" +
                config_.runtimeMode + " / policy=" +
                (config_.completionPolicy.empty() ? std::string("(derived)")
                                                  : config_.completionPolicy) +
                ".");
            fullResponse = lastSalvage;
            responseOutput_ = lastSalvage;
            emit.finish("auto_promote_cap", lastSalvage);
        } else {
            fullResponse =
                "⚠ Agent stopped without emitting <response final=\"true\">. "
                "The runtime refused to treat non-final/bare output as "
                "completion" +
                (lastSalvage.empty() ? std::string(".")
                                     : " (salvage was available but "
                                       "completion_policy=strict).");
            emit.status("[STOP] no final response before cap/timeout");
            emit.finish("stop_no_final", fullResponse);
        }
    }

    // Vet-fix: skip auto-saves when the run produced no captured content
    // AND no live state worth persisting. Empty-then-create + empty-then-
    // checkpoint was creating pairs of zero-row files the Sessions page had
    // to filter out by hand.
    if (!ctx.ephemeral && !ctx.sessionId.empty()) {
        const bool hasContent =
            std::any_of(history_.begin(), history_.end(),
                        [](const std::string &h) { return !h.empty(); });
        if (hasContent || !contextFeeds_.empty()) {
            saveSession(ctx.sessionId);
            saveStateCheckpoint(ctx.sessionId);
        } else if (sessionMgr_.exists(ctx.sessionId)) {
            // Session previously persisted — refresh its checkpoint so
            // operator-controlled resume still works even after our newer
            // content-gating engagement above.
            saveStateCheckpoint(ctx.sessionId);
        }
    }

    liveIteration_.store(0, std::memory_order_relaxed);
    // After cancel/timeout the next prompt() may be minutes away — shrink now.
    applyCtxEconomyInPlace(ctx.iteration, /*force=*/false, ctx.sessionId);
    if (ctx.raw && !rawOutput.empty()) {
        return rawOutput;
    }
    return sanitize(fullResponse);
}




}  // namespace cortex::mk3
