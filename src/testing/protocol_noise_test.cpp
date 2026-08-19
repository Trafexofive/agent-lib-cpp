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

    CHECK(isThoughtNoise("</context_feed>"), "orphan context_feed close");
    CHECK(isThoughtNoise("  </response>\n</thought>  "), "multi orphan closes");
    CHECK(isThoughtNoise("</result>"), "orphan result close");
    CHECK(isThoughtNoise("</context_f"), "partial context_feed close");
    CHECK(isThoughtNoise("</user"), "partial user close");

    CHECK(isThoughtNoise("<result id=\"init\" status=\"ok\" ms=\"1\" "
                         "bytes=\"0\">no result</result>"),
          "injected result blob");
    CHECK(isThoughtNoise(
              "<result id=\"salvage\" status=\"salvage\">The previous turn's "
              "output was discarded. Your new output is the only output that "
              "will be processed.</result>"),
          "salvage result blob");

    // Live dump shape: history + result + glued final= + prose + more result.
    const std::string liveDump =
        "<history>\n"
        "  <entry turn=\"1\">\n"
        "  <action type=\"agent\" name=\"discovery\" id=\"ping1\" mode=\"sync\" "
        "ephemeral=\"true\">Reply with the word \"ALIVE\".</action>\n"
        "  </entry>\n"
        "</history>\n"
        "<result id=\"ping1\" status=\"ok\" ms=\"1074.0\" bytes=\"8\">ALIVE"
        "</result>final=\"true\"Sub-agent `discovery` is alive and responding. "
        "`critic` is also available if needed."
        "<result id=\"r1\" status=\"ok\" ms=\"1202.5\" bytes=\"46\">"
        "Sub-agent `discovery` is alive and responding. `critic` is also "
        "available if needed.</result>";

    auto cleaned = stripProtocolNoise(liveDump);
    CHECK(!cleaned.empty(), "live dump keeps human prose");
    CHECK(cleaned.find("discovery") != std::string::npos,
          "live dump prose mentions discovery");
    CHECK(cleaned.find("<result") == std::string::npos, "no result tags left");
    CHECK(cleaned.find("<history") == std::string::npos, "no history tags left");
    CHECK(cleaned.find("final=") == std::string::npos, "attr debris stripped");

    // context_feed continue-banner (harness inject parrot).
    CHECK(isThoughtNoise(
              "<context_feed>\n"
              "  <user>Continue from the inline transcript above. Use runtime "
              "results only; if enough information is available, emit "
              "<response final=\"true\">.</user>\n"
              "</context_f"),
          "context_feed continue banner is noise");

    auto s1 = stripProtocolNoise("</context_feed>\nreal plan here");
    CHECK(s1 == "real plan here", "peel front close leaves prose");

    auto s2 = stripProtocolNoise("real plan here\n</response>");
    CHECK(s2 == "real plan here", "peel back close leaves prose");

    auto s4 = stripProtocolNoise("Map the src/ directory structure");
    CHECK(s4 == "Map the src/ directory structure", "clean prose kept");

    auto s5 = stripProtocolNoise("  \n  ");
    CHECK(s5.empty(), "whitespace-only stripped");

    // Mixed: real thought with a trailing orphan close.
    auto s6 = stripProtocolNoise(
        "I should ping discovery next.</context_feed>");
    CHECK(s6.find("ping discovery") != std::string::npos,
          "real thought kept with trailing close stripped");


    // nm / c++filt symbol-table dumps must never paint as thoughts.
    {
        std::string dump;
        for (int i = 0; i < 12; ++i)
            dump += "_ZNKSt17basic_string_viewIwSt11char_traitsIwEE13find_first_ofES2_m "
                    "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEpLEPKc "
                    "std::__cxx11::basic_string ";
        CHECK(looksLikeSymbolDump(dump), "itanium mangled dump detected");
        CHECK(isThoughtNoise(dump), "symbol dump is thought noise");
        CHECK(stripProtocolNoise(dump).empty(), "symbol dump stripped empty");
    }
    CHECK(!looksLikeSymbolDump("I should inspect the workflow runtime next."),
          "normal prose is not a symbol dump");

    {
        std::string plan;
        for (int i = 0; i < 80; ++i)
            plan += "read-hub-key-files-list tool fs_read examples/boilerplate-hub true 50 ";
        CHECK(looksLikeToolPlanDump(plan), "12k tool-plan thought is a dump");
        CHECK(isThoughtNoise(plan), "tool-plan dump is thought noise");
        CHECK(!looksLikeToolPlanDump("I should read the hub Makefile next."),
              "short prose is not a tool-plan dump");
    }

    std::cout << (g_fail ? "\nFAIL\n" : "\nok\n");
        return g_fail ? 1 : 0;
}
