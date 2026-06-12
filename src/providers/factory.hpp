#pragma once
// =============================================================================
// agent-lib-MK3 — Provider Factory
// Decoupled provider creation — no module should import provider headers directly.
// =============================================================================

#include "../core/provider.hpp"
#include "generic_openai.hpp"
#include <memory>
#include <string>

namespace cortex::mk3::providers {

inline std::shared_ptr<ILlmProvider> createProvider(const std::string& name, const std::string& overrideModel = "") {
    OpenAIProviderConfig cfg;

    if (name == "deepseek")       cfg = deepseekConfig();
    else if (name == "openrouter") cfg = openrouterConfig();
    else if (name == "groq")       cfg = groqConfig();
    else if (name == "zen")        cfg = zenConfig();
    else if (name == "together")   cfg = togetherConfig();
    else if (name == "fireworks")  cfg = fireworksConfig();
    else return nullptr;

    if (!overrideModel.empty()) cfg.defaultModel = overrideModel;

    return std::make_shared<GenericOpenAIClient>(cfg);
}

inline std::vector<std::string> availableProviders() {
    return {"deepseek", "openrouter", "groq", "zen", "together", "fireworks", "sambanova", "cerebras", "hyperbolic", "llm7", "nvidia"};
}

inline std::string defaultProviderModel(const std::string& name) {
    if (name == "deepseek")       return "deepseek-chat";
    if (name == "openrouter")     return "nex-agi/nex-n2-pro:free";
    if (name == "groq")           return "llama-3.3-70b-versatile";
    if (name == "zen")            return "big-pickle";
    if (name == "together")       return "meta-llama/Llama-3.3-70B-Instruct-Turbo";
    if (name == "fireworks")      return "accounts/fireworks/models/llama-v3p1-70b-instruct";
    if (name == "sambanova")      return "DeepSeek-R1";
    if (name == "cerebras")       return "llama3.1-405b";
    if (name == "hyperbolic")     return "deepseek-r1";
    if (name == "llm7")           return "deepseek-r1";
    if (name == "nvidia")         return "meta/llama-3.3-70b-instruct";
    return "";
}

} // namespace cortex::mk3::providers
