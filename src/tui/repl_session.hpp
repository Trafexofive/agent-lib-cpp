// src/tui/repl_session.hpp — Legacy MK3 REPL TUI extracted from main.cpp.
//
// This is intentionally a 1:1 port of the working legacy TUI surface:
// TuiRenderer + SessionView + StatusPromptRenderer + Input + Dialog + FrameClock.
// Do not redesign this into inkcell Scenes/TextArea until parity is locked.
#pragma once

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "src/core/agent.hpp"
#include "src/core/manifest_loader.hpp"
#include "src/session/manager.hpp"
#include "src/tui/dialog.hpp"
#include "src/tui/frame_clock.hpp"
#include "src/tui/input.hpp"
#include "src/tui/renderer.hpp"
#include "src/tui/session_view.hpp"
#include "src/tui/slash_commands.hpp"
#include "src/tui/status_prompt.hpp"
#include "src/utils/ansi.hpp"

namespace cortex::mk3::tui {

struct ReplSessionConfig {
    std::string provider;
    std::string model;
    std::string sessionId;
    std::string sessionName;
    std::string tuiDebugDumpPath;
    std::string workflowXml;
    std::vector<ToolSchema> allSchemas;
    bool ephemeral = false;
    bool toolAnsi = true;
    volatile bool* resizedFlag = nullptr;
};

struct ReplAskDialogSession {
    std::string actionId;
    Json::Value params;
    DialogState state;
    Json::Value result;
    bool active = false;
    bool complete = false;
    bool cancelled = false;
    std::mutex mutex;
    std::condition_variable cv;
};

class ReplSession {
   public:
    explicit ReplSession(ReplSessionConfig cfg) : cfg_(std::move(cfg)), renderer_(80) {
        renderer_.setToolAnsiPassthrough(cfg_.toolAnsi);
    }

