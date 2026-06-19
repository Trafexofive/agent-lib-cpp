// Test: verify context block (harness, system, persona) assembles correctly
#include <iostream>
#include "../src/core/agent.hpp"
#include "../src/core/manifest_loader.hpp"
#include "../src/providers/factory.hpp"

int main() {
    using namespace cortex::mk3;

    // Load the default agent manifest
    auto cfg = ManifestLoader::loadAgentConfig("manifests/agents/default/agent.yml");

    std::cout << "=== Config paths ===" << "\n";
    std::cout << "harnessPath:      " << cfg.harnessPath << "\n";
    std::cout << "systemPromptPath: " << cfg.systemPromptPath << "\n";
    std::cout << "personaPath:      " << cfg.personaPath << "\n";

    // Check files exist
    for (auto& label : {"harnessPath", "systemPromptPath", "personaPath"}) {
        std::string path;
        if (std::string(label) == "harnessPath") path = cfg.harnessPath;
        else if (std::string(label) == "systemPromptPath") path = cfg.systemPromptPath;
        else path = cfg.personaPath;

        std::ifstream f(path);
        if (!f) {
            std::cerr << "FAIL: file not found for " << label << ": " << path << "\n";
            return 1;
        }
    }

    // Create provider and agent
    auto provider = providers::createProvider(cfg.provider, cfg.model);
    if (!provider) {
        std::cerr << "FAIL: could not create provider " << cfg.provider << "\n";
        return 1;
    }

    Agent agent(cfg, provider);

    // Render the system prompt
    AgentContext ctx;
    ctx.iteration = 1;
    ctx.userInput = "hello";

    // We need access to buildSystemPrompt — it's private, but renderSystemPrompt
    // is the testing hook. Let's check if it works.
    // Actually, let's just check the loaded text members via the prompt.
    std::string prompt = agent.renderSystemPrompt();

    // Verify all 3 blocks are present
    bool hasHarness = prompt.find("<harness>") != std::string::npos;
    bool hasProtocol = prompt.find("<protocol>") != std::string::npos;
    bool hasPersona = prompt.find("<persona>") != std::string::npos;
    bool hasSystem = prompt.find("  <system_prompt>") != std::string::npos;
    bool hasActionAvailable = prompt.find("<action_available>") != std::string::npos;

    std::cout << "\n=== Prompt structure ===" << "\n";
    std::cout << "<harness>:          " << (hasHarness ? "YES" : "NO") << "\n";
    std::cout << "<protocol>:         " << (hasProtocol ? "YES" : "NO") << "\n";
    std::cout << "<persona>:          " << (hasPersona ? "YES" : "NO") << "\n";
    std::cout << "<system_prompt>:    " << (hasSystem ? "YES" : "NO") << "\n";
    std::cout << "<action_available>: " << (hasActionAvailable ? "YES" : "NO") << "\n";

    // Show a snippet of each block
    auto snippet = [&](const std::string& tag) {
        size_t pos = prompt.find(tag);
        if (pos == std::string::npos) return std::string("NOT FOUND");
        size_t end = prompt.find("\n", pos + tag.size() + 1);
        end = prompt.find("\n", end + 1);
        end = prompt.find("\n", end + 1);
        return prompt.substr(pos, end - pos);
    };

    std::cout << "\n=== Snippets ===" << "\n";
    std::cout << snippet("<persona>") << "\n...\n";
    std::cout << snippet("  <system_prompt>") << "\n...\n";

    int failures = 0;
    if (!hasHarness) { std::cerr << "FAIL: missing <harness>\n"; failures++; }
    if (!hasProtocol) { std::cerr << "FAIL: missing <protocol>\n"; failures++; }
    if (!hasPersona) { std::cerr << "FAIL: missing <persona>\n"; failures++; }
    if (!hasSystem) { std::cerr << "FAIL: missing <system_prompt>\n"; failures++; }
    if (!hasActionAvailable) { std::cerr << "FAIL: missing <action_available>\n"; failures++; }

    if (failures == 0) {
        std::cout << "\n✓ ALL 5 BLOCKS PRESENT\n";
        return 0;
    }
    std::cerr << "\n✗ " << failures << " FAILURES\n";
    return 1;
}
