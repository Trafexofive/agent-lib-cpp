// src/core/output_sink.hpp — Output interface for agent rendering
// Library-first: agent emits to this interface, TUI implements it
// No TUI dependency in core agent
#pragma once

#include <string>
#include <vector>
#include <functional>

namespace cortex::mk3::core {

// Abstract output sink — agent feeds events, implementation decides how to display
struct OutputSink {
    virtual ~OutputSink() = default;

    // Called when an action starts (before execution)
    virtual void onActionStart(const std::string& type, const std::string& name,
                               const std::string& id, bool sync) = 0;

    // Called when an action result is ready
    virtual void onActionResult(const std::string& id, bool ok,
                                const std::string& summary) = 0;

    // Called for each response text token (streaming)
    virtual void onResponseToken(const std::string& token) = 0;

    // Called when response is complete
    virtual void onResponseComplete() = 0;

    // Called for thought content
    virtual void onThought(const std::string& text) = 0;

    // Raw pass-through mode
    virtual void onRawToken(const std::string& token) = 0;

    // Get lines to display (for TUI rendering)
    virtual std::vector<std::string> flush() = 0;

    // Reset for new turn
    virtual void reset() = 0;

    // Toggle raw mode
    virtual void setRaw(bool v) = 0;
    virtual bool raw() const = 0;
};

// Null sink — discards all output (library default)
struct NullSink : OutputSink {
    void onActionStart(const std::string&, const std::string&,
                       const std::string&, bool) override {}
    void onActionResult(const std::string&, bool, const std::string&) override {}
    void onResponseToken(const std::string&) override {}
    void onResponseComplete() override {}
    void onThought(const std::string&) override {}
    void onRawToken(const std::string&) override {}
    std::vector<std::string> flush() override { return {}; }
    void reset() override {}
    void setRaw(bool) override {}
    bool raw() const override { return false; }
};

} // namespace cortex::mk3::core
