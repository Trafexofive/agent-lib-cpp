// =============================================================================
// agent-lib-MK3 — Manifest semantics tests
//
// Verifies agent.yml is the source of truth for runtime/context/provider and
// that sub-agents keep their own cognitive_engine instead of inheriting root
// provider settings.
// =============================================================================

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "src/core/agent.hpp"
#include "src/core/manifest_loader.hpp"
#include "src/providers/factory.hpp"

using namespace cortex::mk3;
namespace fs = std::filesystem;

static int passed = 0, failed = 0;

#define TEST(name)                           \
    do {                                     \
        std::cout << "  " << name << "... "; \
    } while (0)
#define PASS()                 \
    do {                       \
        passed++;              \
        std::cout << "PASS\n"; \
    } while (0)
#define FAIL(msg)                             \
    do {                                      \
        failed++;                             \
        std::cout << "FAIL: " << msg << "\n"; \
        return;                               \
    } while (0)
#define CHECK(cond, msg) \
    do {                 \
        if (!(cond)) {   \
            FAIL(msg);   \
        }                \
    } while (0)

static void writeFile(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << s;
}

void test_context_section_wires_to_agent_config() {
    TEST("agent.yml context section drives AgentConfig");
    fs::path root = fs::temp_directory_path() / "mk3-manifest-semantics-context";
    fs::remove_all(root);
    writeFile(root / "agent.yml", R"YAML(kind: Agent
name: context-agent
version: "1.0"
cognitive_engine:
  primary:
    provider: openai-codex
    model: gpt-5.5
context:
  max_iterations: 3
  history_cap: 22
  action_timeout_sec: 17
)YAML");

    auto cfg = ManifestLoader::loadAgentConfig((root / "agent.yml").string());
    CHECK(cfg.iterationCap == 3, "context.max_iterations not parsed");
    CHECK(cfg.historyCap == 22, "context.history_cap not parsed");
    CHECK(cfg.actionTimeoutSec == 17, "context.action_timeout_sec not parsed");
    PASS();
}

void test_prompt_building_runtime_capabilities_parse() {
    TEST("prompt_building.runtime_capabilities parses");
    fs::path root = fs::temp_directory_path() / "mk3-manifest-semantics-prompt-building";
    fs::remove_all(root);
    writeFile(root / "agent.yml", R"YAML(kind: Agent
name: prompt-building-agent
version: "1.0"
cognitive_engine:
  primary:
    provider: openai-codex
    model: gpt-5.5
prompt_building:
  runtime_capabilities:
    input_schemas: false
    return_schemas: disable
    usage_examples: false
)YAML");

    auto cfg = ManifestLoader::loadAgentConfig((root / "agent.yml").string());
    CHECK(!cfg.promptBuilding.runtimeCapabilities.inputSchemas, "input_schemas not parsed");
    CHECK(!cfg.promptBuilding.runtimeCapabilities.returnSchemas, "return_schemas not parsed");
    CHECK(!cfg.promptBuilding.runtimeCapabilities.usageExamples, "usage_examples not parsed");
    PASS();
}

void test_tool_schema_xml_respects_prompt_building_flags() {
    TEST("tool schema XML respects prompt_building flags");
    fs::path root = fs::temp_directory_path() / "mk3-manifest-semantics-prompt-building-xml";
    fs::remove_all(root);
    writeFile(root / "agent.yml", R"YAML(kind: Agent
name: prompt-building-xml-agent
version: "1.0"
cognitive_engine:
  primary:
    provider: openai-codex
    model: gpt-5.5
import:
  tools:
    - context_pin
)YAML");

    auto cfg = ManifestLoader::loadAgentConfig((root / "agent.yml").string());
    auto provider = providers::createProvider(cfg.provider, cfg.model);
    CHECK(provider, "could not create provider");
    Agent agent(cfg, provider);
    auto schemas = ManifestLoader::loadTools((root / "agent.yml").string(), agent);
    CHECK(!schemas.empty(), "context_pin schema missing");
    auto xml = ManifestLoader::toolSchemasToXml(schemas, 4, true, false, false);
    CHECK(xml.find("<params>") != std::string::npos, "input params missing when included");
    CHECK(xml.find("<returns>") == std::string::npos, "return schema not disabled");
    CHECK(xml.find("<examples>") == std::string::npos, "usage examples not disabled");
    PASS();
}

