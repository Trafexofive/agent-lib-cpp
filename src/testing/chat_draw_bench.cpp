// Chat draw + ctx-meter perf bench — the "why is framerate low" measurer.
// Measures the per-frame costs the live loop pays while a turn streams:
//   S1  compaction::estimateTokens(history) — footer ctx meter (per frame)
//   S1b cached-path validate (sum of sizes)  — the fix's per-frame cost
//   S2  drawChatSurface idle (no version bump)
//   S3  drawChatSurface streaming (tail mutate + version bump per frame)
// Dev tool, not a gate. Numbers printed, exit 0.
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "inkcell/surface.hpp"
#include "src/core/compaction.hpp"
#include "src/ui/chat/chat_footer.hpp"
#include "src/ui/chat/chat_view.hpp"

using namespace cortex::mk3;
using namespace cortex::mk3::ui;
using namespace cortex::mk3::ui::chat;
using clk = std::chrono::steady_clock;

static double msSince(clk::time_point t0) {
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

static void benchCtxMeter() {
    std::vector<std::string> hist;
    for (int i = 0; i < 380; ++i) hist.push_back("User: short turn " + std::to_string(i));
    for (int i = 0; i < 12; ++i)
        hist.push_back(std::string(200 * 1024, 'x'));  // full-SoT result bodies
    size_t bytes = 0;
    for (auto& h : hist) bytes += h.size();

    const int N = 100;
    auto t0 = clk::now();
    size_t acc = 0;
    for (int i = 0; i < N; ++i) acc += compaction::estimateTokens(hist);
    std::printf("S1  estimateTokens over %.1fMB history: %.3f ms/call  (~%.0f tok)\n",
                bytes / 1048576.0, msSince(t0) / N, double(acc) / N);

    t0 = clk::now();
    size_t c = 0;
    for (int i = 0; i < N * 30; ++i) {
        size_t s = 0;
        for (auto& h : hist) s += h.size();
        c += s;
    }
    std::printf("S1b cached-validate (sum sizes, %zu entries): %.6f ms/call  (%zu)\n",
                hist.size(), msSince(t0) / (N * 30), c);
}

static void benchDraw() {
    const int LINES = 6000;
    std::vector<std::string> lines;
    lines.reserve(LINES + 64);
    for (int i = 0; i < LINES; ++i)
        lines.push_back("  │ transcript content line " + std::to_string(i) +
                        " with enough prose that narrow terminals wrap it sometimes");

    TranscriptWrapCache cache;
    ChatSurfaceModel m;
    m.transcriptSource = &lines;
    m.transcriptCache = &cache;
    m.transcriptVersion = 1;
    m.followBottom = true;
    m.running = true;
    m.status = "agent running";
    m.agentName = "discovery";
    m.tokenBytes = 6000;
    m.input = "hello";
    m.inputCursor = 5;

    ChatFooterModel foot;
    foot.running = true;
    foot.agentName = "discovery";
    foot.provider = "opencode-go";
    foot.model = "deepseek-v4-flash";
    foot.ctxMaxTokens = 128000;
    foot.ctxUsedTokens = 5600;
    foot.iterCurrent = 2;
    foot.iterMax = 400;
    foot.historyUsed = 4;
    foot.historyMax = 40;
    foot.tokenBytes = 6000;
    foot.turnElapsedMs = 55000;

    inkcell::Surface s({120, 40});
    inkcell::Rect frame{0, 0, 120, 40};

    for (int i = 0; i < 10; ++i) drawChatSurface(s, frame, m, &foot);
    const int N = 200;
    auto t0 = clk::now();
    for (int i = 0; i < N; ++i) drawChatSurface(s, frame, m, &foot);
    std::printf("S2  idle draw (no version bump): %.3f ms/frame\n", msSince(t0) / N);

    t0 = clk::now();
    for (int i = 0; i < N; ++i) {
        lines.back() = "  │ streaming tail chunk " + std::to_string(i);
        if (i % 20 == 0) lines.push_back("  │ new line " + std::to_string(i));
        ++m.transcriptVersion;
        drawChatSurface(s, frame, m, &foot);
    }
    std::printf("S3  streaming draw (mutate+bump): %.3f ms/frame  (%d lines)\n",
                msSince(t0) / N, static_cast<int>(lines.size()));
}

int main() {
    std::printf("chat_draw_bench @ 120x40\n");
    benchCtxMeter();
    benchDraw();
    return 0;
}
