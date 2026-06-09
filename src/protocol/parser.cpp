// =============================================================================
// agent-lib-MK3 — Streaming Protocol Parser Implementation
// =============================================================================

#include "parser.hpp"
#include <sstream>
#include <iostream>
#include <regex>
#include <thread>

namespace cortex::mk3::protocol {

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
Parser::Parser(ActionExecutor executor) : executor_(std::move(executor)) {}

Parser::~Parser() {
    waitForActions();
}

// ---------------------------------------------------------------------------
// Feed tokens
// ---------------------------------------------------------------------------
void Parser::feed(const std::string& token, bool isFinal) {
    buffer_ += token;
    processBuffer();

    if (isFinal) flush();
}

void Parser::flush() {
    // Emit any remaining plain text
    if (readPos_ < buffer_.size()) {
        std::string remaining = buffer_.substr(readPos_);
        // Strip any stray tags
        static const std::regex tagRe(R"(<\/?[^>]+>)");
        remaining = std::regex_replace(remaining, tagRe, "");
        if (!remaining.empty()) {
            emit({TokenEvent::TEXT, remaining, {}, {}});
        }
        readPos_ = buffer_.size();
    }
}

// ---------------------------------------------------------------------------
// Core parse loop
// ---------------------------------------------------------------------------
void Parser::processBuffer() {
    while (true) {
        size_t tagStart = findNextTag();
        if (tagStart == std::string::npos) {
            // No more tags — if inside <response>, emit remaining content as stream
            if (inResponse_ && readPos_ < buffer_.size()) {
                std::string partial = buffer_.substr(readPos_);
                readPos_ = buffer_.size();
                if (!partial.empty()) {
                    emit({TokenEvent::RESPONSE, partial, {}, {}});
                }
            }
            return;
        }

        // Emit plain text before this tag (as RESPONSE if inside <response>, else TEXT)
        if (tagStart > readPos_) {
            std::string text = buffer_.substr(readPos_, tagStart - readPos_);
            if (inResponse_) {
                emit({TokenEvent::RESPONSE, text, {}, {}});
            } else {
                emit({TokenEvent::TEXT, text, {}, {}});
            }
            readPos_ = tagStart;
        }

        // Wait until we have a complete opening tag (<...>)
        size_t gt = buffer_.find('>', readPos_);
        if (gt == std::string::npos) return;

        // Identify the tag
        std::string tagName = identifyTag(readPos_);
        if (tagName.empty()) {
            // Unknown tag — check if it's a closing </tag>
            size_t nameStart = readPos_ + 1;
            // Skip leading '/' for closing tags
            if (nameStart < buffer_.size() && buffer_[nameStart] == '/') {
                nameStart++;
            }
            size_t nameEnd = buffer_.find_first_of(" >/", nameStart);
            if (nameEnd != std::string::npos) {
                std::string raw = buffer_.substr(nameStart, nameEnd - nameStart);
                if (raw == "response" && inResponse_) {
                    inResponse_ = false;
                    // Remap raw attrs to metadata keys (matching handleResponse)
                    TokenEvent ev{TokenEvent::RESPONSE, "", {}, {}};
                    auto fit = responseAttrs_.find("final");
                    if (fit != responseAttrs_.end()) {
                        ev.metadata["is_final"] = fit->second;
                    }
                    emit(ev);
                    responseAttrs_.clear();
                }
            }
            // Skip past the entire closing tag
            readPos_ = gt + 1;
            continue;
        }

        // Check for self-closing tag (/>)
        bool selfClosing = (gt > readPos_ && buffer_[gt-1] == '/');

        std::string openingTag, content;
        size_t contentStart;
        size_t closingPos = 0;

        if (selfClosing) {
            // <tag ... /> — no content, tag ends at />
            openingTag = buffer_.substr(readPos_ + 1, gt - readPos_ - 2); // between < and />
            contentStart = gt + 1;
            content = "";
        } else if (tagName == "response") {
            // STREAMING: emit partial response content as it arrives
            contentStart = gt + 1;
            closingPos = findClosingTag(tagName, contentStart);
            // Save attrs from opening tag for final emission
            openingTag = buffer_.substr(readPos_ + 1, contentStart - readPos_ - 2);
            responseAttrs_ = parseAttrs(openingTag);
            inResponse_ = true;
            if (closingPos == std::string::npos) {
                // </response> not arrived yet — emit whatever content we have
                std::string partial = buffer_.substr(contentStart);
                readPos_ = buffer_.size();
                if (!partial.empty()) {
                    emit({TokenEvent::RESPONSE, partial, {}, responseAttrs_});
                }
                return; // Wait for more data
            }
            // </response> found — emit final chunk
            inResponse_ = false;
            size_t closingTagStart = closingPos - (tagName.length() + 3);
            content = buffer_.substr(contentStart, closingTagStart - contentStart);
            readPos_ = closingPos;  // skip past </response>
        } else {
            closingPos = findClosingTag(tagName, gt + 1);
            if (closingPos == std::string::npos) return; // Tag not closed yet

            contentStart = gt + 1;
            size_t closingTagStart = closingPos - (tagName.length() + 3); // back to < of </tagName>
            openingTag = buffer_.substr(readPos_ + 1, contentStart - readPos_ - 2);
            content = buffer_.substr(contentStart, closingTagStart - contentStart);
        }

        auto attrs = parseAttrs(openingTag);

        // Dispatch to handler
        if (tagName == "thought")      handleThought(content);
        else if (tagName == "action")  handleAction(content, attrs);
        else if (tagName == "response") handleResponse(content, attrs);
        else if (tagName == "result")  handleResult(content, attrs);
        else if (tagName == "context_feed") handleContextFeed(content, attrs);

        // Advance past closing tag
        if (selfClosing) {
            readPos_ = contentStart; // past />
        } else {
            readPos_ = closingPos; // past </tagName>
        }
    }
}

// ---------------------------------------------------------------------------
// Find next '<' in buffer
// ---------------------------------------------------------------------------
size_t Parser::findNextTag() {
    return buffer_.find('<', readPos_);
}

// ---------------------------------------------------------------------------
// Identify tag name from opening tag — only when full <...> is present
// ---------------------------------------------------------------------------
std::string Parser::identifyTag(size_t tagStart) {
    // Extract tag name: chars between '<' and first space or '>'
    size_t nameStart = tagStart + 1;
    size_t nameEnd = buffer_.find_first_of(" >/", nameStart);
    std::string tagName = buffer_.substr(nameStart, nameEnd - nameStart);

    static const std::vector<std::string> known = {
        "thought", "action", "response", "result", "context_feed"
    };
    for (auto& k : known) {
        if (tagName == k) return tagName;
    }
    return "";
}

// ---------------------------------------------------------------------------
// Find closing tag
// ---------------------------------------------------------------------------
size_t Parser::findClosingTag(const std::string& tagName, size_t contentStart) {
    std::string closingTag = "</" + tagName + ">";
    size_t pos = buffer_.find(closingTag, contentStart);
    if (pos != std::string::npos) return pos + closingTag.length(); // past </tagName>

    // Handle self-closing: <tag ... />
    size_t selfClose = buffer_.find("/>", contentStart);
    size_t nextOpen = buffer_.find("<" + tagName, contentStart);

    if (selfClose != std::string::npos) {
        if (nextOpen == std::string::npos || selfClose < nextOpen) {
            return selfClose + 2; // past />
        }
    }

    return std::string::npos;
}

// ---------------------------------------------------------------------------
// Parse XML attributes
// ---------------------------------------------------------------------------
std::map<std::string, std::string> Parser::parseAttrs(const std::string& tagContent) {
    std::map<std::string, std::string> attrs;
    std::regex attrRe("(\\w+)\\s*=\\s*\"([^\"]*)\"?");

    auto begin = std::sregex_iterator(tagContent.begin(), tagContent.end(), attrRe);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        attrs[(*it)[1]] = (*it)[2];
    }

