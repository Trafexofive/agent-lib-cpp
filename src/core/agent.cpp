// =============================================================================
// agent-lib-MK3 — Agent Implementation
// Core loop: prompt → build messages → LLM generate → parse actions → dispatch
// → loop
// =============================================================================

#include "agent.hpp"
#include "agent_xml.hpp"
#include "agent_run_helpers.hpp"
#include "agent_harness.hpp"
#include "turn_emitter.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "../feeds/feed_engine.hpp"
#include "../protocol/noise.hpp"
#include "../tools/dispatch.hpp"
#include "../utils/ansi.hpp"
#include "dispatch.hpp"
#include "manifest_loader.hpp"

namespace cortex::mk3 {

// ── XML attribute escaping ──────────────────────────────────────────────

// Vet-fix: harness resolver. Searches a deterministic list of roots
// (any cwd, any host) so the agent never falls back to a hardcoded
// developer-machine absolute path. The order favours:
//   1. exact path the manifest loader resolved to (config_.harnessPath)
//   2. $CORTEX_HOME/manifests/harness/<relative>
//   3. ./manifests/harness/<relative>      (cwd-relative; standard repo layout)
//   4. ~/.config/cortex-mk3/manifests/harness/<relative>  (installed layout)
// If none match and the relative hint is empty, fall back to default.md in
// the same roots, in the same order. Returns empty string when nothing
// resolves; caller throws.


// ═══════════════════════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════════════════════

Agent::Agent(AgentConfig cfg, LlmProviderPtr provider)
    : config_(std::move(cfg)), provider_(std::move(provider)) {
    provider_->setModel(config_.model);
    provider_->setTemperature(config_.temperature);
    provider_->setMaxTokens(config_.maxTokens > 0 ? config_.maxTokens
                                                  : provider_->getMaxTokens());
    provider_->setTopP(config_.topP);
    provider_->setTopK(config_.topK);
    provider_->setPresencePenalty(config_.presencePenalty);
    provider_->setFrequencyPenalty(config_.frequencyPenalty);
    provider_->setStreamStallTimeoutSec(config_.streamStallTimeoutSec);

    for (auto &[k, v] : config_.environment)
        env_[k] = v;

    // Load system prompt
    if (!config_.systemPromptText.empty()) {
        systemPrompt_ = config_.systemPromptText;
    } else if (!config_.systemPromptPath.empty()) {
        std::ifstream f(config_.systemPromptPath);
        if (f) {
            std::ostringstream ss;
            ss << f.rdbuf();
            systemPrompt_ = ss.str();
        }
    }

    // Cache harness file once (doesn't change at runtime).
    // Pre-indent every line so buildSystemPrompt doesn't redo O(n) work per
    // turn.
    //
    // Vet-fix: harness resolution must NOT hardcode /home/mlamkadm.
    // The previous fallback was a developer-machine absolute path that
    // crashed on any other host and silently mis-resolved on the dev's
    // host when the build tree moved. Operator wants harness to load
    // consistently from $CORTEX_HOME -> ./manifests -> ~/.config lookups
    // regardless of where the agent was compiled or run.
    {
        std::vector<std::string> looked;
        std::string resolved = findHarnessPath(config_.harnessPath, looked);
        if (resolved.empty()) {
            std::string routes;
            for (std::size_t i = 0; i < looked.size(); ++i) {
                if (i)
                    routes += "\n  ";
                routes += looked[i] + (i + 1 < looked.size() ? " (miss)" : "");
            }
            throw std::runtime_error(
                "harness prompt not found — searched:\n  " + routes +
                "\nUse --manifest-dir <path> or set CORTEX_HOME. Default "
                "fallback is manifests/harness/default.md");
        }
        config_.harnessPath = resolved; // remember what we resolved to
        std::ifstream hf(resolved);
        if (hf.is_open()) {
            std::ostringstream oss;
            std::string line;
            while (std::getline(hf, line))
                oss << "    " << line << "\n";
            harnessText_ = oss.str();
        }
    }

    // Load persona prompt (identity/values)
    if (!config_.personaPath.empty()) {
        std::ifstream pf(config_.personaPath);
        if (pf) {
            std::ostringstream ss;
            ss << pf.rdbuf();
            personaText_ = ss.str();
        }
    }

    // Operator context (context.user → USER.md). Optional; silent if missing.
    if (!config_.userPath.empty()) {
        std::ifstream uf(config_.userPath);
        if (uf) {
            std::ostringstream ss;
            ss << uf.rdbuf();
            userText_ = ss.str();
        }
    }

    // Built-ins are registered in the backend registry below, but NOT granted
    // to this agent by default. Capabilities are declarative: a tool appears in
    // tools_ only when the active manifest imports it.

    // Register internal tool implementations and feeds
    tools::registerDefaults();
    feeds::registerFeeds();

    // Do not restore ./manifests/_session/tools.json automatically here.
    // Capabilities are declarative: the active manifest import block is the
    // runtime tool surface. Session tool files are legacy/reload artifacts and
    // must not silently leak stale tools into a fresh agent.
}

// ═══════════════════════════════════════════════════════════════════════
// Execution Entry Points
// ═══════════════════════════════════════════════════════════════════════

std::string Agent::prompt(const std::string &input,
                          const std::string &sessionId, bool ephemeral,
                          PromptSource source, const std::string &sourceName) {
    return prompt(input, nullptr, sessionId, ephemeral, source, sourceName);
}

std::string Agent::prompt(const std::string &input, StreamCallback onToken,
                          const std::string &sessionId, bool ephemeral,
                          PromptSource source, const std::string &sourceName) {
    AgentContext ctx;
    ctx.userInput = input;
    ctx.sessionId = sessionId;
    ctx.streaming = (onToken != nullptr);
    ctx.onToken = std::move(onToken);
    ctx.ephemeral = ephemeral;
    ctx.source = source;
    ctx.sourceName = sourceName;
    ctx.raw = raw_;
    ctx.verbose = verbose_;
    ctx.debug =
        (env_.count("__DEBUG_MODE__") && env_["__DEBUG_MODE__"] == "true") ||
        devMode_;
    // Vet-fix: do NOT loadSession when history_ already holds this turn's
    // seed (submitComposer → seedUserPrompt + saveSession). Reloading from
    // disk cleared in-memory history_ back to User-only, then the final
    // save often never rewrote Agent lines (taskComplete broke without
    // pushing Agent:), so resume showed only the prompt.
    //
    // Rules:
    //   - history empty → cold load from disk + checkpoint
    //   - history live + same or first session id → keep memory (seed)
    //   - history live + different lastSessionId_ → switch sessions
    if (!ephemeral && !sessionId.empty()) {
        if (history_.empty()) {
            loadSession(sessionId);
            loadStateCheckpoint(sessionId);
        } else if (!lastSessionId_.empty() && lastSessionId_ != sessionId) {
            history_.clear();
            contextFeeds_.clear();
            loadSession(sessionId);
            loadStateCheckpoint(sessionId);
        }
        // else: keep seeded / continuing history_
    }
    lastSessionId_ = sessionId;
    fallbackTriedThisTurn_ = false;
    fallbackSwappedThisTurn_ = false;

    TlsRunGuard tls(&runControl_);
    std::string result = runLoop(ctx);
    restorePrimaryIfFallback();

    dumpSessionArtifacts();

    return result;
}

void Agent::requestStop(RunStopKind k) {
    runControl_.requestStop(k);
    std::vector<std::shared_ptr<Agent>> kids;
    kids.reserve(subAgents_.size());
    for (const auto& kv : subAgents_)
        kids.push_back(kv.second);
    for (const auto& kptr : kids) {
        if (kptr)
            kptr->requestStop(k);
    }
}

void Agent::restorePrimaryIfFallback() {
    if (!fallbackSwappedThisTurn_ || !fallbackSavedProvider_)
        return;
    setProvider(fallbackSavedProvider_, fallbackSavedProviderName_,
                fallbackSavedModel_);
    fallbackSavedProvider_.reset();
    fallbackSwappedThisTurn_ = false;
}

Json::Value Agent::inspectContext(int lastN) const {
    Json::Value r(Json::objectValue);
    r["success"] = true;
    r["name"] = config_.name;
    r["provider"] = config_.provider;
    r["model"] = config_.model;
    r["response_output"] = responseOutput_;
    r["thought_bytes"] = static_cast<Json::UInt64>(thoughtOutput_.size());
    r["raw_bytes"] = static_cast<Json::UInt64>(rawLlOutput_.size());
    r["protocol_events"] = static_cast<int>(protocolEvents_.size());
    r["sub_agents"] = Json::Value(Json::arrayValue);
    for (const auto &kv : subAgents_)
        r["sub_agents"].append(kv.first);

    Json::Value hist(Json::arrayValue);
    int start = 0;
    if (lastN > 0 && static_cast<int>(history_.size()) > lastN)
        start = static_cast<int>(history_.size()) - lastN;
    for (int i = start; i < static_cast<int>(history_.size()); ++i) {
        const std::string &h = history_[static_cast<size_t>(i)];
        Json::Value entry(Json::objectValue);
        if (h.rfind("User: ", 0) == 0) {
            entry["role"] = "user";
            entry["content"] = h.substr(6);
        } else if (h.rfind("Parent(", 0) == 0) {
            entry["role"] = "parent";
            auto close = h.find(')');
            if (close != std::string::npos && close + 2 <= h.size()) {
                entry["from"] = h.substr(7, close - 7);
                entry["content"] = h.substr(close + 2); // skip ") "
                if (!entry["content"].asString().empty() &&
                    entry["content"].asString()[0] == ' ')
                    entry["content"] = entry["content"].asString().substr(1);
            } else {
                entry["content"] = h;
            }
        } else if (h.rfind("Agent: ", 0) == 0) {
            entry["role"] = "agent";
            entry["content"] = h.substr(7);
        } else if (h.rfind("System: ", 0) == 0) {
            entry["role"] = "system";
            entry["content"] = h.substr(8);
        } else {
            entry["role"] = "other";
            entry["content"] = h;
        }
        hist.append(entry);
    }
    r["history"] = hist;
    r["history_total"] = static_cast<int>(history_.size());
    r["context"] = contextSnapshot();
    return r;
}

// ═══════════════════════════════════════════════════════════════════════
// Core Loop
// ═══════════════════════════════════════════════════════════════════════
// Tool Dispatch
// ═══════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════

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
    size_t runEpochStart = continuation ? protocolEvents_.size() : size_t(0);
    if (!continuation) {
        protocolActions_.clear();
        protocolResults_.clear();
        protocolEvents_.clear();
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

    TurnEmitter emit{history_, protocolEvents_, ctx.onToken, ctx.iteration,
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
        ILlmProvider::StreamStats streamStats;
        int maxAttempts = std::max(1, 1 + config_.emptyResponseMaxRetries);
        int attempt = 0;
        int backoffMs = config_.emptyResponseInitialBackoffMs;
        for (;;) {
            if (attempt > 0) {
                // Reset per-iteration state for the retry attempt so the
                // next stream's tokens don't mix with the prior attempt.
                st.llmOutput.clear();
                st.actionTranscriptOutput.clear();
                iterationRawOutput.clear();
                iterationRuntimeOutput.clear();
                responseOutput_.clear();
                thoughtOutput_.clear();
                protocolEvents_.clear();
                parser.reset();

                // Out-of-band retry marker — bridge consumes this and
                // resets its known-protocol baseline so the retry stream
                // starts without double-rendering attempt-N streams.
                {
                    ProtocolEvent retryMarker;
                    retryMarker.kind = ProtocolEventKind::RETRY;
                    retryMarker.text = std::string("retry ") +
                                       std::to_string(attempt) + " / " +
                                       std::to_string(maxAttempts - 1);
                    protocolEvents_.push_back(std::move(retryMarker));
                }

                int delay =
                    std::min(backoffMs, config_.emptyResponseMaxBackoffMs);
                // Vet-fix: also fire the structured retry signal so a TUI hook
                // can publish a Notification card. The bridge collapses by
                // source+id; the same callback path makes network/HTTP and
                // empty-response retries visually equivalent.
                if (retryHandler_) {
                    RetrySignal rs;
                    rs.kind = RetrySignal::Kind::Network; // empty-response =
                                                          // upstream silence
                    rs.attempt = attempt;
                    rs.maxAttempts = std::max(0, maxAttempts - 1);
                    rs.curlError = "empty-response";
                    rs.backoffMs = delay;
                    retryHandler_(rs);
                }
                if (!silenceTerminal_ && ctx.debug) {
                    std::cerr
                        << "[MK3:RETRY] empty-response attempt=" << attempt
                        << " delay_ms=" << delay << " finish_reason=\""
                        << streamStats.finishReason << "\" any_content="
                        << (streamStats.anyContent ? "true" : "false") << "\n";
                }
                auto deadline = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(delay);
                while (g_running &&
                       std::chrono::steady_clock::now() < deadline) {
                    auto step = std::min(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now()),
                        std::chrono::milliseconds(100));
                    if (step.count() > 0)
                        std::this_thread::sleep_for(step);
                }
                if (!g_running)
                    break;
                backoffMs = std::min(
                    static_cast<int>(backoffMs *
                                     config_.emptyResponseBackoffMultiplier),
                    config_.emptyResponseMaxBackoffMs);
            }

            try {
                provider_->generateStream(
                    msgs, [&](const std::string &token, bool isFinal) {
                        if (st.taskComplete)
                            return;
                        // Route thinking tokens (\x01 prefix) to thought stream
                        // — live dimmed
                        if (!token.empty() && token[0] == '\x01') {
                            std::string thoughtChunk = token.substr(1);
                            thoughtOutput_ += thoughtChunk;
                            if (!thoughtChunk.empty()) {
                                // Only merge into THIS run's open thought — never
                                // append into a prior-turn THOUGHT left in the
                                // vector on continuation (streams-into-old-block).
                                const bool canMerge =
                                    protocolEvents_.size() > runEpochStart &&
                                    protocolEvents_.back().kind ==
                                        ProtocolEventKind::THOUGHT;
                                if (canMerge) {
                                    protocolEvents_.back().text += thoughtChunk;
                                } else {
                                    protocolEvents_.push_back(
                                        {ProtocolEventKind::THOUGHT,
                                         thoughtChunk,
                                         {},
                                         {}});
                                }
                            }
                            if (ctx.onToken)
                                ctx.onToken("", false); // trigger render
                            return;
                        }
                        rawLlOutput_ += token; // cumulative model/runtime trace
                        iterationRawOutput +=
                            token; // exact model bytes this iteration
                        if (ctx.raw)
                            rawOutput += token;
                        parser.feed(token, isFinal);
                        if (ctx.onToken)
                            ctx.onToken("", isFinal);
                    });
            } catch (const std::exception &e) {
                const std::string msg = e.what() ? e.what() : "";
                const auto sk = currentRunStopKind();
                const TransportClass tc = classifyTransportError(msg, sk);
                const bool isStreamAbort = tc == TransportClass::StreamAbort;
                const bool isTimeout = tc == TransportClass::Timeout;
                const bool isTransientNet = transportIsRetryable(tc);
                const bool isRegionOrForbidden =
                    tc == TransportClass::RegionForbidden;

                if (tc == TransportClass::HardExternal) {
                    fullResponse = "[timed out]";
                    emit.harness("TIMEOUT",
                                "External signal (SIGTERM / wall timeout) stopped "
                                "the process — not a flaky stream to FALLBACK.",
                                "limit");
                    emit.finish("timeout", "[timed out · external signal]");
                    iterationOutputs_.push_back("[timed out]");
                    break;
                }
                if (tc == TransportClass::HardOperator ||
                    (!g_running && sk == RunStopKind::Operator)) {
                    fullResponse = "[cancelled]";
                    emit.harness("CANCEL",
                                "Operator stopped the turn (Ctrl-C/X). Halt the plan.",
                                "limit");
                    emit.finish("cancel", "[cancelled by operator]");
                    iterationOutputs_.push_back("[cancelled]");
                    break;
                }

                if (!g_running && !isStreamAbort && !isTransientNet &&
                    sk == RunStopKind::None) {
                    fullResponse = "[cancelled]";
                    emit.harness("CANCEL", "Operator stopped the turn.", "limit");
                    emit.finish("cancel", "[cancelled by operator]");
                    iterationOutputs_.push_back("[cancelled]");
                    break;
                }

                if (isStreamAbort && !g_hardKill.load(std::memory_order_acquire))
                    runControl_.clearSoft();

                auto backoffSleep = [&]() {
                    int waitMs = std::min(
                        config_.emptyResponseInitialBackoffMs * (attempt + 1),
                        config_.emptyResponseMaxBackoffMs);
                    auto step = std::chrono::milliseconds(100);
                    auto left = std::chrono::milliseconds(waitMs);
                    while (g_running && left.count() > 0) {
                        auto slice = left < step ? left : step;
                        std::this_thread::sleep_for(slice);
                        left -= slice;
                    }
                };

                // ── 3) PRIMARY retries first (exhaust maxAttempts)
                if (isTransientNet && attempt + 1 < maxAttempts) {
                    std::string reason = msg;
                    if (reason.find("Failed writing received data") !=
                        std::string::npos) {
                        reason =
                            "stream aborted mid-read (stall/callback) — not disk; " +
                            msg;
                    }
                    emit.status("[RETRY] primary " + config_.provider + "/" +
                               config_.model + " · attempt " +
                               std::to_string(attempt + 1) + "/" +
                               std::to_string(maxAttempts) + " · " + reason);
                    history_.push_back(
                        "System: <harness code=\"RETRY\" kind=\"runtime\">\n"
                        "Primary stream failed (transient). Retrying same provider "
                        "before any FALLBACK.\nreason: " +
                        reason + "\n</harness>");
                    if (ctx.onToken) ctx.onToken("", false);
                    backoffSleep();
                    ++attempt;
                    continue;
                }

                // ── 4) FALLBACK only after primary retries exhausted OR hard 403
                const bool primaryExhausted = (attempt + 1 >= maxAttempts);
                const bool mayFallback =
                    !fallbackTriedThisTurn_ && !config_.fallbackProvider.empty() &&
                    !config_.fallbackModel.empty() &&
                    (isRegionOrForbidden || (isTransientNet && primaryExhausted));
                if (mayFallback) {
                    auto fb = providers::createProvider(config_.fallbackProvider,
                                                        config_.fallbackModel);
                    if (fb) {
                        fallbackTriedThisTurn_ = true;
                        fb->setQuietLogs(silenceTerminal_);
                        const std::string from =
                            config_.provider + "/" + config_.model;
                        const std::string to =
                            config_.fallbackProvider + "/" +
                            config_.fallbackModel;
                        if (!fallbackSwappedThisTurn_) {
                            fallbackSavedProvider_ = provider_;
                            fallbackSavedProviderName_ = config_.provider;
                            fallbackSavedModel_ = config_.model;
                            fallbackSwappedThisTurn_ = true;
                        }
                        setProvider(fb, config_.fallbackProvider,
                                    config_.fallbackModel);
                        std::string reason = msg;
                        if (reason.find("Failed writing received data") !=
                            std::string::npos) {
                            reason =
                                "stream aborted mid-read after " +
                                std::to_string(maxAttempts) +
                                " primary attempt(s) — not a disk error; " + msg;
                        }
                        emit.status("[FALLBACK] " + from + " → " + to +
                                   " · after " + std::to_string(maxAttempts) +
                                   " primary attempt(s) · reason: " + reason);
                        history_.push_back(
                            "System: <harness kind=\"runtime\" code=\"FALLBACK\">\n"
                            "primary " + from + " exhausted retries.\n"
                            "switched to " + to + " for the rest of this turn.\n"
                            "reason: " + reason + "\n"
                            "Do not assume the original provider is live.\n"
                            "</harness>");
                        // Fresh attempt budget on the fallback provider.
                        attempt = 0;
                        continue;
                    }
                }

                // ── 5) Terminal failure
                std::ostringstream notice;
                if (isTimeout) {
                    notice << "⚠ Upstream stream TIMED OUT after retries.\n"
                           << "Detail: " << msg << "\n"
                           << "This is a transport failure — not a completed answer.";
                } else if (isTransientNet) {
                    notice << "⚠ Upstream network/HTTP failure after "
                           << maxAttempts << " primary attempt(s)"
                           << (fallbackTriedThisTurn_ ? " (+ fallback)" : "")
                           << ".\nDetail: " << msg << "\n"
                           << "Turn aborted without a final response.";
                } else {
                    notice << "⚠ Provider error: " << msg << "\n"
                           << "Turn aborted without a final response.";
                }
                const std::string err = notice.str();
                rawLlOutput_ += err;
                iterationRawOutput += err;
                iterationOutputs_.push_back(err);
                emit.status(std::string(isTimeout ? "[TIMEOUT] " : "[ERROR] ") + msg);
                history_.push_back(
                    "System: [PROVIDER " +
                    std::string(isTimeout ? "TIMEOUT" : "ERROR") + "] " + msg +
                    " — previous turn did not complete. Do not treat any partial "
                    "output as final; continue or ask the operator to retry.");
                emit.finish("provider_error", err);
                fullResponse = err;
                responseOutput_ = err;
                break;
            }

            streamStats = provider_ ? provider_->lastStreamStats()
                                    : ILlmProvider::StreamStats{};

            // Decide whether to retry. Don't retry on legitimate content;
            // only on upstream-side transient failures (empty / filtered /
            // length-truncated / configured reasons).
            bool shouldRetry = (attempt + 1 < maxAttempts);
            if (shouldRetry) {
                if (streamStats.anyContent) {
                    bool retryForReason = false;
                    if (config_.retryOnFinishReasonLength &&
                        streamStats.finishReason == "length")
                        retryForReason = true;
                    if (config_.retryOnFinishReasonContentFilter &&
                        (streamStats.finishReason == "content_filter" ||
                         streamStats.finishReason == "empty"))
                        retryForReason = true;
                    for (const auto &r : config_.retryOnFinishReasons) {
                        if (streamStats.finishReason == r) {
                            retryForReason = true;
                            break;
                        }
                    }
                    shouldRetry = retryForReason;
                }
                // else: !anyContent → always retry
            }

            if (!shouldRetry)
                break;
            ++attempt;
        }

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
                detail += " after " + std::to_string(attempt + 1) + "/" +
                          std::to_string(maxAttempts) + " attempt(s)";
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
            } else {
                if (incompleteNoted && !finalizationTurn &&
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
                }
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
        protocolEvents_.push_back({ProtocolEventKind::STATUS,
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
        protocolEvents_.push_back(
            {ProtocolEventKind::STATUS,
             "[BARE_TEXT] untagged kept as thought — loop continues",
             {}, {}});
    }
    if (ctx.onToken) ctx.onToken("", false);
}

void Agent::publishCleanThought(ProtocolStreamState &st, const std::string &rawAppend) {
    if (st.thoughtDroppedAsNoise) return;  // rest of segment is dead air
    if (rawAppend.empty() && st.thoughtRawBuf.empty())
        return;
    st.thoughtRawBuf += rawAppend;
    // Cap raw thought buffer so a tool-echo / nm dump cannot grow without bound
    // during streaming (UI still rate-limits display; this protects the agent).
    constexpr size_t kThoughtRawCap = 48 * 1024;
    if (st.thoughtRawBuf.size() > kThoughtRawCap)
        st.thoughtRawBuf = st.thoughtRawBuf.substr(st.thoughtRawBuf.size() - kThoughtRawCap);

    // Symbol dumps / pure protocol debris → drop the THOUGHT event entirely.
    if (protocol::looksLikeSymbolDump(st.thoughtRawBuf) ||
        protocol::isThoughtNoise(st.thoughtRawBuf)) {
        if (st.thoughtEventIdx != static_cast<size_t>(-1) &&
            st.thoughtEventIdx < protocolEvents_.size() &&
            protocolEvents_[st.thoughtEventIdx].kind ==
                ProtocolEventKind::THOUGHT) {
            protocolEvents_.erase(
                protocolEvents_.begin() +
                static_cast<std::ptrdiff_t>(st.thoughtEventIdx));
        }
        st.thoughtEventIdx = static_cast<size_t>(-1);
        st.thoughtDroppedAsNoise = true;
        st.thoughtRawBuf.clear();  // free the dump from RAM
        thoughtOutput_.clear();
        return;
    }

    std::string cleaned = protocol::stripProtocolNoise(st.thoughtRawBuf);
    if (cleaned.empty()) {
        if (st.thoughtEventIdx != static_cast<size_t>(-1) &&
            st.thoughtEventIdx < protocolEvents_.size() &&
            protocolEvents_[st.thoughtEventIdx].kind ==
                ProtocolEventKind::THOUGHT) {
            protocolEvents_.erase(
                protocolEvents_.begin() +
                static_cast<std::ptrdiff_t>(st.thoughtEventIdx));
        }
        st.thoughtEventIdx = static_cast<size_t>(-1);
        return;
    }
    // Soft cap cleaned thought text published to the UI (not the LLM history).
    // 12KB was clipping long reasoning mid-audit; keep more for operator scroll.
    constexpr size_t kThoughtPubCap = 64 * 1024;
    if (cleaned.size() > kThoughtPubCap)
        cleaned = cleaned.substr(0, kThoughtPubCap - 48) +
                  "\n…[thought UI safety; full stream in dump raw/iterations]";
    thoughtOutput_ = cleaned; // authoritative cleaned form for this run
    auto reuseThought = [&]() -> size_t {
        if (st.thoughtEventIdx != static_cast<size_t>(-1) &&
            st.thoughtEventIdx < protocolEvents_.size() &&
            protocolEvents_[st.thoughtEventIdx].kind ==
                ProtocolEventKind::THOUGHT)
            return st.thoughtEventIdx;
        // One thought well per prompt() epoch, unless an ACTION sealed it.
        for (size_t i = protocolEvents_.size(); i-- > st.runEpochStart;) {
            if (protocolEvents_[i].kind == ProtocolEventKind::ACTION)
                return static_cast<size_t>(-1);
            if (protocolEvents_[i].kind == ProtocolEventKind::THOUGHT)
                return i;
        }
        return static_cast<size_t>(-1);
    };
    size_t idx = reuseThought();
    if (idx != static_cast<size_t>(-1)) {
        std::string& dst = protocolEvents_[idx].text;
        if (cleaned.find(dst) != std::string::npos)
            dst = cleaned;
        else if (dst.find(cleaned) == std::string::npos) {
            dst += "\n\n---\n\n";
            dst += cleaned;
        }
        st.thoughtEventIdx = idx;
    } else {
        st.thoughtEventIdx = protocolEvents_.size();
        protocolEvents_.push_back(
            {ProtocolEventKind::THOUGHT, cleaned, {}, {}});
    }
}

void Agent::handleProtocolEvent(AgentContext &ctx, ProtocolStreamState &st,
                                const protocol::TokenEvent &ev) {
    switch (ev.type) {
    case protocol::TokenEvent::TEXT: {
    publishCleanThought(st, ev.content);
    break;
    }

    case protocol::TokenEvent::RESPONSE:
    // Seal thought segment — next bare text is a new thought.
    st.thoughtRawBuf.clear();
    st.thoughtDroppedAsNoise = false;
    st.thoughtEventIdx = static_cast<size_t>(-1);
    st.llmOutput += ev.content;
    responseOutput_ += ev.content;
    if (!ev.content.empty()) {
        // Use raw content directly — stripProtocolNoise is too aggressive
        // for response text (strips spaces around UTF-8 punctuation like em dashes).
        // The parser already extracted clean content between <response> tags.
        std::string paint = ev.content;
        auto prevSame = [&](ProtocolEventKind k) {
            for (size_t i = protocolEvents_.size();
                 i > st.runEpochStart;) {
                --i;
                if (protocolEvents_[i].kind == k)
                    return protocolEvents_.begin() + i;
            }
            return protocolEvents_.end();
        };
        if (auto it = prevSame(ProtocolEventKind::RESPONSE);
            it != protocolEvents_.end()) {
            it->text += paint;
        } else {
            protocolEvents_.push_back(
                {ProtocolEventKind::RESPONSE, paint, {}, {}});
        }
    }
    if (ctx.onToken)
        ctx.onToken(ev.content, false);
    if (ev.metadata.count("is_final") &&
        ev.metadata.at("is_final") == "true") {
        st.taskComplete = true;
    }
    break;

    case protocol::TokenEvent::THOUGHT: {
    publishCleanThought(st, ev.content);
    break;
    }

    case protocol::TokenEvent::ACTION_START:
    // Seal thought segment before the action card.
    st.thoughtRawBuf.clear();
    st.thoughtDroppedAsNoise = false;
    st.thoughtEventIdx = static_cast<size_t>(-1);
    if (ev.action) {
        // Store protocol action for TUI/timeline regardless of
        // raw/debug; debug mode must not hide the action/result UI.
        std::string typeStr;
        switch (ev.action->type) {
        case protocol::ActionType::AGENT:
            typeStr = "agent";
            break;
        case protocol::ActionType::RELIC:
            typeStr = "relic";
            break;
        case protocol::ActionType::FEED:
            typeStr = "feed";
            break;
        case protocol::ActionType::WORKFLOW:
            typeStr = "workflow";
            break;
        default:
            typeStr = "tool";
            break;
        }
        // Provisional actions (open-tag-only, body not yet streamed) carry an
        // empty `{}` params object. Serializing that to "{}" produced a bogus
        // body that the headless byte-delta renderer then mis-diffed: it treats
        // a same-id merge as a prefix-append, so "{}" → full JSON emitted the
        // body minus its leading `{"` (corrupt action display). Leave the
        // provisional body empty so the merge is a clean append.
        const bool provisional =
            ev.metadata.count("provisional") &&
            ev.metadata.at("provisional") == "true";
        std::string body = ev.action->content;
        if (body.empty() && !ev.action->params.isNull() && !provisional) {
            Json::StreamWriterBuilder wb;
            wb["indentation"] = "";
            body = Json::writeString(wb, ev.action->params);
        }
        std::string modeStr;
        switch (ev.action->mode) {
        case protocol::ExecutionMode::ASYNC:
            modeStr = "async";
            break;
        case protocol::ExecutionMode::FIRE_AND_FORGET:
            modeStr = "fire_and_forget";
            break;
        default:
            modeStr = "sync";
            break;
        }
        std::map<std::string, std::string> modifiers;
        if (ev.action->params.isObject()) {
            static const std::unordered_set<std::string> reserved =
                {"type", "name",       "id",
                 "mode", "depends_on", "timeout"};
            for (const auto &key :
                 ev.action->params.getMemberNames()) {
                if (reserved.count(key))
                    continue;
                Json::StreamWriterBuilder aw;
                aw["indentation"] = "";
                modifiers[key] =
                    Json::writeString(aw, ev.action->params[key]);
            }
        }
        ProtocolAction protocolAction{
            typeStr,           ev.action->name, ev.action->id, body,
            modeStr == "sync", modeStr,         modifiers};
        // Provisional open-tag then full close share one id —
        // update the existing ACTION event/card in place
        // (stream-as-fast-as-parse).
        bool merged = false;
        for (auto it = protocolEvents_.rbegin();
             it != protocolEvents_.rend(); ++it) {
            if (it->kind == ProtocolEventKind::ACTION &&
                it->action.id == protocolAction.id) {
                it->action = protocolAction;
                merged = true;
                break;
            }
        }
        if (!merged) {
            protocolActions_.push_back(protocolAction);
            protocolEvents_.push_back({ProtocolEventKind::ACTION,
                                       "",
                                       protocolAction,
                                       {}});
        } else {
            // Keep protocolActions_ tail in sync when present.
            for (auto it = protocolActions_.rbegin();
                 it != protocolActions_.rend(); ++it) {
                if (it->id == protocolAction.id) {
                    *it = protocolAction;
                    break;
                }
            }
        }
        // Notify the TUI immediately on ACTION_START, before
        // sync dispatch blocks on tools/sub-agents. The action
        // card must render first; results arrive later.
        if (ctx.onToken)
            ctx.onToken("", false);
        std::ostringstream ax;
        ax << "<action type=\""
           << (ev.action->type == protocol::ActionType::TOOL
                   ? "tool"
               : ev.action->type == protocol::ActionType::AGENT
                   ? "agent"
               : ev.action->type == protocol::ActionType::RELIC
                   ? "relic"
               : ev.action->type == protocol::ActionType::WORKFLOW
                   ? "workflow"
                   : "feed")
           << "\" name=\"" << ev.action->name << "\" id=\""
           << ev.action->id << "\" mode=\""
           << (ev.action->mode == protocol::ExecutionMode::SYNC
                   ? "sync"
                   : "async")
           << "\"";
        if (!ev.action->content.empty() &&
            ev.action->params.isObject()) {
            for (const auto &key :
                 ev.action->params.getMemberNames()) {
                const auto &v = ev.action->params[key];
                if (v.isObject() || v.isArray())
                    continue;
                std::string val;
                if (v.isString())
                    val = v.asString();
                else {
                    Json::StreamWriterBuilder aw;
                    aw["indentation"] = "";
                    val = Json::writeString(aw, v);
                }
                ax << " " << key << "=\"" << xmlAttr(val) << "\"";
            }
        }
        ax << ">";
        if (!ev.action->content.empty()) {
            ax << ev.action->content;
        } else if (!ev.action->params.isNull()) {
            Json::StreamWriterBuilder w;
            w["indentation"] = "";
            ax << Json::writeString(w, ev.action->params);
        }
        ax << "</action>";
        st.llmOutput += ax.str() + "\n";
        st.actionTranscriptOutput += ax.str() + "\n";
    }
    break;

    case protocol::TokenEvent::ACTION_RESULT:
    break;

    case protocol::TokenEvent::ERROR:
    if (ev.metadata.count("reason") &&
        ev.metadata.at("reason") == "forged_result") {
        // Inline XML correction — the model must see it never emits <result>.
        history_.push_back(
            "System: <thought>Forged <result> tags are ignored by the "
            "runtime. Never emit <result> — real results are injected "
            "automatically after each action. Use the injected results, "
            "not invented ones.</thought>");
    } else {
        history_.push_back(
            "[ERROR] action=" + (ev.action ? ev.action->name : "?") +
            " id=" +
            (ev.metadata.count("id") ? ev.metadata.at("id") : "?") +
            " reason=" +
            (ev.metadata.count("reason") ? ev.metadata.at("reason") : "?") +
            ": " + ev.content);
    }
    break;

    case protocol::TokenEvent::CONTEXT_FEED:
    break;

    default:
    break;
    }
}

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
        protocolEvents_.push_back(
            {ProtocolEventKind::RESULT, "", {}, protocolResult});
        // Notify callback so TUI can stream tool results
        // immediately
        if (ctx.onToken && ctx.streaming)
            ctx.onToken("", false);
    }

    return result;
}

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

// ── Tool dispatch — see agent_tool_dispatch.cpp
// ── Session lifecycle — see agent_session.cpp

} // namespace cortex::mk3
