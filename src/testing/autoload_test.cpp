// =============================================================================
// agent-lib-MK3 — Recursive autoload tests (MA01)
//
// Verifies that when ManifestAutoload::loadGlobal encounters a sub-agent
// manifest, the sub-agent's own `import:` block is honored:
//   - import.tools (path-relative script tools) get registered on the SUB
//   - import.feeds/relics get registered on the SUB
//   - import.agents (nested sub-agents) get recursively constructed
//
// Pre-fix: sub-agents are constructed with an empty world (only the global
// auto-registered built-ins). Post-fix: sub-agents own their declared set.
// =============================================================================

#include "src/core/agent.hpp"
#include "src/core/manifest_autoload.hpp"
#include "src/providers/factory.hpp"
#include <iostream>
#include <string>

using namespace cortex::mk3;

static int passed = 0, failed = 0;

#define TEST(name)  do { std::cout << "  " << name << "... "; } while (0)
#define PASS()      do { passed++; std::cout << "PASS\n"; } while (0)
#define FAIL(msg)   do { failed++; std::cout << "FAIL: " << msg << "\n"; return; } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); } } while (0)

void test_MA01_subagent_loads_own_tools() {
    TEST("MA01 sub-agent's import.tools resolved");

    AgentConfig cfg;
    cfg.provider = "deepseek";
    cfg.model = "deepseek-chat";
    auto provider = providers::createProvider("deepseek", "deepseek-chat");
    CHECK(provider, "could not create deepseek provider");

    Agent container(cfg, provider);

    const std::string root = "tests/fixtures/code-analyzer-module";
    ManifestAutoload::loadGlobal(root, container, "deepseek");

    CHECK(container.hasSubAgent("code-analyzer"),
          "code-analyzer sub-agent was not discovered during autoload");

    Agent* sub = container.getSubAgent("code-analyzer");
    CHECK(sub, "getSubAgent returned null");

    // These are SCRIPT tools declared in the sub-agent's own import.tools.
    // They are NOT built-ins. If MA01 is broken the sub's tools_ map will
    // not contain them.
    CHECK(sub->hasTool("complexity"),
          "sub-agent missing 'complexity' tool (its own import.tools ignored)");
    CHECK(sub->hasTool("dep-graph"),
          "sub-agent missing 'dep-graph' tool (its own import.tools ignored)");
    CHECK(sub->hasTool("test-coverage"),
          "sub-agent missing 'test-coverage' tool (its own import.tools ignored)");

    PASS();
}

int main() {
    std::cout.setf(std::ios::unitbuf);
    std::cout << "\n╔══════════════════════════════════════════╗\n";
    std::cout << "║   MK3 Autoload Tests                     ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n\n";

    test_MA01_subagent_loads_own_tools();

    std::cout << "\n──────────────────────────────────────────\n";
    std::cout << "  " << passed << " passed, " << failed << " failed\n";
    std::cout << "──────────────────────────────────────────\n";
    return failed > 0 ? 1 : 0;
}
