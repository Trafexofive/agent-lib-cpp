#pragma once
// Agent turn streaming: bridge an Agent::prompt() stream into UI events.
// Token coalescing / protocol diff / retry badges / TurnDone-error-cancel.
// Nothing here touches the inkcell app shell — pure bridge<->agent conduit.

#include <atomic>
#include <chrono>
#include <exception>
#include <string>
#include <vector>

#include "src/core/agent.hpp"
#include "src/core/provider.hpp"
#include "src/ui/bridge/agent_bridge.hpp"
#include "src/ui/bridge/ui_event.hpp"
#include "src/ui/model/protocol_event_diff.hpp"

namespace cortex::mk3::ui {

inline void runAgentTurn(AgentBridge &bridge, Agent &agent,
                         const std::string &prompt,
                         const std::string &sessionId, bool ephemeral,
                         std::atomic<bool> &done) {
    try {
        bridge.publish(UiEvent::status("agent running"));
        // TUI owns the alternate screen. Any agent/provider stderr mid-frame
        // corrupts cells (prompt leakage, "eaten spaces", overlapping blocks).
        // Dev dumps still write files under .cortex/dev/ — just never the tty.
        agent.setSilenceTerminal(true);
        {
            auto provider = agent.provider();
            if (provider)
                provider->setQuietLogs(true);
        }
        agent.setRetryHandler(
            [&bridge, &agent, sessionId](const RetrySignal &rs) {
                std::string source =
                    rs.kind == RetrySignal::Kind::Http ? "http" : "network";
                std::string detail =
                    rs.kind == RetrySignal::Kind::Http
                        ? std::string("HTTP ") + std::to_string(rs.httpStatus)
                        : rs.curlError;
                std::string title = source + " retry · " + detail;
                std::string fmt = std::string("attempt ") +
                                  std::to_string(rs.attempt) + "/" +
                                  std::to_string(rs.maxAttempts) + " · " +
                                  std::to_string(rs.backoffMs / 1000) + "s";
                // Stable id per (provider, turn) so retries collapse to one
                // badge.
                std::string id =
                    std::string("retry:") + agent.name() + ":" + sessionId;
                bridge.publish(
                    UiEvent::notification(source, "warn", title + " — " + fmt,
                                          rs.attempt, rs.maxAttempts, id));
            });
        size_t rawSeen = 0;
        std::vector<ProtocolEvent> previousEvents;
        auto lastUiFlush =
            std::chrono::steady_clock::now() - std::chrono::milliseconds(16);
        auto onToken = [&](const std::string &token, bool finalChunk) {
            // Stream-as-fast-as-parse: never delay a closed (or provisional)
            // protocol event. Only coalesce pure byte heartbeats (~60fps).
            const auto &cur = agent.protocolEvents();
            bool protocolDirty = cur.size() != previousEvents.size();
            if (!protocolDirty) {
                for (size_t i = 0; i < cur.size(); ++i) {
                    if (i >= previousEvents.size() ||
                        !sameProtocolEvent(cur[i], previousEvents[i])) {
                        protocolDirty = true;
                        break;
                    }
                }
            }
            auto now = std::chrono::steady_clock::now();
            if (!finalChunk && !protocolDirty &&
                now - lastUiFlush < std::chrono::milliseconds(16))
                return;
            lastUiFlush = now;
            std::vector<UiEvent> batch;
            // Forwarded child tokens (non-empty) publish directly so the UI
            // stays alive during a synchronous sub-agent call — the child's
            // bytes don't appear in the parent's rawLlOutput, so the delta
            // read below wouldn't see them. For the parent's own stream,
            // 'token' is empty (content lands in rawLlOutput) and the delta
            // read handles it.
            if (!token.empty()) {
                batch.push_back(UiEvent::token(token));
            }
            const std::string &raw = agent.rawLlOutput();
            if (raw.size() > rawSeen) {
                batch.push_back(UiEvent::token(raw.substr(rawSeen)));
                rawSeen = raw.size();
            }
            collectProtocolChanges(batch, agent.protocolEvents(),
                                   previousEvents);
            if (!batch.empty())
                bridge.publishMany(std::move(batch));
        };
        std::string result =
            agent.prompt(prompt, onToken, sessionId, ephemeral);
        onToken("", true);
        UiEvent end;
        end.kind = UiEventKind::TurnDone;
        // Normalize abort leftovers if prompt returned a curl-ish cancel
        // string.
        if (!g_running ||
            result.find("Operation was aborted") != std::string::npos ||
            result.find("ABORTED_BY_CALLBACK") != std::string::npos ||
            result.find("cancelled") != std::string::npos)
            end.text = "[cancelled]";
        else
            end.text = result;
        bridge.publish(std::move(end));
    } catch (const std::exception &e) {
        const std::string msg = e.what() ? e.what() : "";
        const bool isCancel =
            !g_running || msg == "cancelled" ||
            msg.find("cancelled") != std::string::npos ||
            msg.find("Operation was aborted") != std::string::npos ||
            msg.find("ABORTED_BY_CALLBACK") != std::string::npos;
        if (isCancel) {
            UiEvent end;
            end.kind = UiEventKind::TurnDone;
            end.text = "[cancelled]";
            bridge.publish(std::move(end));
        } else {
            bridge.publish(UiEvent::error(msg));
            UiEvent end;
            end.kind = UiEventKind::TurnDone;
            end.text = std::string("error: ") + msg;
            bridge.publish(std::move(end));
        }
    }
    done.store(true, std::memory_order_release);
}
} // namespace cortex::mk3::ui
