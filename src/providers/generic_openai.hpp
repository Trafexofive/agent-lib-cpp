#pragma once
// =============================================================================
// agent-lib-MK3 — GenericOpenAI Provider
// ONE implementation for all OpenAI-compatible APIs.
// DeepSeek, OpenRouter, Groq, Together, Fireworks, Zen, NVIDIA — all here.
// No more 18 separate classes. Just configs.
// =============================================================================

#include <curl/curl.h>
#include <json/json.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/provider.hpp"

namespace cortex::mk3::providers {

// ---------------------------------------------------------------------------
// Provider preset — just a config struct, no code duplication
// ---------------------------------------------------------------------------
struct OpenAIProviderConfig {
    std::string name;
    std::string baseUrl;
    std::string apiKeyEnvVar;           // e.g. "DEEPSEEK_API_KEY"
    std::string defaultApiKeyFallback;  // e.g. "public" for zen
    std::string defaultModel;
    std::map<std::string, std::string> extraHeaders;
    bool supportsTools = true;  // native function calling
    bool supportsTopK = true;   // some providers (Groq) reject top_k
    std::string chatEndpoint = "/chat/completions";
    std::string modelsEndpoint = "/models";
    std::string reasoningEffort;  // OpenAI/Codex reasoning models: low|medium|high|xhigh
    int defaultMaxTokens = 65536;
    std::string apiMode = "chat-completions";  // chat-completions | openai-codex-responses

