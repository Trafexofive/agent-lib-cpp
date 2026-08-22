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

void Agent::streamUntilSettled(AgentContext &ctx, StreamAttempt &a) {
    ProtocolStreamState &st = *a.st;
    protocol::Parser &parser = *a.parser;
    ChatMessages &msgs = *a.msgs;
    TurnEmitter &emit = *a.emit;
    std::string &iterationRawOutput = *a.iterationRawOutput;
    std::string &iterationRuntimeOutput = *a.iterationRuntimeOutput;
    std::string &rawOutput = *a.rawOutput;
    std::string &fullResponse = *a.fullResponse;
    const size_t runEpochStart = a.runEpochStart;

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
                protocol_.clear();
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
                    protocol_.push(std::move(retryMarker));
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
                        << a.stats.finishReason << "\" any_content="
                        << (a.stats.anyContent ? "true" : "false") << "\n";
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
                size_t leftoverAfterSettle = 0;
                size_t preActionThink = 0;
                bool batchSettled = false;
                auto cutLeftover = [&]() {
                    if (leftoverAfterSettle < 4096 || !provider_)
                        return;
                    const std::string dropped = parser.dropHollowProvisional();
                    if (!dropped.empty()) {
                        protocol_.mutate([&](std::vector<ProtocolEvent>& evs) {
                            for (auto it = evs.begin(); it != evs.end();) {
                                if (it->kind == ProtocolEventKind::ACTION &&
                                    it->action.id == dropped &&
                                    it->action.body.empty())
                                    it = evs.erase(it);
                                else
                                    ++it;
                            }
                        });
                    }
                    provider_->abortGeneration();
                };
                provider_->generateStream(
                    msgs, [&](const std::string &token, bool isFinal) {
                        if (st.taskComplete)
                            return;
                        if (provider_ && provider_->generationAborted())
                            return;
                        // Route thinking tokens (\x01 prefix) to thought stream
                        // — live dimmed
                        if (!token.empty() && token[0] == '\x01') {
                            std::string thoughtChunk = token.substr(1);
                            if (!thoughtChunk.empty())
                                publishCleanThought(st, thoughtChunk);
                            if (batchSettled) {
                                leftoverAfterSettle += thoughtChunk.size();
                                cutLeftover();
                            } else {
                                // Furnace before first <action>: expanding
                                // "I'll scout… list path … grep path" as native
                                // thinking with open=0. Not leftover-after-batch.
                                preActionThink += thoughtChunk.size();
                                if (preActionThink >= 4096 && provider_ &&
                                    (st.thoughtDroppedAsNoise ||
                                     protocol::looksLikeToolPlanDump(st.thoughtRawBuf) ||
                                     protocol::looksLikeToolPlanDump(thoughtOutput_)))
                                    provider_->abortGeneration();
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
                        if (parser.generationSettled()) {
                            if (!batchSettled) leftoverAfterSettle = 0;
                            batchSettled = true;
                            leftoverAfterSettle += token.size();
                            cutLeftover();
                        } else {
                            batchSettled = false;
                            leftoverAfterSettle = 0;
                        }
                        if (ctx.onToken)
                            ctx.onToken("", isFinal);
                    });
            } catch (const std::exception &e) {
                const std::string msg = e.what() ? e.what() : "";
                const auto sk = currentRunStopKind();
                const TransportClass tc = classifyTransportError(msg, sk);
                const bool hasFb = !config_.fallbackProvider.empty() &&
                                   !config_.fallbackModel.empty();
                const CatchDecision dec = decideTransportCatch(
                    tc, sk, static_cast<bool>(g_running), attempt, maxAttempts,
                    fallbackTriedThisTurn_, hasFb);
                if (dec.clearSoft && !g_hardKill.load(std::memory_order_acquire))
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

                auto streamReason = [&]() {
                    if (msg.find("Failed writing received data") != std::string::npos)
                        return std::string("stream aborted mid-read (stall/callback) — not disk; ") +
                               msg;
                    return msg;
                };

                if (dec.action == CatchAction::HardTimeout) {
                    fullResponse = "[timed out]";
                    emit.harness("TIMEOUT",
                                "External signal (SIGTERM / wall timeout) stopped "
                                "the process — not a flaky stream to FALLBACK.",
                                "limit");
                    emit.finish("timeout", "[timed out · external signal]");
                    iterationOutputs_.push_back("[timed out]");
                    break;
                }
                if (dec.action == CatchAction::HardCancel) {
                    fullResponse = "[cancelled]";
                    emit.harness("CANCEL",
                                "Operator stopped the turn (Ctrl-C/X). Halt the plan.",
                                "limit");
                    emit.finish("cancel", "[cancelled by operator]");
                    iterationOutputs_.push_back("[cancelled]");
                    break;
                }
                if (dec.action == CatchAction::RetryPrimary) {
                    const std::string reason = streamReason();
                    emit.status("[RETRY] primary " + config_.provider + "/" +
                               config_.model + " · attempt " +
                               std::to_string(attempt + 1) + "/" +
                               std::to_string(maxAttempts) + " · " + reason);
                    emit.harness("RETRY",
                                 "Primary stream failed (transient). Retrying same "
                                 "provider before any FALLBACK. reason: " + reason);
                    backoffSleep();
                    ++attempt;
                    continue;
                }
                if (dec.action == CatchAction::Fallback) {
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
                        std::string reason = streamReason();
                        emit.status("[FALLBACK] " + from + " → " + to +
                                   " · after " + std::to_string(maxAttempts) +
                                   " primary attempt(s) · reason: " + reason);
                        emit.harness("FALLBACK",
                                     "primary " + from + " exhausted retries. "
                                     "switched to " + to +
                                     " for the rest of this turn. reason: " + reason);
                        attempt = 0;
                        continue;
                    }
                }

                std::ostringstream notice;
                const bool transient = transportIsRetryable(tc);
                if (dec.isTimeout) {
                    notice << "⚠ Upstream stream TIMED OUT after retries.\n"
                           << "Detail: " << msg << "\n"
                           << "This is a transport failure — not a completed answer.";
                } else if (transient) {
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
                emit.status(std::string(dec.isTimeout ? "[TIMEOUT] " : "[ERROR] ") +
                            msg);
                emit.harness(dec.isTimeout ? "TIMEOUT" : "ERROR",
                             msg + " — previous turn did not complete. Do not treat "
                             "any partial output as final.");
                emit.finish("provider_error", err);
                fullResponse = err;
                responseOutput_ = err;
                break;
            }

            a.stats = provider_ ? provider_->lastStreamStats()
                                    : ILlmProvider::StreamStats{};
            if (!shouldRetryEmptyStream(config_, attempt, maxAttempts,
                                        a.stats.anyContent,
                                        a.stats.finishReason))
                break;
            ++attempt;
        }
        a.attempt = attempt;
        a.maxAttempts = maxAttempts;

}


}  // namespace cortex::mk3
