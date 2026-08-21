#include <json/json.h>

#include <atomic>
#include <cstdlib>
#include <iostream>

#include "src/providers/generic_openai.hpp"



static void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

int main() {
    auto cfg = cortex::mk3::providers::openrouterConfig();
    expect(!cfg.supportsTopK, "openrouter defaults top_k off for direct runs");

    auto xaiCfg = cortex::mk3::providers::xaiConfig();
    expect(xaiCfg.defaultModel == "grok-4.5", "xai default model");
    expect(xaiCfg.baseUrl == "https://api.x.ai/v1", "xai base url");
    expect(!xaiCfg.supportsTopK, "xai defaults top_k off for direct runs");

    Json::Value ultra(Json::objectValue);
    ultra["id"] = "nvidia/nemotron-3-ultra-550b-a55b:free";
    ultra["name"] = "NVIDIA: Nemotron 3 Ultra (free)";
    ultra["context_length"] = 1000000;
    ultra["pricing"]["prompt"] = "0";
    ultra["pricing"]["completion"] = "0";
    ultra["supported_parameters"] = Json::arrayValue;
    ultra["supported_parameters"].append("include_reasoning");
    ultra["supported_parameters"].append("max_tokens");
    ultra["supported_parameters"].append("tool_choice");
    ultra["supported_parameters"].append("tools");
    ultra["supported_parameters"].append("top_p");

    auto ultraInfo = cortex::mk3::providers::GenericOpenAIClient::modelInfoFromJson(cfg, ultra);
    expect(ultraInfo.id == "nvidia/nemotron-3-ultra-550b-a55b:free", "ultra id");
    expect(ultraInfo.contextWindow == 1000000, "ultra context_length parsed");
    expect(ultraInfo.isFree, "ultra pricing detected as free");
    expect(!ultraInfo.supportsTopK, "ultra top_k not supported");

    Json::Value nex(Json::objectValue);
    nex["id"] = "nex-agi/nex-n2-pro:free";
    nex["name"] = "Nex: N2 Pro (free)";
    nex["context_length"] = 65536;
    nex["supported_parameters"] = Json::arrayValue;
    nex["supported_parameters"].append("temperature");
    nex["supported_parameters"].append("top_k");
    nex["supported_parameters"].append("top_p");

    auto nexInfo = cortex::mk3::providers::GenericOpenAIClient::modelInfoFromJson(cfg, nex);
    expect(nexInfo.contextWindow == 65536, "nex context_length parsed");
    expect(nexInfo.supportsTopK, "nex top_k supported");

    Json::Value fallback(Json::objectValue);
    fallback["id"] = "nvidia/nemotron-3-ultra-550b-a55b";
    fallback["name"] = "NVIDIA: Nemotron 3 Ultra";

    auto fallbackInfo =
        cortex::mk3::providers::GenericOpenAIClient::modelInfoFromJson(cfg, fallback);
    expect(fallbackInfo.contextWindow == 1000000, "openrouter known fallback context");
    expect(!fallbackInfo.supportsTopK, "missing supported_parameters defaults to provider support");

    Json::Value grok(Json::objectValue);
    grok["id"] = "grok-4.5";
    grok["name"] = "Grok 4.5";

    auto grokInfo = cortex::mk3::providers::GenericOpenAIClient::modelInfoFromJson(xaiCfg, grok);
    expect(grokInfo.contextWindow == 500000, "xai known fallback context");
    expect(!grokInfo.supportsTopK, "xai missing supported_parameters defaults to no top_k");

    std::cout << "provider model metadata tests passed\n";
    return 0;
}
