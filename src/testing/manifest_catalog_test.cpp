// Catalog discovery — hub must never return empty for a real PROD tree.
#include <iostream>
#include <string>

#include "src/core/agent_catalog.hpp"

using namespace cortex::mk3::catalog;

static int passed = 0, failed = 0;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        std::cout << "  " << (m) << "... ";                                    \
        if (c) {                                                               \
            ++passed;                                                          \
            std::cout << "PASS\n";                                             \
        } else {                                                               \
            ++failed;                                                          \
            std::cout << "FAIL\n";                                             \
        }                                                                      \
    } while (0)

int main() {
    std::cout << "manifest_catalog_test\n";

    // Simulate the broken hub path: override = agent.yml file
    auto rootsFromAgent = manifestsSearchRoots("manifests/agents/coder/agent.yml");
    CHECK(!rootsFromAgent.empty(), "agent.yml override walks up to a manifests root");
    bool hasAgents = false;
    for (const auto& r : rootsFromAgent) {
        if (r.first.find("/manifests") != std::string::npos) hasAgents = true;
    }
    CHECK(hasAgents, "resolved root path contains manifests");

    auto all = discoverManifests("manifests/agents/coder/agent.yml");
    CHECK(all.size() >= 10, "discoverManifests returns a non-trivial PROD set");

    int agents = 0, tools = 0;
    bool hasCoder = false, hasDefault = false, hasBrainstormer = false, hasOrch = false;
    for (const auto& e : all) {
        if (e.kind == "agent") {
            ++agents;
            if (e.name == "coder") hasCoder = true;
            if (e.name == "default") hasDefault = true;
            if (e.name == "brainstormer") hasBrainstormer = true;
            if (e.name == "std-orchestrator") hasOrch = true;
        }
        if (e.kind == "tool") ++tools;
    }
    CHECK(agents >= 5, "at least 5 agent manifests (including nested)");
    CHECK(tools >= 5, "built-in tools appear in hub");
    CHECK(hasCoder && hasDefault, "coder + default present");
    CHECK(hasBrainstormer, "brainstormer restored to PROD");
    CHECK(hasOrch, "std-orchestrator present in PROD");

    // Empty placeholder dirs must not be the only root when a real tree exists.
    auto roots = manifestsSearchRoots("");
    CHECK(!roots.empty(), "default search finds at least one root via cwd/binary");

    std::cout << passed << " passed, " << failed << " failed\n";
    std::cout << "  (debug) manifests=" << all.size() << " agents=" << agents
              << " tools=" << tools << " roots=" << roots.size() << "\n";
    return failed ? 1 : 0;
}
