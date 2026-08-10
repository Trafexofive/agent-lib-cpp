#pragma once
// =============================================================================
// agent-lib-MK3 — Streaming Protocol Parser
// XML-based: <action>, <response>, <thought>, <result>
// Leaner than MK2 but same protocol semantics.
// =============================================================================

#include <json/json.h>

#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "../core/types.hpp"

namespace cortex::mk3::protocol {

// ---------------------------------------------------------------------------
// Action types
// ---------------------------------------------------------------------------
enum class ActionType { TOOL, AGENT, RELIC, FEED, WORKFLOW, LLM_CALL, INTERNAL };
enum class ExecutionMode { SYNC, ASYNC, FIRE_AND_FORGET };

struct ParsedAction {
    std::string id;
    ActionType type = ActionType::TOOL;
    ExecutionMode mode = ExecutionMode::SYNC;
    std::string name;
    Json::Value params;
    std::string content;  // text-only body (non-JSON, used by agent/relic)
    std::vector<std::string> dependsOn;
    int timeout = 0;
};

struct ParsedResponse {
    std::string content;
    bool isFinal = false;
};

// ---------------------------------------------------------------------------
// Token events emitted during parsing
// ---------------------------------------------------------------------------
struct TokenEvent {
    enum Type { TEXT, THOUGHT, ACTION_START, ACTION_RESULT, RESPONSE, CONTEXT_FEED, ERROR };
    Type type;
    std::string content;
    std::shared_ptr<ParsedAction> action;
    std::map<std::string, std::string> metadata;
};

// ---------------------------------------------------------------------------
// Action executor callback — runtime provides this
// ---------------------------------------------------------------------------
using ActionExecutor = std::function<Json::Value(const ParsedAction&)>;
using EventCallback = std::function<void(const TokenEvent&)>;

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
class Parser {
   public:
    explicit Parser(ActionExecutor executor = nullptr);
    ~Parser();

    // Feed tokens from LLM stream
    void feed(const std::string& token, bool isFinal = false);
    void flush();

    // Callbacks
    void onEvent(EventCallback cb) {
        eventCb_ = std::move(cb);
    }
    void setExecutor(ActionExecutor ex) {
        executor_ = std::move(ex);
    }

    // Inject a result (called by runtime when action completes)
    void injectResult(const std::string& id, const Json::Value& result);

    // Wait for pending async actions (with optional deadline). Returns false on timeout.
    bool waitForActions(std::chrono::seconds deadline = std::chrono::seconds(0));

    // Query results
    Json::Value getResult(const std::string& id) const;
    std::map<std::string, Json::Value> allResults() const;
    void clearResults();  // clear accumulated results (call between iterations)

    // Context feeds (for prompt injection)
    std::vector<std::string> contextFeeds() const {
        return contextFeeds_;
    }
    void clearContextFeeds() {
        contextFeeds_.clear();
    }

    // Reset for new iteration
    void reset();

   private:
    // Core parse loop — processes buffer looking for tags
    void processBuffer();
    void compactBuffer();

    // Tag detection
    size_t findNextTag();
    std::string identifyTag(size_t tagStart);
    size_t findClosingTag(const std::string& tagName, size_t contentStart);
    size_t findResponseClose(size_t contentStart) const;
    std::map<std::string, std::string> parseAttrs(const std::string& tagContent);

    // Handlers
    void handleThought(const std::string& content);
    void handleAction(const std::string& content, const std::map<std::string, std::string>& attrs);
    void handleResponse(const std::string& content,
                        const std::map<std::string, std::string>& attrs);
    void handleResult(const std::string& content, const std::map<std::string, std::string>& attrs);
    void handleContextFeed(const std::string& content,
                           const std::map<std::string, std::string>& attrs);

    // Action execution
    void executeAction(std::shared_ptr<ParsedAction> action);
    bool canExecute(const ParsedAction& action) const;
    void dispatchPending();
    std::shared_ptr<ParsedAction> buildAction(const std::string& json,
                                              const std::map<std::string, std::string>& attrs);

    // Variable resolution — ${id} and ${id.field}
    std::string resolveVars(const std::string& input) const;
    Json::Value resolveVars(const Json::Value& input) const;

    // JSON helpers
    std::string trimJson(const std::string& raw);
    std::string cleanJson(const std::string& raw);  // trim only — no silent repair
    std::string completeJson(const std::string& raw);  // retained for tests/debug; not used on action bodies
    bool isCompleteJson(const std::string& s);

    // Enum parsers
    ExecutionMode parseMode(const std::string& s);
    ActionType parseType(const std::string& s);

    // Emit event
    void emit(const TokenEvent& ev);

    // Members
    ActionExecutor executor_;
    EventCallback eventCb_;

    std::string buffer_;
    size_t readPos_ = 0;
    // Length of the most recent close marker matched by findClosingTag.
    // Needed by the caller to compute closingTagStart correctly when the
    // close is a synonym (e.g. </think> for <thought>).
    size_t lastCloseLen_ = 0;
    std::string closingScanTag_;
    size_t closingScanContentStart_ = std::string::npos;
    size_t closingScanPos_ = 0;
    int closingScanDepth_ = 1;
    bool closingScanInString_ = false;
    bool closingScanEscape_ = false;
    bool inResponse_ = false;  // true between <response> and </response> for streaming
    size_t responseContentStart_ =
        0;  // start of active response body for code-span-aware close scans
    bool inThought_ = false;  // stream thought body as tokens arrive (same as response)
    size_t thoughtContentStart_ = 0;
    bool finalResponseSeen_ = false;  // true after first <response final="true">
    int forgedResultCount_ = 0;       // LLM-emitted <result> tags ignored by runtime
    std::map<std::string, std::string>
        responseAttrs_;  // attrs from opening <response>, applied when streaming closes
    // At most one incomplete action while its body streams — stable id for UI card.
    bool hasProvisionalAction_ = false;
    std::string provisionalActionId_;

    // Results
    std::map<std::string, Json::Value> results_;
    std::map<std::string, bool> completed_;

    // Protocol enforcement — track used action IDs
    std::unordered_set<std::string> usedActionIds_;
    // Retained last-result per used id — survives clearResults() so a model
    // that re-emits an already-run action id gets an idempotent replay instead
    // of a duplicate-id error (prevents a non-recovering stall).
    std::map<std::string, Json::Value> retainedResults_;

    // Context feeds (accumulated for prompt injection)
    std::vector<std::string> contextFeeds_;

    // Pending actions (waiting on dependsOn)
    std::vector<std::shared_ptr<ParsedAction>> pending_;

    // Async futures
    std::vector<std::future<void>> futures_;
    mutable std::mutex mtx_;

    int idCounter_ = 0;
};

}  // namespace cortex::mk3::protocol
