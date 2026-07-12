#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

#include "src/protocol/parser.hpp"
#include "src/ui/chat/chat_view.hpp"
#include "src/ui/model/inkcell_app_model.hpp"

using namespace cortex::mk3;
using namespace cortex::mk3::protocol;
using namespace cortex::mk3::ui;

namespace {
int failures = 0;

void budget(const std::string& name, long elapsedMs, long budgetMs) {
    bool ok = elapsedMs <= budgetMs;
    std::cout << "  " << name << "  " << elapsedMs << "ms / " << budgetMs << "ms... "
              << (ok ? "PASS" : "FAIL") << "\n";
    if (!ok) ++failures;
}

long elapsed(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count();
}

void parser_response_stream_budget() {
    constexpr size_t bodySize = 256 * 1024;
    size_t responseBytes = 0;
    Parser parser;
    parser.onEvent([&](const TokenEvent& event) {
        if (event.type == TokenEvent::RESPONSE) responseBytes += event.content.size();
    });
    std::string wire = "<response final=\"true\">" + std::string(bodySize, 'x') + "</response>";
    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < wire.size(); ++i)
        parser.feed(wire.substr(i, 1), i + 1 == wire.size());
    long ms = elapsed(start);
    if (responseBytes != bodySize) {
        std::cout << "  streamed response byte integrity... FAIL\n";
        ++failures;
    }
    budget("256KiB response / 1-byte chunks", ms, 250);
}

void parser_action_stream_budget() {
    constexpr size_t bodySize = 128 * 1024;
    size_t actionBytes = 0;
    Parser parser([&](const ParsedAction& action) {
        actionBytes = action.content.size();
        return Json::Value(Json::objectValue);
    });
    std::string wire = "<action type=\"agent\" name=\"reader\" id=\"a1\">" +
                       std::string(bodySize, 'x') + "</action>";
    auto start = std::chrono::steady_clock::now();
    for (char ch : wire) parser.feed(std::string(1, ch));
    long ms = elapsed(start);
    if (actionBytes != bodySize) {
        std::cout << "  streamed action byte integrity... FAIL\n";
        ++failures;
    }
    budget("128KiB action / 1-byte chunks", ms, 300);
}

void cached_transcript_render_budget() {
    inkcell::Surface surface({120, 34});
    std::vector<std::string> transcript;
    transcript.reserve(3000);
    for (int i = 0; i < 1000; ++i) {
        transcript.push_back(i % 2 == 0 ? "  CORTEX" : "  TOOL  read  #r");
        transcript.push_back("    representative wrapped transcript body " + std::to_string(i));
        transcript.push_back("");
    }
    chat::TranscriptWrapCache cache;
    chat::ChatSurfaceModel model;
    model.transcriptSource = &transcript;
    model.transcriptCache = &cache;
    model.transcriptVersion = 1;
    chat::drawTranscript(surface, {2, 2, 116, 28}, model);  // warm cache
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i)
        chat::drawTranscript(surface, {2, 2, 116, 28}, model);
    budget("3000-line cached transcript / 200 frames", elapsed(start), 150);
}

void reducer_batch_budget() {
    AgentBridge bridge;
    ShellModel model;
    std::string response;
    for (int i = 0; i < 1000; ++i) {
        response.append(64, 'x');
        ProtocolEvent event;
        event.kind = ProtocolEventKind::RESPONSE;
        event.text = response;
        bridge.publish(UiEvent::protocolEvent(std::move(event), 0));
    }
    uint64_t rebuildsBefore = model.viewRebuildCount;
    auto start = std::chrono::steady_clock::now();
    model.drain(bridge);
    long ms = elapsed(start);
    bool valid = model.rootRows.size() == 1 && model.rootRows.front().body.size() == response.size();
    std::cout << "  reducer indexed response integrity... " << (valid ? "PASS" : "FAIL") << "\n";
    if (!valid) ++failures;
    bool singleRebuild = model.viewRebuildCount == rebuildsBefore + 1;
    std::cout << "  reducer performs one view rebuild per drained batch... "
              << (singleRebuild ? "PASS" : "FAIL") << "\n";
    if (!singleRebuild) ++failures;
    bool boundedSnapshot = bridge.snapshot().events.empty();
    std::cout << "  bridge excludes streaming events from snapshot retention... "
              << (boundedSnapshot ? "PASS" : "FAIL") << "\n";
    if (!boundedSnapshot) ++failures;
    budget("1000 protocol updates / one drain", ms, 100);
}
}  // namespace

int main() {
    std::cout << "Cortex performance regression gates\n";
    parser_response_stream_budget();
    parser_action_stream_budget();
    reducer_batch_budget();
    cached_transcript_render_budget();
    std::cout << "\n" << (failures == 0 ? "all passed" : "failures: " + std::to_string(failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