    std::string resolveApiKey() const {
        if (!apiKeyEnvVar.empty()) {
            const char* env = std::getenv(apiKeyEnvVar.c_str());
            if (env && env[0])
                return env;
        }
        if (apiKeyEnvVar == "OPENAI_API_KEY") {
            const char* home = std::getenv("HOME");
            if (home && home[0]) {
                // First-class pi auth store. `pi /login openai-codex` refreshes
                // and persists credentials here; Cortex should reuse that
                // working credential instead of an older ~/.codex token.
                {
                    std::ifstream f(std::string(home) + "/.pi/agent/auth.json");
                    if (f.good()) {
                        Json::Value root;
                        Json::CharReaderBuilder reader;
                        std::string errs;
                        if (Json::parseFromStream(reader, f, &root, &errs) &&
                            root.isMember("openai-codex") && root["openai-codex"].isObject()) {
                            const Json::Value& codex = root["openai-codex"];
                            if (codex.isMember("access") && codex["access"].isString())
                                return codex["access"].asString();
                            if (codex.isMember("key") && codex["key"].isString())
                                return codex["key"].asString();
                            if (codex.isMember("access_token") && codex["access_token"].isString())
                                return codex["access_token"].asString();
                        }
                    }
                }

                // Legacy/official Codex CLI store.
                std::ifstream f(std::string(home) + "/.codex/auth.json");
                if (f.good()) {
                    Json::Value root;
                    Json::CharReaderBuilder reader;
                    std::string errs;
                    if (Json::parseFromStream(reader, f, &root, &errs)) {
                        if (root.isMember("OPENAI_API_KEY") && root["OPENAI_API_KEY"].isString())
                            return root["OPENAI_API_KEY"].asString();
                        if (root.isMember("tokens") && root["tokens"].isObject() &&
                            root["tokens"].isMember("access_token") &&
                            root["tokens"]["access_token"].isString())
                            return root["tokens"]["access_token"].asString();
                    }
                }
            }
        }
        if (apiKeyEnvVar == "XAI_API_KEY") {
            const char* token = std::getenv("XAI_AUTH_TOKEN");
            if (token && token[0])
                return token;

            auto readTokenObject = [](const Json::Value& obj) -> std::string {
                if (!obj.isObject())
                    return "";
                // pi auth.json API-key entries use { type:"api_key", key:"..." }.
                if (obj.isMember("key") && obj["key"].isString())
                    return obj["key"].asString();
                // pi OAuth entries, including pi-xai-oauth, use access/refresh.
                if (obj.isMember("access") && obj["access"].isString())
                    return obj["access"].asString();
                // Grok CLI and older guesses use access_token/token.
                if (obj.isMember("access_token") && obj["access_token"].isString())
                    return obj["access_token"].asString();
                if (obj.isMember("token") && obj["token"].isString())
                    return obj["token"].asString();
                return "";
            };

            const char* home = std::getenv("HOME");
            if (home && home[0]) {
                // First-class pi global auth storage. This is where /login xai-auth
                // persists OAuth credentials for the pi-xai-oauth extension.
                {
                    std::ifstream f(std::string(home) + "/.pi/agent/auth.json");
                    if (f.good()) {
                        Json::Value root;
                        Json::CharReaderBuilder reader;
                        std::string errs;
                        if (Json::parseFromStream(reader, f, &root, &errs)) {
                            for (const char* provider : {"xai-auth", "xai", "x-ai", "grok"}) {
                                if (!root.isMember(provider))
                                    continue;
                                std::string access = readTokenObject(root[provider]);
                                if (!access.empty())
                                    return access;
                            }
                        }
                    }
                }

                // Reuse the official Grok CLI OAuth bearer when present, mirroring
                // pi-xai-oauth's secondary credential discovery path.
                std::ifstream f(std::string(home) + "/.grok/auth.json");
                if (f.good()) {
                    Json::Value root;
                    Json::CharReaderBuilder reader;
                    std::string errs;
                    if (Json::parseFromStream(reader, f, &root, &errs)) {
                        const std::string oidcScope =
                            "https://auth.x.ai::b1a00492-073a-47ea-816f-4c329264a828";
                        const std::string legacyScope = "https://accounts.x.ai/sign-in";
                        if (root.isMember(oidcScope)) {
                            std::string access = readTokenObject(root[oidcScope]);
                            if (!access.empty())
                                return access;
                        }
                        if (root.isMember(legacyScope)) {
                            std::string access = readTokenObject(root[legacyScope]);
                            if (!access.empty())
                                return access;
                        }
                        if (root.isMember("access_token") && root["access_token"].isString())
                            return root["access_token"].asString();
                        if (root.isMember("token") && root["token"].isString())
                            return root["token"].asString();
                    }
                }
            }
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

    void setModel(const std::string& model) override {
        // Preserve the provider's configured default when an empty model is
        // passed (happens when --model is omitted and no manifest sets one).
        // Otherwise buildRequestBody would send "model":"" and the API 401s.
        if (!model.empty())
            model_ = model;
    }
    void setTemperature(double t) override {
        temperature_ = t;
    }
    void setMaxTokens(int n) override {
        maxTokens_ = n;
    }
    void setTopP(double p) override {
        topP_ = p;
    }
    void setTopK(int k) override {
        topK_ = k;
    }
    void setPresencePenalty(double p) override {
        presencePenalty_ = p;
    }
    void setFrequencyPenalty(double p) override {
        frequencyPenalty_ = p;
    }
    void setQuietLogs(bool q) override {
        quietLogs_ = q;
    }
    void setRetryCallback(RetryCallback cb) override {
        retryCb_ = cb;
    }
    // Manifest-configurable stream stall cutoff (runtime.throttling). 0 = off.
    void setStreamStallTimeoutSec(int sec) override {
        streamStallTimeoutSec_ = sec;
    }
    std::string getModel() const override {
        return model_;
    }
    double getTemperature() const override {
        return temperature_;
    }
    int getMaxTokens() const override {
        return maxTokens_;
    }
    std::string providerName() const override {
        return config_.name;
    }

    // Model discovery
    std::vector<ModelInfo> listModels() override;
    static ModelInfo modelInfoFromJson(const OpenAIProviderConfig& cfg, const Json::Value& m);

    // Stream diagnostics — see provider.hpp
    StreamStats lastStreamStats() const override {
        return lastStats_;
    }

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
    int streamStallTimeoutSec_ = 0;  // runtime.throttling; 0 = off

    // HTTP
    Json::Value buildRequestBody(const ChatMessages& msgs, bool stream) const;
    std::string httpPost(const std::string& url, const Json::Value& body,
                         StreamCallback cb = nullptr, bool stream = false);

    // SSE parsing
    static size_t writeCb(void* ptr, size_t sz, size_t nmemb, void* userdata);
    static size_t streamCb(void* ptr, size_t sz, size_t nmemb, void* userdata);
    static int abortCheckCb(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                            curl_off_t ulnow);

    struct StreamCtx {
        StreamCallback cb;
        std::string buffer;
        std::string lastErrorBody;  // preserved for HTTP error responses
        bool codexResponses = false;
        bool codexSawTextDelta = false;
        bool anyContent = false;   // true if any non-thinking token reached cb
        std::string finishReason;  // last finish_reason seen in SSE deltas
        long httpStatus = 0;       // HTTP status of the streaming response
        // Streaming stall detection: last time a chunk arrived. A model that is
        // connected but dribbles zero bytes for a long window is stalled (the
        // "spinner spins forever" hang). Free models legitimately pause seconds
        // between tokens, so the cutoff is generous and manifest-configurable.
        std::chrono::steady_clock::time_point lastChunk{std::chrono::steady_clock::now()};
        // stallTimeoutSec: 0 = no progress-based cutoff (LOW_SPEED governs);
        // >0 = abort if zero bytes arrive for this many seconds.
        int stallTimeoutSec = 0;
    };

    mutable StreamStats lastStats_;

    // Model cache
    mutable std::vector<ModelInfo> cachedModels_;
    mutable bool modelsFetched_ = false;
    mutable std::unordered_map<std::string, bool> modelTopKSupport_;
    // Transport retries (timeout / recv / 429). Free tiers flake often.
    int maxRetries_ = 6;
    bool quietLogs_ = false;
    RetryCallback retryCb_;

    static std::string resolveCodexAccountId(const std::string& token);
};

// ===========================================================================
// PRESET CONFIGS — Add providers here, not by subclassing
// ===========================================================================

// DeepSeek — first-class, native
inline OpenAIProviderConfig deepseekConfig() {
    return {"deepseek",
            "https://api.deepseek.com/v1",
            "DEEPSEEK_API_KEY",
            "",
            "deepseek-chat",
            {
                {"X-Title", "Cortex-MK3"},
            },
            true,
            true,
            "/chat/completions",
            "/models",
            "",
            65536,
            "chat-completions"};
}

// OpenRouter
inline OpenAIProviderConfig openrouterConfig() {
    return {"openrouter",
            "https://openrouter.ai/api/v1",
            "OPENROUTER_API_KEY",
            "",
            "nex-agi/nex-n2-pro:free",
            {
                {"HTTP-Referer", "https://github.com/Cortex-Prime-MK1"},
                {"X-Title", "Cortex-MK3"},
            },
            true,
            false,  // OpenRouter is model-specific; direct runs omit top_k unless /models says it
                    // is supported.
            "/chat/completions",
            "/models",
            "",
            4096,
            "chat-completions"};
}

// OpenAI Codex subscription/API surface
inline OpenAIProviderConfig codexConfig() {
    OpenAIProviderConfig cfg{"openai-codex",
                             "https://chatgpt.com/backend-api",
                             "OPENAI_API_KEY",
                             "",
                             "gpt-5.5",
                             {},
                             true,
                             false,
                             "/codex/responses",
                             "",
                             "high",
                             65536,
                             "openai-codex-responses"};
    cfg.reasoningEffort = "high";
    cfg.defaultMaxTokens = 65536;
    return cfg;
}

// xAI / Grok — OpenAI-compatible API. API keys use XAI_API_KEY; if absent,
// we also accept XAI_AUTH_TOKEN or a bearer from ~/.grok/auth.json for parity
// with pi-xai-oauth's Grok CLI credential discovery.
inline OpenAIProviderConfig xaiConfig() {
    return {"xai",
            "https://api.x.ai/v1",
            "XAI_API_KEY",
            "",
            "grok-4.5",
            {
                {"X-Title", "Cortex-MK3"},
            },
            true,
            false,
            "/chat/completions",
            "/models",
            "",
            131072,
            "chat-completions"};
}

// Groq — does NOT support top_k
inline OpenAIProviderConfig groqConfig() {
    return {"groq",
            "https://api.groq.com/openai/v1",
            "GROQ_API_KEY",
            "",
            "llama-3.3-70b-versatile",
            {},
            true,   // supportsTools
            false,  // supportsTopK — Groq rejects top_k
            "/chat/completions",
            "/models",
            "",
            65536,
            "chat-completions"};
}

// OpenCode Zen (free tier)
// OpenCode Zen — FREE tier (anonymous, Bearer "public")
// Only the "-free"-suffixed models work here. Paid model IDs (kimi-k2.6,
// deepseek-v4-flash, etc.) live on the /go/ paid-sub endpoint (opencode-go).
// Verified working free models (2026-06-17):
//   deepseek-v4-flash-free  (default — fast, matches go-sub default)
//   mimo-v2.5-free
//   north-mini-code-free
// Promotions that have ended (401 "Free promotion has ended"):
//   minimax-m3-free, qwen3.6-plus-free, nemotron-3-ultra-free (flaky)
// Note: the no-auth /models listing is the only one that surfaces -free IDs;
// the authenticated listing hides them.
inline OpenAIProviderConfig zenConfig() {
    return {"zen",
            "https://opencode.ai/zen/v1",
            "",
            "public",
            "deepseek-v4-flash-free",
            {
                {"X-Title", "Cortex-MK3"},
                {"HTTP-Referer", "https://github.com/Cortex-Prime-MK1"},
            },
            true,
            true,
            "/chat/completions",
            "/models",
            "high",
            65536,
            "chat-completions"};
}

// OpenCode Go (paid subscription tier — $10/mo)
// Requires API key from https://opencode.ai/auth
// Supports: deepseek-v4-flash, deepseek-v4-pro, qwen3.7-max, kimi-k2.7, glm-5.2, etc.
inline OpenAIProviderConfig opencodeGoConfig() {
    return {"opencode-go",
            "https://opencode.ai/zen/go/v1",
            "OPENCODE_API_KEY",
            "",
            "deepseek-v4-flash",
            {
                {"X-Title", "Cortex-MK3"},
                {"HTTP-Referer", "https://github.com/Cortex-Prime-MK1"},
            },
            true,
            true,
            "/chat/completions",
            "/models",
            "high",
            65536,
            "chat-completions"};
}

// Together AI
inline OpenAIProviderConfig togetherConfig() {
    return {"together",
            "https://api.together.xyz/v1",
            "TOGETHER_API_KEY",
            "",
            "meta-llama/Llama-3.3-70B-Instruct-Turbo",
            {},
            true,
            true,
            "/chat/completions",
            "/models",
            "",
            65536,
            "chat-completions"};
}

// Fireworks
inline OpenAIProviderConfig fireworksConfig() {
    return {"fireworks",
            "https://api.fireworks.ai/inference/v1",
            "FIREWORKS_API_KEY",
            "",
            "accounts/fireworks/models/llama-v3p1-70b-instruct",
            {},
            true,
            true,
            "/chat/completions",
            "/models",
            "",
            65536,
            "chat-completions"};
}

// SambaNova
inline OpenAIProviderConfig sambanovaConfig() {
    return {"sambanova",
            "https://api.sambanova.ai/v1",
            "SAMBANOVA_API_KEY",
            "",
            "DeepSeek-R1",
            {},
            true,
            false,
            "/chat/completions",
            "/models",
            "",
            65536,
            "chat-completions"};
}

// Cerebras
inline OpenAIProviderConfig cerebrasConfig() {
    return {"cerebras",
            "https://api.cerebras.ai/v1",
            "CEREBRAS_API_KEY",
            "",
            "llama3.1-405b",
            {},
            true,
            false,
            "/chat/completions",
            "/models",
            "",
            65536,
            "chat-completions"};
}

// Hyperbolic
inline OpenAIProviderConfig hyperbolicConfig() {
    return {"hyperbolic",
            "https://api.hyperbolic.xyz/v1",
            "HYPERBOLIC_API_KEY",
            "",
            "deepseek-r1",
            {},
            true,
            false,
            "/chat/completions",
            "/models",
            "",
            65536,
            "chat-completions"};
}

// LLM7
inline OpenAIProviderConfig llm7Config() {
    return {"llm7",
            "https://api.llm7.io/v1",
            "",
            "public",
            "deepseek-r1",
            {},
            true,
            false,
            "/chat/completions",
            "/models",
            "",
            65536,
            "chat-completions"};
}

// NVIDIA NIM
inline OpenAIProviderConfig nvidiaConfig() {
    return {"nvidia",
            "https://integrate.api.nvidia.com/v1",
            "NVIDIA_API_KEY",
            "",
            "meta/llama-3.3-70b-instruct",
            {},
            true,
            false,
            "/chat/completions",
            "/models",
            "",
            65536,
            "chat-completions"};
}

}  // namespace cortex::mk3::providers
