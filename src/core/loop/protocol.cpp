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
    auto dropThoughtAt = [&]() {
        protocol_.mutate([&](std::vector<ProtocolEvent>& ev) {
            if (st.thoughtEventIdx != static_cast<size_t>(-1) &&
                st.thoughtEventIdx < ev.size() &&
                ev[st.thoughtEventIdx].kind == ProtocolEventKind::THOUGHT)
                ev.erase(ev.begin() +
                         static_cast<std::ptrdiff_t>(st.thoughtEventIdx));
        });
        st.thoughtEventIdx = static_cast<size_t>(-1);
    };
    if (protocol::looksLikeSymbolDump(st.thoughtRawBuf) ||
        protocol::isThoughtNoise(st.thoughtRawBuf)) {
        dropThoughtAt();
        st.thoughtDroppedAsNoise = true;
        st.thoughtRawBuf.clear();
        thoughtOutput_.clear();
        return;
    }

    std::string cleaned = protocol::stripProtocolNoise(st.thoughtRawBuf);
    if (cleaned.empty()) {
        dropThoughtAt();
        return;
    }
    constexpr size_t kThoughtPubCap = 64 * 1024;
    if (cleaned.size() > kThoughtPubCap)
        cleaned = cleaned.substr(0, kThoughtPubCap - 48) +
                  "\n…[thought UI safety; full stream in dump raw/iterations]";
    thoughtOutput_ = cleaned;
    protocol_.mutate([&](std::vector<ProtocolEvent>& ev) {
        size_t idx = static_cast<size_t>(-1);
        if (st.thoughtEventIdx != static_cast<size_t>(-1) &&
            st.thoughtEventIdx < ev.size() &&
            ev[st.thoughtEventIdx].kind == ProtocolEventKind::THOUGHT)
            idx = st.thoughtEventIdx;
        else {
            for (size_t i = ev.size(); i-- > st.runEpochStart;) {
                if (ev[i].kind == ProtocolEventKind::ACTION) {
                    idx = static_cast<size_t>(-1);
                    break;
                }
                if (ev[i].kind == ProtocolEventKind::THOUGHT) {
                    idx = i;
                    break;
                }
            }
        }
        if (idx != static_cast<size_t>(-1)) {
            std::string& dst = ev[idx].text;
            if (cleaned.find(dst) != std::string::npos)
                dst = cleaned;
            else if (dst.find(cleaned) == std::string::npos) {
                dst += "\n\n---\n\n";
                dst += cleaned;
            }
            st.thoughtEventIdx = idx;
        } else {
            st.thoughtEventIdx = ev.size();
            ev.push_back({ProtocolEventKind::THOUGHT, cleaned, {}, {}});
        }
    });
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
        protocol_.mutate([&](std::vector<ProtocolEvent>& evs) {
            for (size_t i = evs.size(); i > st.runEpochStart;) {
                --i;
                if (evs[i].kind == ProtocolEventKind::RESPONSE) {
                    evs[i].text += paint;
                    return;
                }
            }
            evs.push_back({ProtocolEventKind::RESPONSE, paint, {}, {}});
        });
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
        protocol_.mutate([&](std::vector<ProtocolEvent>& evs) {
            for (auto it = evs.rbegin(); it != evs.rend(); ++it) {
                if (it->kind == ProtocolEventKind::ACTION &&
                    it->action.id == protocolAction.id) {
                    it->action = protocolAction;
                    merged = true;
                    break;
                }
            }
            if (!merged)
                evs.push_back({ProtocolEventKind::ACTION, "", protocolAction, {}});
        });
        if (!merged) {
            protocolActions_.push_back(protocolAction);
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


}  // namespace cortex::mk3
