#pragma once
// Headless one-shot turn for --ephemeral -p.
//
// --ephemeral must NOT launch the inkcell TUI: it streams the rendered
// protocol blocks (thought / action / result / response) to stdout as they
// render, like a plain chat CLI. --raw streams the raw model token stream
// verbatim instead. --no-ansi strips escape sequences from both.
//
// Consumed by voice pipelines: `-m voice run -p "<stt>" --ephemeral
// --no-ansi` → the final [response] block is TTS input.

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "src/core/agent.hpp"
#include "src/protocol/events.hpp"
#include "src/ui/bridge/ui_event.hpp"
#include "src/ui/model/protocol_event_diff.hpp"

namespace cortex::mk3::ui {

namespace {

// One rendered block as plain text. STATUS/RETRY are runtime noise — never
// rendered. Streamable kinds (thought/response) grow by append; discrete
// kinds (action/result) serialize fully.
inline std::string renderBlock(const ProtocolEvent &ev) {
    switch (ev.kind) {
        case ProtocolEventKind::THOUGHT:
            return "[thought]\n" + ev.text;
        case ProtocolEventKind::RESPONSE:
            return "[response]\n" + ev.text;
        case ProtocolEventKind::ACTION:
            return "[action " + ev.action.type + " " + ev.action.name + "]\n" +
                   ev.action.body;
        case ProtocolEventKind::RESULT:
            return "[result " + std::string(ev.result.ok ? "ok" : "fail") + "]\n" +
                   ev.result.summary;
        default:
            return "";  // STATUS / RETRY — not conversation.
    }
}

inline std::string kindColor(ProtocolEventKind k) {
    switch (k) {
        case ProtocolEventKind::RESPONSE: return "\033[38;2;130;185;145m";
        case ProtocolEventKind::ACTION:   return "\033[38;2;210;175;110m";
        case ProtocolEventKind::RESULT:   return "\033[38;2;120;175;190m";
        default:                          return "\033[38;2;110;110;118m";
    }
}

// Colorize the header line only; body stays plain.
inline std::string colorizeHeader(const std::string &block, ProtocolEventKind k) {
    const size_t nl = block.find('\n');
    if (nl == std::string::npos)
        return kindColor(k) + block + "\033[0m";
    return kindColor(k) + block.substr(0, nl) + "\033[0m" + block.substr(nl);
}

}  // namespace

// Runs one headless turn. `raw` streams the raw LLM token stream; otherwise
// rendered protocol blocks stream to stdout. `noAnsi` strips colors.
inline int runHeadlessOneShot(Agent &agent, const std::string &prompt, bool raw,
                              bool noAnsi) {
    std::vector<ProtocolEvent> previous;
    std::vector<size_t> emitted;  // chars printed per block index (delta stream)
    size_t rawSeen = 0;
    bool firstBlock = true;
    bool lastWasNewline = false;

    auto emit = [&](const std::string &s) {
        if (s.empty())
            return;
        std::cout << s;
        lastWasNewline = s.back() == '\n';
    };

    auto onToken = [&](const std::string &token, bool /*finalChunk*/) {
        if (raw) {
            // Forwarded child bytes stream directly; parent bytes land in
            // rawLlOutput_ and are read as a delta (same contract as the TUI).
            if (!token.empty())
                emit(token);
            const std::string &rawOut = agent.rawLlOutput();
            if (rawOut.size() > rawSeen) {
                emit(rawOut.substr(rawSeen));
                rawSeen = rawOut.size();
            }
            std::cout.flush();
            return;
        }
        std::vector<UiEvent> changes;
        collectProtocolChanges(changes, agent.protocolEvents(), previous);
        if (emitted.size() > previous.size())
            emitted.resize(previous.size());  // rotation / truncation
        for (const UiEvent &e : changes) {
            if (e.kind != UiEventKind::Protocol)
                continue;
            const ProtocolEvent &ev = e.protocol;
            const size_t i = e.protocolIndex;
            const std::string s = renderBlock(ev);
            if (s.empty())
                continue;
            if (i >= emitted.size())
                emitted.resize(i + 1, 0);
            size_t &em = emitted[i];
            if (s.size() <= em)
                continue;  // unchanged (or replaced) — no output
            if (em == 0) {
                // New block: separate from the previous one, colorize header.
                if (!firstBlock && !lastWasNewline)
                    emit("\n");
                emit(noAnsi ? s : colorizeHeader(s, ev.kind));
                emit("\n");
                firstBlock = false;
            } else {
                // Streaming delta of a growing block — no injected newlines.
                emit(s.substr(em));
            }
            em = s.size();
            std::cout.flush();
        }
    };

    agent.prompt(prompt, onToken, /*sessionId=*/"", /*ephemeral=*/true);
    if (!lastWasNewline)
        emit("\n");
    std::cout.flush();
    return 0;
}

}  // namespace cortex::mk3::ui