void test_runtime_subagent_persistence_parses() {
    TEST("runtime.subagents.persistence parses");
    fs::path root = fs::temp_directory_path() / "mk3-manifest-semantics-subagent-persistence";
    fs::remove_all(root);
    writeFile(root / "agent.yml", R"YAML(kind: Agent
name: persistence-agent
version: "1.0"
cognitive_engine:
  primary:
    provider: openai-codex
    model: gpt-5.5
runtime:
  subagents:
    persistence: session
)YAML");

    auto cfg = ManifestLoader::loadAgentConfig((root / "agent.yml").string());
    CHECK(cfg.subAgentPersistence == "session", "runtime.subagents.persistence not parsed");
    PASS();
}

void test_tool_implementation_input_type_parses() {
    TEST("tool implementation.input_type parses");
    fs::path root = fs::temp_directory_path() / "mk3-manifest-semantics-tool-input-type";
    fs::remove_all(root);
    writeFile(root / "tool.yml", R"YAML(kind: Tool
name: writer
version: "1.0"
summary: "writer"
implementation:
  type: script
  runtime: python3
  entrypoint: ./src/main.py
  input_type: text
  text_param: content
)YAML");

    auto schema = ManifestLoader::loadToolManifest((root / "tool.yml").string());
    CHECK(schema.inputType == "text", "implementation.input_type not parsed");
    CHECK(schema.textParam == "content", "implementation.text_param not parsed");
    PASS();
}

void test_builtin_tool_schemas_render_for_prompt_surface() {
    TEST("builtin tool schemas load for prompt surface");
    fs::path root = fs::temp_directory_path() / "mk3-manifest-semantics-builtin-tools";
    fs::remove_all(root);
    writeFile(root / "agent.yml", R"YAML(kind: Agent
name: builtin-schema-agent
version: "1.0"
cognitive_engine:
  primary:
    provider: openai-codex
    model: gpt-5.5
import:
  tools:
    - context_pin
    - context_peek
    - context_unpin
)YAML");

    auto cfg = ManifestLoader::loadAgentConfig((root / "agent.yml").string());
    auto provider = providers::createProvider(cfg.provider, cfg.model);
    CHECK(provider, "could not create provider");
    Agent agent(cfg, provider);
    auto schemas = ManifestLoader::loadTools((root / "agent.yml").string(), agent);
    auto xml = ManifestLoader::toolSchemasToXml(schemas, 4);

    CHECK(agent.hasTool("context_pin"), "context_pin not registered");
    CHECK(agent.hasTool("context_peek"), "context_peek not registered");
    CHECK(agent.hasTool("context_unpin"), "context_unpin not registered");
    CHECK(xml.find("<tool name=\"context_pin\">") != std::string::npos,
          "context_pin schema missing from XML");
    CHECK(xml.find("<params>") != std::string::npos, "context_pin params missing from XML");
    CHECK(xml.find("<returns>") != std::string::npos, "context_pin returns missing from XML");
    CHECK(xml.find("<examples>") != std::string::npos, "context_pin examples missing from XML");
    PASS();
}

void test_session_tools_do_not_autoload_without_manifest_import() {
    TEST("session tools do not autoload into fresh agent capability surface");
    fs::path root = fs::temp_directory_path() / "mk3-manifest-semantics-session-tools";
    fs::remove_all(root);
    writeFile(root / "_session" / "tools.json", R"JSON([
  {
    "name": "stale_tool",
    "description": "must not leak",
    "scriptRuntime": "bash",
    "scriptPath": "/tmp/stale-tool.sh"
  }
])JSON");

    AgentConfig cfg;
    cfg.name = "fresh-agent";
    cfg.provider = "openai-codex";
    cfg.model = "gpt-5.5";
    cfg.manifestDir = root.string();
    auto provider = providers::createProvider(cfg.provider, cfg.model);
    CHECK(provider, "could not create provider");
    Agent agent(cfg, provider);

    CHECK(!agent.hasTool("stale_tool"), "stale _session/tools.json leaked into fresh agent");
    PASS();
}