    int run(Agent& agent) {
        configureDebugDumpFromEnv();
        enterAltScreen();
        initTerminalSize();
        renderer_.setWidth(termW_);

        Input input;
        std::string cmd;
        bool quit = false;
        auto askDialog = std::make_shared<ReplAskDialogSession>();
        std::atomic<bool> dialogActive{false};
        std::string dialogInputLine;
        std::vector<std::string> historyLines;
        int scrollOffset = 0;
        bool showPrompts = false;
        bool streaming = false;
        std::chrono::steady_clock::time_point streamStart;
        FrameClock frameClock;
        std::vector<std::string> tuiFrameLog;
        std::vector<std::string> tuiAnsiFrames;
        std::string streamPhase = "idle";
        size_t streamActionCount = 0;
        size_t streamResultCount = 0;
        size_t streamRespBytes = 0;
        std::string streamThoughtPreview;
        std::atomic<bool> agentDone{false};
        std::mutex streamMtx;
        std::vector<ProtocolEvent> snapEvents;
        std::string snapResponse;
        size_t snapRawBytes = 0;
        size_t snapActionCount = 0;
        size_t snapResultCount = 0;
        std::string snapPhase = "waiting provider";
        bool snapDirty = false;
        bool snapClearRenderer = false;
        bool firstToken = true;

        std::vector<ProtocolEvent> uiEvents;
        std::string uiResponse;
        std::vector<std::string> cachedRendererLines;
        bool transcriptDirty = true;
        int cachedRendererWidth = -1;
        size_t uiTimelineSignature = 0;

        auto statusState = [&]() {
            StatusBarState state;
            state.dialogActive = dialogActive.load(std::memory_order_acquire);
            state.streaming = streaming;
            state.spinnerFrame = frameClock.spinnerFrame();
            state.streamStart = streamStart;
            state.phase = streamPhase;
            state.actionCount = streamActionCount;
            state.resultCount = streamResultCount;
            state.responseBytes = streamRespBytes;
            state.mode = renderer_.mode();
            state.provider = cfg_.provider;
            state.model = cfg_.model;
            state.thoughtPreview = streamThoughtPreview;
            state.sessionId = cfg_.sessionId;
            state.sessionName = cfg_.sessionName;
            return state;
        };
        auto statusBarText = [&](int displaySize) -> std::string {
            (void)displaySize;
            return StatusPromptRenderer::statusBar(statusState());
        };
        auto promptLineText = [&]() -> std::string {
            return StatusPromptRenderer::promptLine(input,
                                                     dialogActive.load(std::memory_order_acquire));
        };
        auto captureAnsiFrame = [&](const std::vector<std::string>& visible, int startRow,
                                    int visibleCount, int displaySize) {
            std::vector<std::string> frame(static_cast<size_t>(termH_));
            for (int i = 0; i < visibleCount && i < static_cast<int>(visible.size()); i++) {
                int row = startRow + i - 1;
                if (row >= 0 && row < termH_)
                    frame[static_cast<size_t>(row)] = visible[static_cast<size_t>(i)];
            }
            if (termH_ >= 2) {
                frame[static_cast<size_t>(termH_ - 2)] = statusBarText(displaySize);
                frame[static_cast<size_t>(termH_ - 1)] = promptLineText();
            }
            std::ostringstream ss;
            for (const auto& line : frame)
                ss << line << "\n";
            tuiAnsiFrames.push_back(ss.str());
            while (tuiAnsiFrames.size() > 20)
                tuiAnsiFrames.erase(tuiAnsiFrames.begin());
        };

        SessionView sessionView(termW_, termH_);
        auto renderScreen = [&](bool bypassPacing = false, bool fullRedraw = false) {
            if (!frameClock.shouldRender(streaming, bypassPacing))
                return;

            std::vector<std::string> dynamicRendererLines;
            std::vector<std::string> dialogLines;
            const std::vector<std::string>* rendererLines = &cachedRendererLines;
            bool showDialog = dialogActive.load(std::memory_order_acquire) && !askDialog->state.done();
            if (showDialog) {
                dialogLines = DialogRenderer::render(askDialog->state, termW_, input.line());
            } else if (showPrompts) {
                auto& prompts = agent.iterationPrompts();
                if (prompts.empty()) {
                    dynamicRendererLines.push_back(
                        "\033[2m(no prompts captured — run a prompt first)\033[0m");
                } else {
                    for (size_t i = 0; i < prompts.size(); i++) {
                        dynamicRendererLines.push_back(std::string("\033[1m") +
                                                       ansi::fg(200, 200, 100) + "─── Iter " +
                                                       std::to_string(i + 1) + " ───\033[0m");
                        std::istringstream ps(prompts[i]);
                        std::string pline;
                        while (std::getline(ps, pline))
                            dynamicRendererLines.push_back(std::string("\033[2m") + pline + "\033[0m");
                        dynamicRendererLines.push_back("");
                    }
                }
                rendererLines = &dynamicRendererLines;
            } else {
                if (transcriptDirty || cachedRendererWidth != termW_) {
                    cachedRendererLines = renderer_.renderTranscript(uiEvents, uiResponse, termW_);
                    cachedRendererWidth = termW_;
                    transcriptDirty = false;
                }
            }

            SessionViewport vp =
                sessionView.build(historyLines, *rendererLines, dialogLines, showDialog, scrollOffset);
            if (!cfg_.tuiDebugDumpPath.empty())
                captureAnsiFrame(vp.visible, vp.startRow, vp.visibleCount, vp.displaySize);

            std::cout << sessionView.render(vp, statusBarText, promptLineText(), fullRedraw)
                      << std::flush;
            frameClock.didRender(streaming);
        };

        input.start([&](const std::string& s) {
            if (dialogActive.load(std::memory_order_acquire))
                dialogInputLine = s;
            else
                cmd = s;
        });

        const char* home = getenv("HOME");
        std::string histPath = home ? std::string(home) + "/.mk3_history" : "/tmp/.mk3_history";
        input.history().load(histPath);
        input.setCompleter([](const std::string& prefix) { return SlashCommands::complete(prefix); });
        input.scrollUp = [&] {
            scrollOffset += std::max(1, (termH_ - 2) / 2);
            frameClock.requestFrame();
            renderScreen(true);
        };
        input.scrollDown = [&] {
            scrollOffset -= std::max(1, (termH_ - 2) / 2);
            frameClock.requestFrame();
            renderScreen(true);
        };
        input.clearScreen = [&] {
            std::cout << "\033[2J\033[H" << std::flush;
            frameClock.requestFrame();
            renderScreen(true, true);
        };

        auto pushTuiLine = [&](const std::string& line) {
            historyLines.push_back(std::string("\033[2m\033[3m") + line + cortex::mk3::ansi::reset);
            frameClock.requestFrame();
        };
        auto pushTuiSection = [&](const std::string& title, const std::vector<std::string>& items) {
            pushTuiLine("[" + title + "] " + (items.empty() ? "none" : std::to_string(items.size())));
            for (const auto& item : items)
                pushTuiLine("  - " + item);
        };
        auto dumpTuiState = [&](const std::string& path, const std::string& reason,
                                bool notify) -> bool {
            if (path.empty())
                return false;
            std::ofstream f(path);
            std::vector<ProtocolEvent> curEvents;
            std::string curResponse;
            {
                std::lock_guard<std::mutex> lk(streamMtx);
                curEvents = snapEvents;
                curResponse = snapResponse;
            }
            auto lines = renderer_.renderTranscript(curEvents, curResponse, termW_);
            if (!f) {
                if (notify)
                    pushTuiLine("Failed to write " + path);
                return false;
            }
            const auto& acts = agent.protocolActions();
            const auto& ress = agent.protocolResults();
            f << "# Cortex MK3 TUI debug dump\n";
            f << "reason: " << reason << "\n";
            f << "mode: " << TuiRenderer::modeName(renderer_.mode()) << "\n";
            f << "term: " << termW_ << "x" << termH_ << "\n";
            f << "streaming: " << (streaming ? "true" : "false") << "\n";
            f << "history_lines: " << historyLines.size() << "\n";
            f << "render_lines: " << lines.size() << "\n";
            f << "protocol_actions: " << acts.size() << "\n";
            f << "protocol_results: " << ress.size() << "\n";
            f << "frame_log_lines: " << tuiFrameLog.size() << "\n";
            f << "ansi_snapshot_count: " << tuiAnsiFrames.size() << "\n\n";
            f << "## Frame log\n";
            for (const auto& l : tuiFrameLog)
                f << l << "\n";
            f << "\n## Raw ANSI snapshots\n";
            for (size_t i = 0; i < tuiAnsiFrames.size(); i++) {
                f << "--- ansi frame " << i << " ---\n";
                f << tuiAnsiFrames[i];
            }
            f << "\n## Protocol events\n";
            for (const auto& a : acts) {
                f << "ACTION " << a.type << " " << a.name << "#" << a.id
                  << " sync=" << (a.sync ? "true" : "false") << "\n";
                if (!a.body.empty())
                    f << "  body: " << a.body.substr(0, 1200)
                      << (a.body.size() > 1200 ? "..." : "") << "\n";
            }
            for (const auto& r : ress) {
                f << "RESULT " << r.id << " ok=" << (r.ok ? "true" : "false") << " ms=" << r.elapsedMs
                  << " bytes=" << r.outputBytes << "\n";
                if (!r.summary.empty())
                    f << "  summary: " << r.summary.substr(0, 1200)
                      << (r.summary.size() > 1200 ? "..." : "") << "\n";
            }
            f << "\n## History\n";
            for (const auto& l : historyLines)
                f << l << "\n";
            f << "\n## Current Renderer\n";
            for (const auto& l : lines)
                f << l << "\n";
            if (notify) {
                pushTuiLine("Wrote " + path + " (reason " + reason + ", history " +
                            std::to_string(historyLines.size()) + ", current " +
                            std::to_string(lines.size()) + ", actions " + std::to_string(acts.size()) +
                            ", results " + std::to_string(ress.size()) + ")");
            }
            return true;
        };
        auto workflowNamesFromXml = [&]() {
            std::vector<std::string> names;
            size_t pos = 0;
            while ((pos = cfg_.workflowXml.find("<workflow", pos)) != std::string::npos) {
                size_t namePos = cfg_.workflowXml.find("name=\"", pos);
                if (namePos == std::string::npos) {
                    pos += 9;
                    continue;
                }
                namePos += 6;
                size_t end = cfg_.workflowXml.find('"', namePos);
                if (end == std::string::npos) {
                    pos += 9;
                    continue;
                }
                names.push_back(cfg_.workflowXml.substr(namePos, end - namePos));
                pos = end + 1;
            }
            return names;
        };
        auto showManifests = [&]() {
            std::vector<std::string> tools;
            for (const auto& s : cfg_.allSchemas)
                tools.push_back(s.name + (s.description.empty() ? "" : " — " + s.description));
            pushTuiLine("─── Active Manifest Surface ───");
            pushTuiLine("agent: " + agent.name() + "  provider: " + agent.config().provider +
                        "  model: " + agent.config().model);
            pushTuiSection("tools", tools);
            pushTuiSection("feeds", agent.feedNames());
            pushTuiSection("relics", agent.relicNames());
            pushTuiSection("agents", agent.subAgentNames());
            pushTuiSection("workflows", workflowNamesFromXml());
        };

        replayRenderedHistory(historyLines);
        renderScreen();

        auto timelineSignature = [](const std::vector<ProtocolEvent>& events) -> size_t {
            size_t h = 1469598103934665603ull;
            auto mix = [&](size_t v) {
                h ^= v;
                h *= 1099511628211ull;
            };
            for (const auto& ev : events) {
                mix(static_cast<size_t>(ev.kind));
                mix(ev.text.size());
                mix(ev.action.type.size());
                mix(ev.action.name.size());
                mix(ev.action.id.size());
                mix(ev.action.body.size());
                mix(ev.result.id.size());
                mix(ev.result.summary.size());
                mix(ev.result.toolName.size());
                mix(static_cast<size_t>(ev.result.ok));
                mix(static_cast<size_t>(ev.result.outputBytes));
            }
            return h;
        };

        auto applyStreamSnapshot = [&]() -> bool {
            std::vector<ProtocolEvent> events;
            std::string response, phase;
            size_t rawBytes = 0;
            size_t actions = 0;
            size_t results = 0;
            bool clearRenderer = false;
            {
                std::lock_guard<std::mutex> lk(streamMtx);
                if (!snapDirty)
                    return false;
                events = snapEvents;
                response = snapResponse;
                phase = snapPhase;
                rawBytes = snapRawBytes;
                actions = snapActionCount;
                results = snapResultCount;
                clearRenderer = snapClearRenderer;
                snapClearRenderer = false;
                snapDirty = false;
            }

            if (clearRenderer)
                renderer_.clear();
            const size_t nextTimelineSignature = timelineSignature(events);
            const bool eventsChanged = nextTimelineSignature != uiTimelineSignature;
            const bool responseChanged = response.size() != uiResponse.size();
            uiTimelineSignature = nextTimelineSignature;
            uiEvents = std::move(events);
            uiResponse = std::move(response);
            streamActionCount = actions;
            streamResultCount = results;
            streamRespBytes = uiResponse.size();
            streamPhase = phase;
            (void)rawBytes;
            if (eventsChanged)
                frameClock.requestFrame();
            const bool changed = eventsChanged || responseChanged || clearRenderer;
            if (changed) {
                transcriptDirty = true;
                frameClock.requestFrame();
            }
            return changed;
        };

        while (cortex::mk3::g_running && !quit) {
            handleResize(sessionView, transcriptDirty, cachedRendererWidth, frameClock, renderScreen);
            cmd.clear();
            while (cmd.empty() && cortex::mk3::g_running) {
                std::string before = input.line();
                size_t beforeCp = input.cursorPos();
                input.poll();
                if (input.line() != before || input.cursorPos() != beforeCp) {
                    frameClock.requestFrame();
                    renderScreen(true, false);
                }
            }
            if (!cortex::mk3::g_running || cmd.empty())
                continue;

            if (cmd == "/quit" || cmd == "/exit") {
                quit = true;
                break;
            }
            if (cmd[0] == '/') {
                handleSlashCommand(cmd, agent, historyLines, showPrompts, streamMtx, snapEvents,
                                   snapResponse, dumpTuiState, showManifests, renderScreen,
                                   pushTuiLine, termW_);
                continue;
            }

            std::string promptText = cmd;
            cmd.clear();
            cortex::mk3::g_running = true;
            streaming = true;
            agentDone.store(false, std::memory_order_release);
            firstToken = true;
            {
                std::lock_guard<std::mutex> lk(streamMtx);
                snapEvents.clear();
                snapResponse.clear();
                snapRawBytes = 0;
                snapActionCount = 0;
                snapResultCount = 0;
                snapPhase = "waiting provider";
                snapDirty = false;
                snapClearRenderer = false;
            }
            streamPhase = "waiting provider";
            streamActionCount = 0;
            streamResultCount = 0;
            streamRespBytes = 0;
            streamThoughtPreview.clear();
            uiEvents.clear();
            uiResponse.clear();
            cachedRendererLines.clear();
            transcriptDirty = true;
            cachedRendererWidth = -1;
            uiTimelineSignature = 0;
            streamStart = std::chrono::steady_clock::now();
            input.clearEscape();

            {
                const std::string promptBg = "\033[48;2;45;45;50m";
                historyLines.push_back(padRight(promptBg, termW_));
                historyLines.push_back(padRight(
                    promptBg + " " + std::string(cortex::mk3::ansi::bold) + "▸ " + promptText + " ", termW_));
                historyLines.push_back(padRight(promptBg, termW_));
            }
            scrollOffset = 0;
            frameClock.requestFrame();
            renderScreen(true, false);

            std::mutex askMtx;
            std::condition_variable askCv;
            Json::Value askParams;
            bool askParamsReady = false;
            std::atomic<bool> askPending{false};

            agent.setAskToolHandler([&](const Json::Value& params) -> Json::Value {
                {
                    std::lock_guard<std::mutex> lk(askMtx);
                    askParams = params;
                    askParamsReady = true;
                }
                askPending.store(true, std::memory_order_release);
                askCv.notify_one();

                std::unique_lock<std::mutex> lk(askMtx);
                askCv.wait(lk, [&] {
                    return askDialog->complete || askDialog->cancelled || !cortex::mk3::g_running;
                });
                Json::Value out;
                if (askDialog->cancelled) {
                    out["success"] = false;
                    out["cancelled"] = true;
                    out["results"] = askDialog->state.results;
                } else {
                    out["success"] = true;
                    out["results"] = askDialog->state.results;
                }
                askDialog->active = false;
                askDialog->complete = false;
                askDialog->cancelled = false;
                askParamsReady = false;
                askPending.store(false, std::memory_order_release);
                dialogActive.store(false, std::memory_order_release);
                input.clearActionInterceptor();
                frameClock.requestFrame();
                return out;
            });

            std::thread agentThread([&]() {
                agent.prompt(
                    promptText,
                    [&](const std::string& /*token*/, bool) {
                        if (!cortex::mk3::g_running)
                            return;
                        const std::vector<ProtocolEvent>& events = agent.protocolEvents();
                        const std::string& response = agent.responseOutput();
                        const std::string& raw = agent.rawLlOutput();
                        const size_t actions = agent.protocolActions().size();
                        const size_t results = agent.protocolResults().size();
                        std::string phase = "waiting provider";
                        if (actions > results)
                            phase = "running tools";
                        else if (!response.empty())
                            phase = "streaming response";
                        else if (!raw.empty())
                            phase = "parsing protocol";
                        {
                            std::lock_guard<std::mutex> lk(streamMtx);
                            if (firstToken) {
                                snapClearRenderer = true;
                                firstToken = false;
                            }
                            snapEvents = events;
                            snapResponse = response;
                            snapRawBytes = raw.size();
                            snapActionCount = actions;
                            snapResultCount = results;
                            snapPhase = phase;
                            snapDirty = true;
                        }
                    },
                    cfg_.sessionId, cfg_.ephemeral);
                {
                    std::lock_guard<std::mutex> lk(streamMtx);
                    snapEvents = agent.protocolEvents();
                    snapResponse = agent.responseOutput();
                    snapRawBytes = agent.rawLlOutput().size();
                    snapActionCount = agent.protocolActions().size();
                    snapResultCount = agent.protocolResults().size();
                    snapPhase = "complete";
                    snapDirty = true;
                }
                agentDone.store(true, std::memory_order_release);
            });

            while (!agentDone.load(std::memory_order_acquire) && cortex::mk3::g_running && !quit) {
                if (!dialogActive.load(std::memory_order_acquire) &&
                    askPending.load(std::memory_order_acquire)) {
                    std::lock_guard<std::mutex> lk(askMtx);
                    if (askParamsReady) {
                        askDialog->active = true;
                        askDialog->complete = false;
                        askDialog->cancelled = false;
                        askDialog->params = askParams;
                        askDialog->state = parseDialogState(askParams);
                        completeNonInteractiveCards(askDialog->state);
                        if (askDialog->state.done()) {
                            askDialog->complete = true;
                            askDialog->result = askDialog->state.results;
                            askCv.notify_one();
                        } else {
                            dialogActive.store(true, std::memory_order_release);
                            input.clearEscape();
                            frameClock.requestFrame();
                            input.setActionInterceptor([&](int act, char outChar) -> bool {
                                return handleDialogActionInterceptor(act, outChar, *askDialog,
                                                                     dialogActive, input,
                                                                     frameClock, askCv);
                            });
                        }
                    }
                }

                std::string beforeInput = input.line();
                size_t beforeCursor = input.cursorPos();
                bool hadInput = input.poll();
                bool inputChanged = (input.line() != beforeInput || input.cursorPos() != beforeCursor);
                if (inputChanged)
                    frameClock.requestFrame();

                if (input.escapePressed()) {
                    if (dialogActive.load(std::memory_order_acquire)) {
                        askDialog->cancelled = true;
                        askDialog->active = false;
                        dialogActive.store(false, std::memory_order_release);
                        input.clearActionInterceptor();
                        frameClock.requestFrame();
                        input.clearEscape();
                        frameClock.requestFrame();
                        askCv.notify_one();
                    } else {
                        cortex::mk3::g_running = false;
                        input.clearEscape();
                    }
                }

                if (dialogActive.load(std::memory_order_acquire) && hadInput) {
                    std::string line = dialogInputLine;
                    dialogInputLine.clear();
                    bool done = handleDialogLine(askDialog->state, line);
                    input.clearEscape();
                    frameClock.requestFrame();
                    if (done) {
                        askDialog->complete = true;
                        askDialog->result = askDialog->state.results;
                        askDialog->active = false;
                        dialogActive.store(false, std::memory_order_release);
                        input.clearActionInterceptor();
                        frameClock.requestFrame();
                        askCv.notify_one();
                    }
                }

                if (!cmd.empty() && (cmd == "/exit" || cmd == "/quit")) {
                    cortex::mk3::g_running = false;
                    quit = true;
                }

                handleResize(sessionView, transcriptDirty, cachedRendererWidth, frameClock, renderScreen);
                applyStreamSnapshot();
                frameClock.heartbeatDue(streaming);
                renderScreen(inputChanged, false);

                if (!hadInput && !inputChanged)
                    usleep(2000);
            }

            applyStreamSnapshot();
            if (agentThread.joinable())
                agentThread.join();
            applyStreamSnapshot();
            renderScreen(true, false);

            if (!cortex::mk3::g_running) {
                streaming = false;
                renderCancelledPrompt(input);
                continue;
            }

            std::vector<ProtocolEvent> curEvents;
            std::string curResponse;
            {
                std::lock_guard<std::mutex> lk(streamMtx);
                curEvents = snapEvents;
                curResponse = snapResponse;
                snapEvents.clear();
                snapResponse.clear();
                snapRawBytes = 0;
                snapActionCount = 0;
                snapResultCount = 0;
                snapDirty = false;
            }
            uiEvents.clear();
            uiResponse.clear();
            cachedRendererLines.clear();
            transcriptDirty = true;
            cachedRendererWidth = -1;
            uiTimelineSignature = 0;
            auto turnLines = renderer_.renderTranscript(curEvents, curResponse, termW_);
            historyLines.insert(historyLines.end(), turnLines.begin(), turnLines.end());
            if (historyLines.empty())
                historyLines.push_back("");
            streaming = false;
            if (!cfg_.tuiDebugDumpPath.empty())
                dumpTuiState(cfg_.tuiDebugDumpPath, "turn-complete", false);
            renderer_.clear();
            streamPhase = "idle";
            frameClock.requestFrame();
            renderScreen();
            saveRenderedHistory(historyLines);
        }

        input.stop();
        input.history().save(histPath);
        std::cout << "\nBye.\n";
        return 0;
    }

