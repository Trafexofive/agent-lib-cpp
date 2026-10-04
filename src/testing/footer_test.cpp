// Footer honesty — a frozen "thinking" must read as stalled, not alive.
// Dump 1787600154859: model went silent mid-thought; footer said "thinking"
// with a climbing clock. Lock in: silence suffix + stalled + name dedupe.
#include <iostream>
#include <string>

#include "inkcell/surface.hpp"
#include "src/ui/chat/chat_footer.hpp"

using namespace cortex::mk3::ui;
using namespace cortex::mk3::ui::chat;

namespace {
int failures = 0;
void check(bool cond, const std::string& name) {
    if (cond) std::cout << "  " << name << "... PASS\n";
    else {
        std::cout << "  " << name << "... FAIL\n";
        ++failures;
    }
}
std::string rowText(const inkcell::Surface& s, int y) {
    std::string out;
    for (int x = 0; x < s.width(); ++x) out += s.at({x, y}).glyph;
    return out;
}
bool containsRow(const inkcell::Surface& s, const std::string& needle) {
    for (int y = 0; y < s.height(); ++y)
        if (rowText(s, y).find(needle) != std::string::npos) return true;
    return false;
}

ChatFooterModel base() {
    ChatFooterModel f;
    f.agentName = "discovery";
    f.provider = "opencode-go";
    f.model = "deepseek-v4-flash";
    f.sessionId = "agent-lib-cpp-1787600154859";
    f.path = ".";
    f.manifestStem = "discovery";
    f.ctxMaxTokens = 128000;
    f.ctxUsedTokens = 5600;
    f.running = true;
    f.turnElapsedMs = 55700;
    f.iterCurrent = 2;
    f.iterMax = 400;
    f.historyUsed = 4;
    f.historyMax = 40;
    f.tokenBytes = 6000;
    f.stallTimeoutSec = 45;
    return f;
}

void render(ChatFooterModel f, inkcell::Surface& s) {
    s.clear(theme::base_bg());
    chat::drawChatFooter(s, {0, 0, s.width(), 5}, f);
}
}  // namespace

int main() {
    std::cout << "footer_test\n";
    inkcell::Surface s({120, 5});

    // Live think, fresh tokens (silent 3s) → normal "thinking", no suffix.
    {
        auto f = base();
        f.phaseKey = "think";
        f.silentSec = 3;
        render(f, s);
        check(containsRow(s, "thinking"), "think + 3s silent → thinking");
        check(!containsRow(s, "stalled"), "3s silent is not stalled");
        check(!containsRow(s, "silent"), "no silence suffix under 8s");
    }

    // Mid-thought 12s silence → warn with count.
    {
        auto f = base();
        f.phaseKey = "think";
        f.silentSec = 12;
        render(f, s);
        check(containsRow(s, "thinking") && containsRow(s, "12s silent"),
              "think + 12s silent → 'thinking · 12s silent'");
    }

    // Past the stall cutoff → "stalled · Ns silent", not "thinking".
    {
        auto f = base();
        f.phaseKey = "think";
        f.silentSec = 50;
        render(f, s);
        check(containsRow(s, "stalled · 50s silent"),
              "50s silent → 'stalled · 50s silent'");
        check(!containsRow(s, "thinking"), "stalled is not thinking");
    }

    // Tools open (pendingOps > 0) — never claim silence; the tool is working.
    {
        auto f = base();
        f.phaseKey = "act";
        f.phaseDetail = "grep";
        f.pendingOps = 1;
        f.silentSec = 60;  // tool has been running 60s — not a model stall
        render(f, s);
        check(!containsRow(s, "stalled"), "open tool is not a stall");
    }

    // Name dedupe: manifestStem == agentName → no "discovery · discovery".
    {
        auto f = base();
        f.running = false;
        f.silentSec = 0;
        f.turnElapsedMs = 0;
        render(f, s);
        std::string row4 = rowText(s, 4);
        check(row4.find("discovery · discovery") == std::string::npos,
              "no 'discovery · discovery' in place row");
    }

    std::cout << "  " << (failures == 0 ? "all passed" : "some failed") << "\n";
    return failures == 0 ? 0 : 1;
}
