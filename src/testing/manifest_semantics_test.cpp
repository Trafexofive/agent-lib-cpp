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
#include "src/core/compaction.hpp"
#include "src/core/manifest_loader.hpp"
#include "src/core/sandbox_launcher.hpp"
#include "src/providers/factory.hpp"
#include "src/sandbox/policy.hpp"

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
    CHECK(prompt.find("ephemeral=\"true|false\"") != std::string::npos,
          "prompt missing sub-agent ephemeral modifier");
    CHECK(prompt.find("dump_context=\"true|false\"") != std::string::npos,
          "prompt missing sub-agent dump_context modifier");
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

void test_sandbox_block_parses_full_surface() {
    TEST("sandbox: block parses gates + bind + files");
    fs::path root = fs::temp_directory_path() / "mk3-manifest-semantics-sandbox";
    fs::remove_all(root);
    writeFile(root / "agent.yml", R"YAML(kind: Agent
name: sandbox-agent
version: "1.0"
cognitive_engine:
  primary:
    provider: deepseek
    model: deepseek-chat
sandbox:
  mode: process
  image: alpine:3.19
  network: none
  readonly: false
  allowed_commands: [ls, cat, python3]
  allowed_paths: ["./", "../shared"]
  allowed_hosts: [api.github.com]
  files:
    - ./seed.txt
  bind:
    - ./ctx:/workspace/ctx
    - ./fixtures:/workspace/fixtures:ro
    - path: ./data
      to: /workspace/data
      readonly: true
)YAML");
    writeFile(root / "seed.txt", "seed\n");
    writeFile(root / "ctx" / "a.txt", "a\n");
    writeFile(root / "fixtures" / "f.txt", "f\n");
    writeFile(root / "data" / "d.txt", "d\n");

    auto cfg = ManifestLoader::loadAgentConfig((root / "agent.yml").string());
    CHECK(cfg.sandboxConfigured, "sandboxConfigured not set");
    CHECK(cfg.sandboxMode == "process", "mode not parsed");
    CHECK(cfg.sandboxImage == "alpine:3.19", "image not parsed");
    CHECK(cfg.sandboxNetwork == "none", "network not parsed");
    CHECK(cfg.sandboxCommandsSet, "commands flag not set");
    CHECK(cfg.sandboxAllowedCommands.size() == 3, "allowed_commands size");
    CHECK(cfg.sandboxPathsSet && cfg.sandboxAllowedPaths.size() == 2, "allowed_paths");
    CHECK(cfg.sandboxHostsSet && cfg.sandboxAllowedHosts.size() == 1, "allowed_hosts");
    CHECK(cfg.sandboxFiles.size() == 1, "files not parsed");
    CHECK(cfg.sandboxBinds.size() == 3, "bind entries not parsed");

    bool sawRo = false, sawCtx = false;
    for (const auto& b : cfg.sandboxBinds) {
        if (b.guest == "/workspace/fixtures" && b.readOnly)
            sawRo = true;
        if (b.guest == "/workspace/ctx" && !b.readOnly)
            sawCtx = true;
        if (b.guest == "/workspace/data")
            CHECK(b.readOnly, "map-form readonly not applied");
    }
    CHECK(sawRo, "scalar :ro bind missing");
    CHECK(sawCtx, "scalar RW bind missing");

    auto policy = sandbox::makePolicyFromConfig(cfg, "/workspace");
    CHECK(policy.enabled, "policy not enabled from config");
    CHECK(policy.validate("exec", R"({"command":"ls"})").empty(), "ls allowed");
    CHECK(!policy.validate("exec", R"({"command":"rm"})").empty(), "rm blocked");
    CHECK(policy.validate("web_fetch", R"({"url":"https://api.github.com/x"})").empty(),
          "host allow");
    CHECK(!policy.validate("web_fetch", R"({"url":"https://evil.com"})").empty(),
          "host deny");
    CHECK(!policy.validate("fs_write", R"({"path":"/workspace/fixtures/x"})").empty(),
          "RO bind write blocked");
    PASS();
}

void test_process_bind_materialize_symlinks() {
    TEST("process binds materialize as live symlinks");
    fs::path root = fs::temp_directory_path() / "mk3-sandbox-bind-mat";
    fs::remove_all(root);
    fs::create_directories(root / "host-ctx");
    writeFile(root / "host-ctx" / "note.txt", "live\n");
    fs::path ws = root / "ws";
    fs::create_directories(ws);

    AgentConfig cfg;
    cfg.sandboxConfigured = true;
    cfg.sandboxMode = "process";
    SandboxBind b;
    b.host = (root / "host-ctx").string();
    b.guest = "/workspace/ctx";
    b.readOnly = false;
    cfg.sandboxBinds.push_back(b);

    auto mat = sandbox::materializeProcessBinds(cfg, ws);
    CHECK(mat.ok, "materialize failed");
    fs::path link = ws / "ctx";
    CHECK(fs::is_symlink(link) || fs::exists(link / "note.txt"), "symlink/ctx missing");
    // Live CRUD reflection: write via guest path, read on host
    writeFile(link / "from-agent.txt", "via-guest\n");
    CHECK(fs::exists(root / "host-ctx" / "from-agent.txt"), "write did not reflect on host");
    PASS();
}

void test_compaction_block_and_history_cap_every() {
    TEST("compaction: + max_turns_per_cycle parse and engine");
    fs::path root = fs::temp_directory_path() / "mk3-compaction-parse";
    fs::remove_all(root);
    writeFile(root / "agent.yml", R"YAML(kind: Agent
name: compact-agent
version: "1.0"
cognitive_engine:
  primary: { provider: deepseek, model: deepseek-chat }
runtime:
  history_cap: 50
  max_turns_per_cycle: 15
compaction:
  enabled: true
  profile: balanced
  trigger:
    context_tokens: 60k
    turns: 15
  overrides:
    tags:
      thought: { keep: none }
      result: { keep_last: 20 }
  output:
    mode: summarize_rules
    archive: { enabled: true, sink: file }
)YAML");

    auto cfg = ManifestLoader::loadAgentConfig((root / "agent.yml").string());
    CHECK(cfg.historyCap == 50, "history_cap");
    CHECK(cfg.maxTurnsPerCycle == 15, "max_turns_per_cycle default/parse");
    CHECK(cfg.compaction.configured && cfg.compaction.enabled, "compaction enabled");
    CHECK(cfg.compaction.profile == "balanced", "profile");
    CHECK(cfg.compaction.triggerContextTokens == 60000, "60k tokens parse");
    CHECK(cfg.compaction.triggerTurns == 15, "trigger turns");
    CHECK(cfg.compaction.tags.count("thought"), "thought policy");
    CHECK(cfg.compaction.tags["thought"].keep == "none", "thought none");
    CHECK(cfg.compaction.tags["result"].keepLast == 20, "override keep_last");
    CHECK(cfg.compaction.archiveEnabled, "archive on");

    // Engine: strip thoughts + tail
    std::vector<std::string> hist;
    hist.push_back("User: hi");
    hist.push_back("Agent: <thought>secret</thought><response>ok</response>");
    hist.push_back("System: {\"success\":true,\"data\":\"x\"}");
    for (int i = 0; i < 30; ++i)
        hist.push_back("System: noise-" + std::to_string(i));
    hist.push_back("System: context_pin path=/tmp/x");  // never_drop pin

    auto cr = compaction::compactHistory(hist, cfg.compaction);
    CHECK(cr.didCompact, "should compact");
    bool pinKept = false, thoughtGone = true;
    for (const auto& l : cr.lines) {
        if (l.find("context_pin") != std::string::npos)
            pinKept = true;
        if (l.find("<thought>") != std::string::npos)
            thoughtGone = false;
    }
    CHECK(pinKept, "pin never_drop");
    CHECK(thoughtGone, "thoughts stripped");
    CHECK(!cr.note.empty(), "summarize_rules note");

    // history window every_turns: freeze between recomputes
    int applied = -1000000;
    size_t frozen = 0;
    size_t s1 = compaction::resolveHistoryWindowStart(100, 40, 15, 1, applied, frozen);
    CHECK(s1 == 60, "first clamp to size-cap");
    size_t s2 = compaction::resolveHistoryWindowStart(105, 40, 15, 5, applied, frozen);
    CHECK(s2 == s1 || s2 <= 65, "frozen window before 15 turns");
    size_t s3 = compaction::resolveHistoryWindowStart(120, 40, 15, 20, applied, frozen);
    CHECK(s3 == 80, "reclamp after 15 user turns");
    PASS();
}

void test_import_files_remain_prompt_only() {
    TEST("import.files still loads as prompt modules (not sandbox binds)");
    fs::path root = fs::temp_directory_path() / "mk3-import-files-prompt";
    fs::remove_all(root);
    writeFile(root / "agent.yml", R"YAML(kind: Agent
name: import-files-agent
version: "1.0"
cognitive_engine:
  primary: { provider: deepseek, model: deepseek-chat }
import:
  files:
    - ./extra.md
)YAML");
    writeFile(root / "extra.md", "# Extra contract\nDo the thing.\n");

    auto cfg = ManifestLoader::loadAgentConfig((root / "agent.yml").string());
    CHECK(cfg.sandboxFiles.empty(), "import.files must not populate sandboxFiles");
    CHECK(cfg.sandboxBinds.empty(), "import.files must not populate sandboxBinds");
    auto xml = ManifestLoader::loadPromptModulesXml((root / "agent.yml").string());
    CHECK(xml.find("<module") != std::string::npos, "prompt module missing");
    CHECK(xml.find("Extra contract") != std::string::npos || xml.find("Do the thing") != std::string::npos,
          "module body missing");
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
    test_sandbox_block_parses_full_surface();
    test_process_bind_materialize_symlinks();
    test_import_files_remain_prompt_only();
    test_compaction_block_and_history_cap_every();

    std::cout << "\n──────────────────────────────────────────\n";
    std::cout << "  " << passed << " passed, " << failed << " failed\n";
    std::cout << "──────────────────────────────────────────\n";
    return failed > 0 ? 1 : 0;
}