void test_subagent_keeps_own_provider_and_model() {
    TEST("sub-agent keeps own cognitive_engine provider/model");
    fs::path root = fs::temp_directory_path() / "mk3-manifest-semantics-subagent";
    fs::remove_all(root);
    writeFile(root / "agent.yml", R"YAML(kind: Agent
name: root-agent
version: "1.0"
cognitive_engine:
  primary:
    provider: openai-codex
    model: gpt-5.5
import:
  agents:
    - worker
)YAML");
    writeFile(root / "agents" / "worker" / "agent.yml", R"YAML(kind: Agent
name: worker
version: "1.0"
cognitive_engine:
  primary:
    provider: openrouter
    model: nex-agi/nex-n2-pro:free
)YAML");

    auto rootCfg = ManifestLoader::loadAgentConfig((root / "agent.yml").string());
    auto provider = providers::createProvider(rootCfg.provider, rootCfg.model);
    CHECK(provider, "could not create root provider");
    Agent rootAgent(rootCfg, provider);
    ManifestLoader::loadSubAgents((root / "agent.yml").string(), rootAgent, rootCfg.provider);

    CHECK(rootAgent.hasSubAgent("worker"), "worker sub-agent not loaded");
    Agent* sub = rootAgent.getSubAgent("worker");
    CHECK(sub, "getSubAgent returned null");
    CHECK(sub->config().provider == "openrouter", "sub-agent provider was overwritten by parent");
    CHECK(sub->config().model == "nex-agi/nex-n2-pro:free", "sub-agent model changed unexpectedly");
    PASS();
}

void test_global_subagent_resolution_and_prompt_metadata() {
    TEST("global sub-agent import renders metadata");
    fs::path root = fs::temp_directory_path() / "mk3-manifest-semantics-global-subagent";
    fs::remove_all(root);
    writeFile(root / "agent.yml", R"YAML(kind: Agent
name: root-agent
version: "1.0"
cognitive_engine:
  primary:
    provider: deepseek
    model: deepseek-chat
import:
  agents: [default]
)YAML");

    auto rootCfg = ManifestLoader::loadAgentConfig((root / "agent.yml").string());
    auto provider = providers::createProvider(rootCfg.provider, rootCfg.model);
    CHECK(provider, "could not create root provider");
    Agent rootAgent(rootCfg, provider);
    ManifestLoader::loadSubAgents((root / "agent.yml").string(), rootAgent, rootCfg.provider);

    CHECK(rootAgent.hasSubAgent("default"), "global default sub-agent not loaded");
    Agent* sub = rootAgent.getSubAgent("default");
    CHECK(sub, "getSubAgent(default) returned null");
    CHECK(sub->findTool("exec") != nullptr, "global sub-agent tools were not loaded");

    std::string prompt = rootAgent.renderSystemPrompt();
    CHECK(prompt.find("<sub_agents>") != std::string::npos, "prompt missing <sub_agents>");
    CHECK(prompt.find("<sub_agent name=\"default\"") != std::string::npos,
          "prompt missing default <sub_agent>");
    CHECK(prompt.find("<tool name=\"exec\"") != std::string::npos,
          "prompt missing sub-agent tool metadata");
    size_t subStart = prompt.find("<sub_agents>");
    size_t subEnd = prompt.find("</sub_agents>");
    CHECK(subStart != std::string::npos && subEnd != std::string::npos && subEnd > subStart,
          "sub_agents block bounds invalid");
    std::string subBlock = prompt.substr(subStart, subEnd - subStart);
    CHECK(subBlock.find("<params>") == std::string::npos,
          "sub-agent tool metadata should not include schemas");
    PASS();
}

int main() {
    std::cout.setf(std::ios::unitbuf);
    std::cout << "\n╔══════════════════════════════════════════╗\n";
    std::cout << "║   MK3 Manifest Semantics Tests           ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n\n";

    test_context_section_wires_to_agent_config();
    test_prompt_building_runtime_capabilities_parse();
    test_tool_schema_xml_respects_prompt_building_flags();
    test_runtime_subagent_persistence_parses();
    test_tool_implementation_input_type_parses();
    test_builtin_tool_schemas_render_for_prompt_surface();
    test_session_tools_do_not_autoload_without_manifest_import();
    test_subagent_keeps_own_provider_and_model();
    test_global_subagent_resolution_and_prompt_metadata();

    std::cout << "\n──────────────────────────────────────────\n";
    std::cout << "  " << passed << " passed, " << failed << " failed\n";
    std::cout << "──────────────────────────────────────────\n";
    return failed > 0 ? 1 : 0;
}
