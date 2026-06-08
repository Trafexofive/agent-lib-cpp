#pragma once
// =============================================================================
// agent-lib-MK3 — TokenEstimate — Rough token counting for prompt budgeting
// Ported from MK2 (src/utils/TokenEstimate.hpp)
// =============================================================================
// OpenAI-style tokenization: ~4 chars per token for ASCII English, ~1.5 for CJK.
// This is approximate — NOT a BPE tokenizer. Good for context budgeting,
// not precise enough for billing.

#include <string>
#include <cctype>
#include <cstddef>

namespace cortex::mk3::utils {

/// Estimate token count for a string. Returns >= 1 for any non-empty text.
inline size_t estimateTokens(const std::string& text) {
    if (text.empty()) return 0;

    size_t cjk = 0;
    size_t ascii = 0;

    for (size_t i = 0; i < text.size(); ) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            ascii++;
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            // 2-byte UTF-8
            if ((c & 0x1F) >= 0x0E && (c & 0x1F) <= 0x0F) cjk++; // CJK ranges
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            // 3-byte UTF-8 — likely CJK
            cjk++;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            // 4-byte UTF-8 — emoji, rare chars
            cjk += 2;
            i += 4;
        } else {
            ascii++;
            i++;
        }
    }

    // ASCII: ~4 chars per token, CJK: ~1.5 chars per token
    size_t asciiTokens = ascii / 4 + (ascii % 4 ? 1 : 0);
    size_t cjkTokens = cjk;

    // +10% overhead for special tokens, formatting
    size_t total = asciiTokens + cjkTokens;
    total = total * 11 / 10;

    return total > 0 ? total : 1;
}

/// Token budget for context window management.
struct TokenBudget {
    size_t windowSize = 128000;   // Total context window
    size_t reserved = 4096;       // Reserved for response generation
    size_t used = 0;              // Current prompt tokens used

    /// Tokens remaining for prompt content.
    size_t available() const {
        size_t maxPrompt = windowSize > reserved ? windowSize - reserved : 0;
        return used < maxPrompt ? maxPrompt - used : 0;
    }

    /// True if prompt exceeds the budget.
    bool exceeds() const {
        size_t maxPrompt = windowSize > reserved ? windowSize - reserved : 0;
        return used > maxPrompt;
    }

    /// Fraction of window used (0.0–1.0). >1.0 means over budget.
    double utilization() const {
        size_t maxPrompt = windowSize > reserved ? windowSize - reserved : 1;
        return static_cast<double>(used) / static_cast<double>(maxPrompt);
    }
};

} // namespace cortex::mk3::utils