   private:
    void configureDebugDumpFromEnv() {
        if (!cfg_.tuiDebugDumpPath.empty())
            return;
        const char* dumpEnv = getenv("MK3_TUI_DEBUG_DUMP");
        if (dumpEnv && *dumpEnv)
            cfg_.tuiDebugDumpPath = std::string(dumpEnv) == "1" ? "/tmp/mk3-tui-debug-dump.txt"
                                                                 : std::string(dumpEnv);
    }

    void enterAltScreen() const {
        std::cout << "\033[?1049h\033[?25l" << std::flush;
        atexit([] { std::cout << "\033[?1049l\033[?25h" << std::flush; });
    }

    void initTerminalSize() {
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
            termW_ = ws.ws_col;
            termH_ = ws.ws_row;
        } else {
            const char* ec = getenv("COLUMNS");
            if (ec)
                termW_ = std::stoi(ec);
            const char* er = getenv("LINES");
            if (er)
                termH_ = std::stoi(er);
        }
    }

    template <typename RenderFn>
    void handleResize(SessionView& sessionView, bool& transcriptDirty, int& cachedRendererWidth,
                      FrameClock& frameClock, RenderFn& renderScreen) {
        if (!cfg_.resizedFlag || !*cfg_.resizedFlag)
            return;
        *cfg_.resizedFlag = false;
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
            termW_ = ws.ws_col;
            termH_ = ws.ws_row;
            renderer_.setWidth(termW_);
            cachedRendererWidth = -1;
            transcriptDirty = true;
            sessionView.setWidthHeight(termW_, termH_);
            frameClock.requestFrame();
            renderScreen(true, true);
        }
    }

    void replayRenderedHistory(std::vector<std::string>& historyLines) const {
        if (cfg_.sessionId.empty())
            return;
        session::SessionManager sm;
        if (sm.exists(cfg_.sessionId)) {
            Session seeded = sm.load(cfg_.sessionId);
            for (const auto& line : seeded.renderedHistory)
                historyLines.push_back(line);
        }
    }

    void saveRenderedHistory(const std::vector<std::string>& historyLines) const {
        if (cfg_.ephemeral || cfg_.sessionId.empty())
            return;
        session::SessionManager sm;
        if (sm.exists(cfg_.sessionId)) {
            Session seed = sm.load(cfg_.sessionId);
            seed.renderedHistory = historyLines;
            sm.save(seed);
        }
    }

    void renderCancelledPrompt(Input& input) const {
        std::cout << "\033[" << std::max(1, termH_ - 1) << ";1H\033[2K" << cortex::mk3::ansi::red << "Cancelled"
                  << cortex::mk3::ansi::reset;
        std::cout << "\033[" << termH_ << ";1H\033[2K" << cortex::mk3::ansi::bold << "▸ "
                  << cortex::mk3::ansi::reset << "\033[2m\033[3m";
        size_t cp = input.cursorPos();
        std::string l = input.line();
        std::cout << l.substr(0, cp);
        std::cout << "\033[7m" << (cp < l.size() ? std::string(1, l[cp]) : " ") << "\033[27m";
        if (cp < l.size())
            std::cout << l.substr(cp + 1);
        std::cout << cortex::mk3::ansi::reset << " " << std::flush;
    }

    template <typename DumpFn, typename ShowManifestsFn, typename RenderFn, typename PushLineFn>
    void handleSlashCommand(const std::string& cmd, Agent& agent, std::vector<std::string>& historyLines,
                            bool& showPrompts, std::mutex& streamMtx,
                            std::vector<ProtocolEvent>& snapEvents, std::string& snapResponse,
                            DumpFn& dumpTuiState, ShowManifestsFn& showManifests,
                            RenderFn& renderScreen, PushLineFn& pushTuiLine, int termW) {
        if (cmd == "/help" || cmd == "/commands") {
            for (auto& l : SlashCommands::helpLines())
                pushTuiLine(l);
        } else if (cmd == "/manifests") {
            showManifests();
        } else if (cmd == "/prompts") {
            showPrompts = !showPrompts;
            historyLines.clear();
        } else if (SlashCommands::isDynamic(cmd)) {
            for (auto& l : SlashCommands::renderDynamic(cmd))
                pushTuiLine(l);
        } else if (cmd == "/cp-all") {
            std::string all;
            for (auto& l : historyLines)
                all += l + "\n";
            std::vector<ProtocolEvent> curEvents;
            std::string curResponse;
            {
                std::lock_guard<std::mutex> lk(streamMtx);
                curEvents = snapEvents;
                curResponse = snapResponse;
            }
            auto rl = renderer_.renderTranscript(curEvents, curResponse, termW);
            for (auto& l : rl)
                all += l + "\n";
            int rc = system("which wl-copy >/dev/null 2>&1 && wl-copy");
            if (rc != 0)
                rc = system("which xclip >/dev/null 2>&1 && xclip -selection clipboard");
            if (rc != 0) {
                std::ofstream f("/tmp/mk3-cp-all.txt");
                if (f)
                    f << all;
            } else {
                FILE* p = popen(rc == 0 ? "wl-copy" : "xclip -selection clipboard", "w");
                if (p) {
                    fwrite(all.c_str(), 1, all.size(), p);
                    pclose(p);
                }
            }
        } else if (cmd == "/cp-raw") {
            std::string raw = agent.rawLlOutput();
            int rc = system("which wl-copy >/dev/null 2>&1");
            FILE* p = popen(rc == 0 ? "wl-copy" : "xclip -selection clipboard", "w");
            if (p) {
                fwrite(raw.c_str(), 1, raw.size(), p);
                pclose(p);
            } else {
                std::ofstream f("/tmp/mk3-cp-raw.txt");
                if (f)
                    f << raw;
            }
        } else if (cmd == "/sessions") {
            session::SessionManager sessionMgr;
            auto list = sessionMgr.list();
            historyLines.push_back(std::string("\033[2m\033[3m") + "─── Sessions ───" + cortex::mk3::ansi::reset);
            for (auto& s : list)
                historyLines.push_back(std::string("\033[2m\033[3m") + s.id + "  " + s.updated +
                                       "  " + std::to_string(s.turnCount) + " turns" + cortex::mk3::ansi::reset);
        } else if (cmd == "/dump-render" || cmd == "/dr") {
            std::string path = cfg_.tuiDebugDumpPath.empty() ? "/tmp/mk3-render-dump.txt" : cfg_.tuiDebugDumpPath;
            dumpTuiState(path, "slash-command", true);
        } else if (cmd == "/dump-prompt" || cmd == "/dp") {
            auto& prompts = agent.iterationPrompts();
            if (prompts.empty()) {
                historyLines.push_back("\033[2m(no prompts captured — run a prompt first)\033[0m");
            } else {
                for (size_t i = 0; i < prompts.size(); i++) {
                    std::string path = "/tmp/mk3-prompt-iter" + std::to_string(i + 1) + ".xml";
                    std::ofstream f(path);
                    f << "<!-- Cortex MK3 Prompt — Iteration " << (i + 1) << " -->\n";
                    f << prompts[i];
                    historyLines.push_back(std::string("\033[2m\033[3m") + "Wrote " + path +
                                           " (" + std::to_string(prompts[i].size()) +
                                           " bytes)" + cortex::mk3::ansi::reset);
                }
            }
        }
        renderScreen();
    }

    static bool handleDialogActionInterceptor(int act, char outChar, ReplAskDialogSession& askDialog,
                                              std::atomic<bool>& dialogActive, Input& input,
                                              FrameClock& frameClock, std::condition_variable& askCv) {
        if (!dialogActive.load(std::memory_order_acquire))
            return false;
        const DialogCard* card = askDialog.state.current();
        if (!card)
            return false;

        if ((act == static_cast<int>(KeyAction::CHAR) && (outChar == 'j' || outChar == 'J')) ||
            act == static_cast<int>(KeyAction::HISTORY_DOWN)) {
            if (card->type == "choice" || card->type == "multi_choice" || card->type == "ranker") {
                if (askDialog.state.selectedOption < static_cast<int>(card->options.size()) - 1)
                    askDialog.state.selectedOption++;
                frameClock.requestFrame();
                return true;
            }
        }
        if ((act == static_cast<int>(KeyAction::CHAR) && (outChar == 'k' || outChar == 'K')) ||
            act == static_cast<int>(KeyAction::HISTORY_UP)) {
            if (card->type == "choice" || card->type == "multi_choice" || card->type == "ranker") {
                if (askDialog.state.selectedOption > 0)
                    askDialog.state.selectedOption--;
                frameClock.requestFrame();
                return true;
            }
        }
        if (card->type == "confirm" && act == static_cast<int>(KeyAction::CHAR)) {
            if (outChar == 'y' || outChar == 'Y' || outChar == 'n' || outChar == 'N') {
                advanceDialog(askDialog.state, outChar == 'y' || outChar == 'Y');
                frameClock.requestFrame();
                if (askDialog.state.done()) {
                    askDialog.complete = true;
                    askDialog.result = askDialog.state.results;
                    askDialog.active = false;
                    dialogActive.store(false, std::memory_order_release);
                    input.clearActionInterceptor();
                    frameClock.requestFrame();
                    askCv.notify_one();
                }
                return true;
            }
        }
        if (act == static_cast<int>(KeyAction::SEARCH) ||
            act == static_cast<int>(KeyAction::SCROLL_UP) ||
            act == static_cast<int>(KeyAction::SCROLL_DOWN) ||
            act == static_cast<int>(KeyAction::CLEAR_SCREEN) ||
            act == static_cast<int>(KeyAction::TAB))
            return true;
        return false;
    }

    ReplSessionConfig cfg_;
    TuiRenderer renderer_;
    int termW_ = 80;
    int termH_ = 24;
};

}  // namespace cortex::mk3::tui
