#pragma once
// =============================================================================
// agent-lib-MK3 — LLM Provider Interface
// Single clean interface. All providers implement this.
// =============================================================================

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "types.hpp"

namespace cortex::mk3 {

struct RetrySignal {
    enum class Kind { Network, Http };   // network = transient transport; http = HTTP code retry
    Kind kind = Kind::Network;
    int attempt = 0;          // 1-based
    int maxAttempts = 0;      // configured cap
    long httpStatus = 0;      // populated for Http
    std::string curlError;    // populated for Network
    int backoffMs = 0;        // wait before next attempt
};

// Severity: "info" | "warn" | "error". One signal per candidate retry sequence
// to dissolve noise; the bridge collapses by id.
using RetryCallback = std::function<void(const RetrySignal&)>;

class ILlmProvider {
   public:
    virtual ~ILlmProvider() = default;

    // Generate a complete response (non-streaming)
    virtual std::string generate(const ChatMessages& msgs) = 0;

    // Generate a streaming response
    virtual void generateStream(const ChatMessages& msgs, StreamCallback cb) = 0;

    // Optional observer hook: providers should call this BEFORE the per-attempt
    // backoff sleeps start, not during. Default is no-op.
    virtual void setRetryCallback(RetryCallback cb) {
        (void)cb;
    }

    // Configuration
    virtual void setModel(const std::string& model) = 0;
    virtual void setTemperature(double t) = 0;
    virtual void setMaxTokens(int n) = 0;
    virtual void setTopP(double p) = 0;
    virtual void setTopK(int k) {
    }
    virtual void setPresencePenalty(double p) {
    }
    virtual void setFrequencyPenalty(double p) {
    }
    virtual void setQuietLogs(bool) {
    }

    // Getters
    virtual std::string getModel() const = 0;
    virtual double getTemperature() const = 0;
    virtual int getMaxTokens() const = 0;
    virtual std::string providerName() const = 0;

    // Model listing
    struct ModelInfo {
        std::string id;
        std::string name;
        int contextWindow = 272000;
        bool isFree = false;
        bool supportsTopK = true;
    };
    virtual std::vector<ModelInfo> listModels() {
        return {};
    }

    // Stream diagnostics — populated after each generateStream call so the
    // agent loop can distinguish a normal empty response from a model that
    // silently returned nothing (refusal, filter, or dead upstream).
    struct StreamStats {
        bool anyContent = false;
        std::string finishReason;
        std::string lastError;
        long httpStatus = 0;
    };
    virtual StreamStats lastStreamStats() const {
        return {};
    }

};

using LlmProviderPtr = std::shared_ptr<ILlmProvider>;

}  // namespace cortex::mk3