    return attrs;
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------
void Parser::handleThought(const std::string& content) {
    emit({TokenEvent::THOUGHT, content, {}, {}});
}

void Parser::handleAction(const std::string& content,
                           const std::map<std::string, std::string>& attrs) {
    // Enforce: no actions after <response final="true">
    if (finalResponseSeen_) {
        emit({TokenEvent::THOUGHT, "[post-final action ignored]", {}, {}});
        return;
    }
    auto action = buildAction(content, attrs);
    if (!action) return;

    // Enforce: reject duplicate action IDs
    if (usedActionIds_.count(action->id)) {
        // Inject error result so model sees the failure
        Json::Value err;
        err["error"] = "duplicate action id: " + action->id
                        + " \u2014 each action must have a unique id";
        results_[action->id] = err;
        completed_[action->id] = true;
        emit({TokenEvent::ACTION_RESULT,
              Json::writeString(Json::StreamWriterBuilder(), err),
              nullptr, {{"id", action->id}}});
        emit({TokenEvent::ERROR, "duplicate id: " + action->id, nullptr,
              {{"id", action->id}, {"reason", "duplicate_action_id"}}});
        return;
    }
    usedActionIds_.insert(action->id);

    emit({TokenEvent::ACTION_START, "", action, {}});

    // Execute
    executeAction(action);
}

void Parser::handleResponse(const std::string& content,
                             const std::map<std::string, std::string>& attrs) {
    // Enforce: no content after first <response final="true">
    if (finalResponseSeen_) {
        emit({TokenEvent::THOUGHT, "[post-final ignored] " + content, {}, {}});
        return;
    }
    TokenEvent ev{TokenEvent::RESPONSE, content, {}, {}};
    auto it = attrs.find("final");
    if (it != attrs.end()) {
        ev.metadata["is_final"] = it->second;
        if (it->second == "true") {
            finalResponseSeen_ = true;
        }
    }
    emit(ev);
}

void Parser::handleResult(const std::string& content,
                           const std::map<std::string, std::string>& attrs) {
    // Results are injected by the runtime, not from LLM output.
    // If the LLM emits <result> tags directly, parse them.
    auto idIt = attrs.find("id");
    if (idIt == attrs.end()) return;

    Json::Value val;
    Json::CharReaderBuilder reader;
    std::string errs;
    std::istringstream ss(content);
    if (Json::parseFromStream(reader, ss, &val, &errs)) {
        results_[idIt->second] = val;
        completed_[idIt->second] = true;
        emit({TokenEvent::ACTION_RESULT, content, nullptr, {{"id", idIt->second}}});
    }
}

void Parser::handleContextFeed(const std::string& content,
                                const std::map<std::string, std::string>& attrs) {
    contextFeeds_.push_back(content);
    emit({TokenEvent::CONTEXT_FEED, content, {}, attrs});
}

// ---------------------------------------------------------------------------
// Build action from JSON body + attrs
// ---------------------------------------------------------------------------
std::shared_ptr<ParsedAction> Parser::buildAction(
    const std::string& json, const std::map<std::string, std::string>& attrs) {

    auto action = std::make_shared<ParsedAction>();

    // Parse type
    auto typeIt = attrs.find("type");
    action->type = (typeIt != attrs.end()) ? parseType(typeIt->second) : ActionType::TOOL;

    // Parse mode
    auto modeIt = attrs.find("mode");
    action->mode = (modeIt != attrs.end()) ? parseMode(modeIt->second) : ExecutionMode::SYNC;

    // Name
    auto nameIt = attrs.find("name");
    if (nameIt != attrs.end()) action->name = nameIt->second;

    // ID
    auto idIt = attrs.find("id");
    if (idIt != attrs.end()) {
        action->id = idIt->second;
    } else {
        action->id = "__auto_" + std::to_string(++idCounter_);
    }

    // Timeout
    auto timeoutIt = attrs.find("timeout");
    if (timeoutIt != attrs.end()) action->timeout = std::stoi(timeoutIt->second);

    // Depends on
    auto depIt = attrs.find("depends_on");
    if (depIt != attrs.end()) {
        std::string deps = depIt->second;
        std::regex commaRe(",\\s*");
        std::sregex_token_iterator it(deps.begin(), deps.end(), commaRe, -1);
        std::sregex_token_iterator end;
        for (; it != end; ++it) action->dependsOn.push_back(*it);
    }

    // Parse JSON body
    std::string cleaned = cleanJson(json);
    Json::Value params;
    Json::CharReaderBuilder reader;
    std::string errs;
    std::istringstream ss(cleaned);
    if (Json::parseFromStream(reader, ss, &params, &errs)) {
        action->params = resolveVars(params);
    } else if (!json.empty()) {
        // Not JSON — treat as raw text content (used by agent/relic text-only calls)
        action->content = json;
    }

    return action;
}

// ---------------------------------------------------------------------------
// Execute action
// ---------------------------------------------------------------------------
void Parser::executeAction(std::shared_ptr<ParsedAction> action) {
    if (!executor_) return;

    if (!canExecute(*action)) {
        pending_.push_back(action);
        return;
    }

    auto doExecute = [this](std::shared_ptr<ParsedAction> a) {
        Json::Value result = executor_(*a);

        std::lock_guard<std::mutex> lock(mtx_);
        results_[a->id] = result;
        completed_[a->id] = true;

        emit({TokenEvent::ACTION_RESULT, Json::writeString(Json::StreamWriterBuilder(), result),
              nullptr, {{"id", a->id}}});
    };

    switch (action->mode) {
    case ExecutionMode::SYNC:
        doExecute(action);
        break;
    case ExecutionMode::ASYNC: {
        std::lock_guard<std::mutex> lock(mtx_);
        futures_.push_back(std::async(std::launch::async, doExecute, action));
        break;
    }
    case ExecutionMode::FIRE_AND_FORGET:
        std::thread(doExecute, action).detach();
        break;
    }

    dispatchPending();
}

// ---------------------------------------------------------------------------
// Check if action can execute (dependencies satisfied)
// ---------------------------------------------------------------------------
bool Parser::canExecute(const ParsedAction& action) const {
    for (const auto& dep : action.dependsOn) {
        auto it = completed_.find(dep);
        if (it == completed_.end() || !it->second) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Dispatch pending actions whose dependencies are now met
// ---------------------------------------------------------------------------
void Parser::dispatchPending() {
    // Note: lock must NOT be held when calling this
    std::vector<std::shared_ptr<ParsedAction>> ready;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = pending_.begin();
        while (it != pending_.end()) {
            if (canExecute(**it)) {
                ready.push_back(*it);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto& action : ready) {
        executeAction(action);
    }
}

// ---------------------------------------------------------------------------
// Inject result
// ---------------------------------------------------------------------------
void Parser::injectResult(const std::string& id, const Json::Value& result) {
    std::lock_guard<std::mutex> lock(mtx_);
    results_[id] = result;
    completed_[id] = true;
    emit({TokenEvent::ACTION_RESULT,
          Json::writeString(Json::StreamWriterBuilder(), result),
          nullptr, {{"id", id}}});
    dispatchPending();
}

// ---------------------------------------------------------------------------
// Wait for async actions
// ---------------------------------------------------------------------------
bool Parser::waitForActions(std::chrono::seconds deadline) {
    std::vector<std::future<void>> futs;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        futs = std::move(futures_);
    }
    for (auto& f : futs) {
        if (!f.valid()) continue;
        if (deadline.count() > 0) {
            auto status = f.wait_for(deadline);
            if (status == std::future_status::timeout) return false;
        } else {
            f.wait();
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Result queries
// ---------------------------------------------------------------------------
Json::Value Parser::getResult(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = results_.find(id);
    return (it != results_.end()) ? it->second : Json::Value();
}

std::map<std::string, Json::Value> Parser::allResults() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return results_;
}

void Parser::clearResults() {
    std::lock_guard<std::mutex> lock(mtx_);
    results_.clear();
    completed_.clear();
    usedActionIds_.clear();
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------
void Parser::reset() {
    std::lock_guard<std::mutex> lock(mtx_);
    buffer_.clear();
    readPos_ = 0;
    results_.clear();
    completed_.clear();
    pending_.clear();
    contextFeeds_.clear();
    idCounter_ = 0;
    finalResponseSeen_ = false;
    usedActionIds_.clear();
    responseAttrs_.clear();
}

// ---------------------------------------------------------------------------
// Variable resolution — ${id} and ${id.field.subfield}
// ---------------------------------------------------------------------------
std::string Parser::resolveVars(const std::string& input) const {
    std::string result = input;
    std::regex varRe(R"(\$\{(\w+(?:\.\w+)*)\})");

    std::smatch match;
    std::string temp = result;
    while (std::regex_search(temp, match, varRe)) {
        std::string path = match[1].str();
        std::string replacement;

        // Split path by '.'
        size_t dot = path.find('.');
        std::string id = path.substr(0, dot);

        auto it = results_.find(id);
        if (it != results_.end()) {
            Json::Value val = it->second;
            if (dot != std::string::npos) {
                // Navigate sub-fields
                std::string remaining = path.substr(dot + 1);
                std::stringstream ss(remaining);
                std::string part;
                while (std::getline(ss, part, '.') && !val.isNull()) {
                    if (val.isObject()) {
                        val = val.get(part, Json::Value());
                    } else if (val.isArray()) {
                        try {
                            int idx = std::stoi(part);
                            if (idx >= 0 && idx < (int)val.size())
                                val = val[idx];
                            else
                                val = Json::Value();
                        } catch (...) {
                            val = Json::Value();
                        }
                    } else {
                        val = Json::Value();
                    }
                }
            }
            if (val.isString()) replacement = val.asString();
            else if (!val.isNull()) replacement = Json::writeString(Json::StreamWriterBuilder(), val);
        }

        result.replace(match.position(), match.length(), replacement);
        temp = result;
    }

    return result;
}

Json::Value Parser::resolveVars(const Json::Value& input) const {
    if (input.isString()) {
        std::string resolved = resolveVars(input.asString());
        // Try parsing as JSON if it looks like it
        if (!resolved.empty() && (resolved[0] == '{' || resolved[0] == '[')) {
            Json::Value parsed;
            Json::CharReaderBuilder reader;
            std::string errs;
            std::istringstream ss(resolved);
            if (Json::parseFromStream(reader, ss, &parsed, &errs))
                return parsed;
        }
        return resolved;
    }
    if (input.isArray()) {
        Json::Value arr(Json::arrayValue);
        for (auto& v : input) arr.append(resolveVars(v));
        return arr;
    }
    if (input.isObject()) {
        Json::Value obj(Json::objectValue);
        for (auto& k : input.getMemberNames())
            obj[k] = resolveVars(input[k]);
        return obj;
    }
    return input;
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------
std::string Parser::cleanJson(const std::string& raw) {
    // Trim whitespace
    size_t start = raw.find_first_not_of(" \t\n\r");
    size_t end = raw.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return "{}";
    std::string s = raw.substr(start, end - start + 1);
    return completeJson(s);
}

std::string Parser::completeJson(const std::string& raw) {
    if (raw.empty()) return "{}";
    if (isCompleteJson(raw)) return raw;

    // Count braces to auto-close
    int braceCount = 0;
    int bracketCount = 0;
    bool inString = false;
    bool escaped = false;

    for (char c : raw) {
        if (escaped) { escaped = false; continue; }
        if (c == '\\') { escaped = true; continue; }
        if (c == '"') { inString = !inString; continue; }
        if (inString) continue;
        if (c == '{') braceCount++;
        if (c == '}') braceCount--;
        if (c == '[') bracketCount++;
        if (c == ']') bracketCount--;
    }

    std::string result = raw;
    for (int i = 0; i < bracketCount; i++) result += "]";
    for (int i = 0; i < braceCount; i++) result += "}";

    return result;
}

bool Parser::isCompleteJson(const std::string& s) {
    Json::Value v;
    Json::CharReaderBuilder reader;
    std::string errs;
    std::istringstream ss(s);
    return Json::parseFromStream(reader, ss, &v, &errs);
}

// ---------------------------------------------------------------------------
// Enum parsers
// ---------------------------------------------------------------------------
ExecutionMode Parser::parseMode(const std::string& s) {
    if (s == "async") return ExecutionMode::ASYNC;
    if (s == "fire_and_forget" || s == "detached") return ExecutionMode::FIRE_AND_FORGET;
    return ExecutionMode::SYNC;
}

ActionType Parser::parseType(const std::string& s) {
    if (s == "agent") return ActionType::AGENT;
    if (s == "relic") return ActionType::RELIC;
    if (s == "feed") return ActionType::FEED;
    if (s == "llm" || s == "llm_call") return ActionType::LLM_CALL;
    if (s == "internal") return ActionType::INTERNAL;
    return ActionType::TOOL;
}

// ---------------------------------------------------------------------------
// Emit event
// ---------------------------------------------------------------------------
void Parser::emit(const TokenEvent& ev) {
    if (eventCb_) eventCb_(ev);
}

} // namespace cortex::mk3::protocol
