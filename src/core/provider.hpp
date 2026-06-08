#pragma once
// =============================================================================
// agent-lib-MK3 — LLM Provider Interface
// Single clean interface. All providers implement this.
// =============================================================================

#include "types.hpp"
#include <string>
#include <memory>

namespace cortex::mk3 {

class ILlmProvider {
public:
    virtual ~ILlmProvider() = default;

    // Generate a complete response (non-streaming)
    virtual std::string generate(const ChatMessages& msgs) = 0;

    // Generate a streaming response
    virtual void generateStream(const ChatMessages& msgs, StreamCallback cb) = 0;

    // Configuration
    virtual void setModel(const std::string& model) = 0;
    virtual void setTemperature(double t) = 0;
    virtual void setMaxTokens(int n) = 0;
    virtual void setTopP(double p) = 0;
    virtual void setTopK(int k) {}
    virtual void setPresencePenalty(double p) {}
    virtual void setFrequencyPenalty(double p) {}

    // Getters
    virtual std::string getModel() const = 0;
    virtual double getTemperature() const = 0;
    virtual int getMaxTokens() const = 0;
    virtual std::string providerName() const = 0;

    // Model listing
    struct ModelInfo {
        std::string id;
        std::string name;
        int contextWindow = 8192;
        bool isFree = false;
    };
    virtual std::vector<ModelInfo> listModels() { return {}; }
};

using LlmProviderPtr = std::shared_ptr<ILlmProvider>;

} // namespace cortex::mk3
