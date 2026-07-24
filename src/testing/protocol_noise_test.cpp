// Protocol noise filter — pure unit tests (no Agent / network).

#include <iostream>
#include <string>

#include "src/protocol/noise.hpp"

using namespace cortex::mk3::protocol;

static int g_fail = 0;

#define CHECK(c, m)                                                            \
    do {                                                                       \
        if (!(c)) {                                                            \
            std::cerr << "  FAIL: " << m << "\n";                              \
            ++g_fail;                                                          \
        } else {                                                               \
            std::cout << "PASS " << m << "\n";                                 \
        }                                                                      \
    } while (0)

int main() {
    std::cout << "protocol noise filter…\n";

    CHECK(isOnlyOrphanCloses("</context_feed>"), "orphan context_feed close");
    CHECK(isOnlyOrphanCloses("  </response>\n</thought>  "),
          "multi orphan closes + ws");
    CHECK(isOnlyOrphanCloses("</result>"), "orphan result close");
    CHECK(!isOnlyOrphanCloses("</context_feed> hello"),
          "close + prose is not only-closes");

    CHECK(isProtocolEchoBlob("<result id=\"init\" status=\"ok\" ms=\"1\" "
                             "bytes=\"0\">no result</result>"),
          "injected result blob");
    CHECK(isProtocolEchoBlob(
              "status=\"salvage\">The previous turn's output was discarded. "
              "Your new output is the only output that will be processed."),
          "salvage banner");
    CHECK(isThoughtNoise("</response>"), "thought noise: orphan response close");
    CHECK(isThoughtNoise("</context_feed>"),
          "thought noise: orphan context_feed close");

    auto s1 = stripProtocolNoise("</context_feed>\nreal plan here");
    CHECK(s1 == "real plan here", "peel front close leaves prose");

    auto s2 = stripProtocolNoise("real plan here\n</response>");
    CHECK(s2 == "real plan here", "peel back close leaves prose");

    auto s3 = stripProtocolNoise(
        "<result id=\"salvage\" status=\"salvage\">The previous turn's output "
        "was discarded. Your new output is the only output that will be "
        "processed.</result>");
    CHECK(s3.empty(), "full result salvage blob stripped");

    auto s4 = stripProtocolNoise("Map the src/ directory structure");
    CHECK(s4 == "Map the src/ directory structure", "clean prose kept");

    auto s5 = stripProtocolNoise("  \n  ");
    CHECK(s5.empty(), "whitespace-only stripped");

    std::cout << (g_fail ? "\nFAIL\n" : "\nok\n");
    return g_fail ? 1 : 0;
}
