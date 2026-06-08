#pragma once
// =============================================================================
// agent-lib-MK3 — GenericOpenAI Provider
// ONE implementation for all OpenAI-compatible APIs.
// DeepSeek, OpenRouter, Groq, Together, Fireworks, Zen, NVIDIA — all here.
// No more 18 separate classes. Just configs.
// =============================================================================

#include "../core/provider.hpp"
#include <curl/curl.h>
#include <string>
#include <map>
#include <cstdlib>

namespace cortex::mk3::providers {

// ---------------------------------------------------------------------------
// Provider preset — just a config struct, no code duplication
// ---------------------------------------------------------------------------
struct OpenAIProviderConfig {
    std::string name;
    std::string baseUrl;
    std::string apiKeyEnvVar;               // e.g. "DEEPSEEK_API_KEY"
    std::string defaultApiKeyFallback;       // e.g. "public" for zen
    std::string defaultModel;
    std::map<std::string, std::string> extraHeaders;
    bool supportsTools = true;               // native function calling
    bool supportsTopK = true;                // some providers (Groq) reject top_k
    std::string chatEndpoint = "/chat/completions";
    std::string modelsEndpoint = "/models";

    std::string resolveApiKey() const {
        if (!apiKeyEnvVar.empty()) {
            const char* env = std::getenv(apiKeyEnvVar.c_str());
            if (env && env[0]) return env;
        }
        return defaultApiKeyFallback;
    }
};

// ---------------------------------------------------------------------------
// GenericOpenAIClient — the ONE client class
// ---------------------------------------------------------------------------
class GenericOpenAIClient : public ILlmProvider {
public:
    explicit GenericOpenAIClient(const OpenAIProviderConfig& cfg);

    // ILlmProvider interface
    std::string generate(const ChatMessages& msgs) override;
    void generateStream(const ChatMessages& msgs, StreamCallback cb) override;

    void setModel(const std::string& model) override { model_ = model; }
    void setTemperature(double t) override { temperature_ = t; }
    void setMaxTokens(int n) override { maxTokens_ = n; }
    void setTopP(double p) override { topP_ = p; }
    void setTopK(int k) override { topK_ = k; }
    void setPresencePenalty(double p) override { presencePenalty_ = p; }
    void setFrequencyPenalty(double p) override { frequencyPenalty_ = p; }
    std::string getModel() const override { return model_; }
    double getTemperature() const override { return temperature_; }
    int getMaxTokens() const override { return maxTokens_; }
    std::string providerName() const override { return config_.name; }

    // Model discovery
    std::vector<ModelInfo> listModels() override;

private:
    OpenAIProviderConfig config_;
    std::string apiKey_;
    std::string model_;
    double temperature_ = 0.7;
    double topP_ = 0.95;
    int topK_ = 40;
    double presencePenalty_ = 0.0;
    double frequencyPenalty_ = 0.0;
    int maxTokens_ = 8192;

    // HTTP
    Json::Value buildRequestBody(const ChatMessages& msgs, bool stream) const;
    std::string httpPost(const std::string& url, const Json::Value& body,
                         StreamCallback cb = nullptr, bool stream = false);

    // SSE parsing
    static size_t writeCb(void* ptr, size_t sz, size_t nmemb, void* userdata);
    static size_t streamCb(void* ptr, size_t sz, size_t nmemb, void* userdata);
    static int abortCheckCb(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);

    struct StreamCtx {
        StreamCallback cb;
        std::string buffer;
        std::string lastErrorBody;  // preserved for HTTP error responses
    };

    // Model cache
    mutable std::vector<ModelInfo> cachedModels_;
    mutable bool modelsFetched_ = false;
    mutable int retryCount_ = 0;
    int maxRetries_ = 3;
};

// ===========================================================================
// PRESET CONFIGS — Add providers here, not by subclassing
// ===========================================================================

// DeepSeek — first-class, native
inline OpenAIProviderConfig deepseekConfig() {
    return {
        "deepseek",
        "https://api.deepseek.com/v1",
        "DEEPSEEK_API_KEY",
        "",
        "deepseek-chat",
        {
            {"X-Title", "Cortex-MK3"},
        },
        true
    };
}

// OpenRouter
inline OpenAIProviderConfig openrouterConfig() {
    return {
        "openrouter",
        "https://openrouter.ai/api/v1",
        "OPENROUTER_API_KEY",
        "",
        "meta-llama/llama-3.1-8b-instruct",
        {
            {"HTTP-Referer", "https://github.com/Cortex-Prime-MK1"},
            {"X-Title", "Cortex-MK3"},
        },
        true
    };
}

// Groq — does NOT support top_k
inline OpenAIProviderConfig groqConfig() {
    return {
        "groq",
        "https://api.groq.com/openai/v1",
        "GROQ_API_KEY",
        "",
        "llama-3.3-70b-versatile",
        {},
        true,    // supportsTools
        false    // supportsTopK — Groq rejects top_k
    };
}

// OpenCode Zen (free tier)
inline OpenAIProviderConfig zenConfig() {
    return {
        "zen",
        "https://opencode.ai/zen/v1",
        "",
        "public",
        "big-pickle",
        {
            {"X-Title", "Cortex-MK3"},
            {"HTTP-Referer", "https://github.com/Cortex-Prime-MK1"},
        },
        true
    };
}

// Together AI
inline OpenAIProviderConfig togetherConfig() {
    return {
        "together",
        "https://api.together.xyz/v1",
        "TOGETHER_API_KEY",
        "",
        "meta-llama/Llama-3.3-70B-Instruct-Turbo",
        {},
        true
    };
}

// Fireworks
inline OpenAIProviderConfig fireworksConfig() {
    return {
        "fireworks",
        "https://api.fireworks.ai/inference/v1",
        "FIREWORKS_API_KEY",
        "",
        "accounts/fireworks/models/llama-v3p1-70b-instruct",
        {},
        true
    };
}

} // namespace cortex::mk3::providers
