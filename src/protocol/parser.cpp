// =============================================================================
// agent-lib-MK3 — Streaming Protocol Parser Implementation
// =============================================================================

#include "parser.hpp"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <regex>
#include <sstream>
#include <thread>

#include "src/core/run_control.hpp"  // runIsActive / g_running

namespace cortex::mk3::protocol {

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
Parser::Parser(ActionExecutor executor) : executor_(std::move(executor)) {
}

Parser::~Parser() {
    waitForActions();
}

// ---------------------------------------------------------------------------
// Feed tokens
// ---------------------------------------------------------------------------
void Parser::feed(const std::string& token, bool isFinal) {
    buffer_ += token;
    processBuffer();
    compactBuffer();

    if (isFinal)
        flush();
}

void Parser::flush() {
    if (readPos_ >= buffer_.size()) return;
    std::string remaining;
    remaining.reserve(buffer_.size() - readPos_);
    size_t cursor = readPos_;
    while (cursor < buffer_.size()) {
        size_t lt = buffer_.find('<', cursor);
        if (lt == std::string::npos) {
            remaining.append(buffer_, cursor, std::string::npos);
            break;
        }
        remaining.append(buffer_, cursor, lt - cursor);
        size_t gt = buffer_.find('>', lt + 1);
        if (gt == std::string::npos) {
            remaining.append(buffer_, lt, std::string::npos);
            break;
        }
        cursor = gt + 1;
    }
    if (!remaining.empty()) emit({TokenEvent::TEXT, std::move(remaining), {}, {}});
    readPos_ = buffer_.size();
    compactBuffer();
}

void Parser::compactBuffer() {
    constexpr size_t kCompactThreshold = 64 * 1024;
    if (readPos_ < kCompactThreshold) return;
    size_t consumed = readPos_;
    buffer_.erase(0, consumed);
    readPos_ = 0;
    responseContentStart_ = responseContentStart_ >= consumed ? responseContentStart_ - consumed : 0;
    thoughtContentStart_ = thoughtContentStart_ >= consumed ? thoughtContentStart_ - consumed : 0;
    if (closingScanContentStart_ != std::string::npos) {
        closingScanContentStart_ = closingScanContentStart_ >= consumed
                                       ? closingScanContentStart_ - consumed
                                       : 0;
        closingScanPos_ = closingScanPos_ >= consumed ? closingScanPos_ - consumed : 0;
    }
}

// ---------------------------------------------------------------------------
// Core parse loop
// ---------------------------------------------------------------------------
void Parser::processBuffer() {
    // Hold back a suffix that might be a partial closing tag across token boundaries.
    auto holdPartialClose = [](const std::string& closeMarker, size_t emitEnd, size_t readPos,
                               const std::string& buffer) -> size_t {
        if (emitEnd <= readPos) return emitEnd;
        size_t maxKeep = std::min(closeMarker.size() - 1, emitEnd - readPos);
        for (size_t keep = maxKeep; keep > 0; --keep) {
            if (buffer.compare(emitEnd - keep, keep, closeMarker, 0, keep) == 0)
                return emitEnd - keep;
        }
        return emitEnd;
    };

    while (true) {
        // Stream <thought>/<think>/<thinking> body as it arrives — paint the
        // Thought card from the first closed-open, not after the whole block.
        if (inThought_) {
            static const std::vector<std::string> closes = {"</thought>", "</think>",
                                                           "</thinking>"};
            size_t closePos = std::string::npos;
            size_t closeLen = 0;
            for (const auto& m : closes) {
                size_t p = buffer_.find(m, readPos_);
                if (p != std::string::npos && (closePos == std::string::npos || p < closePos)) {
                    closePos = p;
                    closeLen = m.size();
                }
            }
            size_t emitEnd = (closePos == std::string::npos) ? buffer_.size() : closePos;
            if (closePos == std::string::npos) {
                // Prefer holding against the longest possible partial close.
                size_t held = emitEnd;
                for (const auto& m : closes)
                    held = std::min(held, holdPartialClose(m, emitEnd, readPos_, buffer_));
                emitEnd = held;
            }
            if (emitEnd > readPos_) {
                emit({TokenEvent::THOUGHT, buffer_.substr(readPos_, emitEnd - readPos_), {}, {}});
            }
            if (closePos == std::string::npos) {
                readPos_ = emitEnd;
                return;
            }
            readPos_ = closePos + closeLen;
            inThought_ = false;
            continue;
        }

        // Once inside <response>, treat everything as user-visible text until
        // the real closing </response>. Literal protocol examples like
        // `<response>` or `<action ...>` inside markdown/code spans must not be
        // parsed as nested runtime tags; otherwise streaming stalls waiting for
        // fake closing tags.
        if (inResponse_) {
            const std::string closeMarker = "</response>";
            size_t closePos = findResponseClose(readPos_);
            size_t emitEnd = (closePos == std::string::npos) ? buffer_.size() : closePos;
            if (closePos == std::string::npos)
                emitEnd = holdPartialClose(closeMarker, emitEnd, readPos_, buffer_);
            if (emitEnd > readPos_) {
                emit({TokenEvent::RESPONSE, buffer_.substr(readPos_, emitEnd - readPos_), {}, {}});
            }
            if (closePos == std::string::npos) {
                readPos_ = emitEnd;
                return;
            }

            readPos_ = closePos + closeMarker.size();
            inResponse_ = false;
            TokenEvent ev{TokenEvent::RESPONSE, "", {}, {}};
            auto fit = responseAttrs_.find("final");
            if (fit != responseAttrs_.end())
                ev.metadata["is_final"] = fit->second;
            emit(ev);
            responseAttrs_.clear();
            continue;
        }

        size_t tagStart = findNextTag();
        if (tagStart == std::string::npos) {
            // No more tags — drain any open stream bodies.
            if (inResponse_ && readPos_ < buffer_.size()) {
                std::string partial = buffer_.substr(readPos_);
                readPos_ = buffer_.size();
                if (!partial.empty())
                    emit({TokenEvent::RESPONSE, partial, {}, {}});
            }
            if (inThought_ && readPos_ < buffer_.size()) {
                std::string partial = buffer_.substr(readPos_);
                readPos_ = buffer_.size();
                if (!partial.empty())
                    emit({TokenEvent::THOUGHT, partial, {}, {}});
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
        if (gt == std::string::npos)
            return;

        // Identify the tag
        std::string tagName = identifyTag(readPos_);
        if (tagName.empty()) {
            // Unknown / closing tag path. Orphan closes of known protocol
            // names (</context_feed>, </response> outside response mode,
            // </result>, etc.) are pure harness echo — consume silently,
            // never emit as TEXT/THOUGHT. Models parrot these constantly.
            size_t nameStart = readPos_ + 1;
            bool isClose = false;
            if (nameStart < buffer_.size() && buffer_[nameStart] == '/') {
                nameStart++;
                isClose = true;
            }
            size_t nameEnd = buffer_.find_first_of(" >/", nameStart);
            if (nameEnd != std::string::npos) {
                std::string raw = buffer_.substr(nameStart, nameEnd - nameStart);
                if (raw == "response" && inResponse_) {
                    inResponse_ = false;
                    TokenEvent ev{TokenEvent::RESPONSE, "", {}, {}};
                    auto fit = responseAttrs_.find("final");
                    if (fit != responseAttrs_.end()) {
                        ev.metadata["is_final"] = fit->second;
                    }
                    emit(ev);
                    responseAttrs_.clear();
                } else if (isClose &&
                           (raw == "response" || raw == "thought" ||
                            raw == "think" || raw == "thinking" ||
                            raw == "action" || raw == "result" ||
                            raw == "context_feed" || raw == "system" ||
                            raw == "user")) {
                    // Orphan close outside its stream mode — skip, no emit.
                }
                // else: truly unknown open/close — still skip the tag bytes
                // (do not paint as thought). Body text around it still emits.
            }
            // Skip past the entire tag (open or close)
            readPos_ = gt + 1;
            continue;
        }

        // Check for self-closing tag (/>)
        bool selfClosing = (gt > readPos_ && buffer_[gt - 1] == '/');

        std::string openingTag, content;
        size_t contentStart;
        size_t closingPos = 0;

        if (selfClosing) {
            // <tag ... /> — no content, tag ends at />
            openingTag = buffer_.substr(readPos_ + 1, gt - readPos_ - 2);  // between < and />
            contentStart = gt + 1;
            content = "";
        } else if (tagName == "response") {
            // STREAMING: enter response mode. From here until the real
            // </response>, all text is user-visible response content, including
            // literal examples such as `<response>` and `<action ...>`.
            contentStart = gt + 1;
            openingTag = buffer_.substr(readPos_ + 1, contentStart - readPos_ - 2);
            responseAttrs_ = parseAttrs(openingTag);
            inResponse_ = true;
            responseContentStart_ = contentStart;
            readPos_ = contentStart;
            continue;
        } else if (tagName == "thought") {
            // STREAMING thought — paint from first body bytes, not after </thought>.
            contentStart = gt + 1;
            inThought_ = true;
            thoughtContentStart_ = contentStart;
            readPos_ = contentStart;
            continue;
        } else if (tagName == "action") {
            // Emit a provisional ACTION_START as soon as the opening tag is
            // complete (name/id/type visible) so the UI paints the card before
            // a large JSON body finishes streaming. Execute only on full close.
            contentStart = gt + 1;
            openingTag = buffer_.substr(readPos_ + 1, contentStart - readPos_ - 2);
            auto openAttrs = parseAttrs(openingTag);
            closingPos = findClosingTag(tagName, contentStart);
            if (closingPos == std::string::npos) {
                // Provisional UI-only emit (no execute, no usedActionIds).
                if (!finalResponseSeen_ && !hasProvisionalAction_) {
                    auto provisional = buildAction("", openAttrs);
                    if (provisional) {
                        hasProvisionalAction_ = true;
                        provisionalActionId_ = provisional->id;
                        TokenEvent pev{TokenEvent::ACTION_START, "", provisional, {}};
                        pev.metadata["provisional"] = "true";
                        emit(pev);
                    }
                }
                return;  // wait for </action>
            }
            size_t closingTagStart = closingPos - lastCloseLen_;
            content = buffer_.substr(contentStart, closingTagStart - contentStart);
            // Reuse provisional id so the UI card updates in place.
            if (hasProvisionalAction_ && openAttrs.find("id") == openAttrs.end() &&
                !provisionalActionId_.empty())
                openAttrs["id"] = provisionalActionId_;
            hasProvisionalAction_ = false;
            provisionalActionId_.clear();
            handleAction(content, openAttrs);
            readPos_ = closingPos;
            continue;
        } else {
            closingPos = findClosingTag(tagName, gt + 1);
            if (closingPos == std::string::npos)
                return;  // Tag not closed yet

            contentStart = gt + 1;
            size_t closingTagStart =
                closingPos - lastCloseLen_;  // back to < of </tagName>
            openingTag = buffer_.substr(readPos_ + 1, contentStart - readPos_ - 2);
            content = buffer_.substr(contentStart, closingTagStart - contentStart);
        }

        auto attrs = parseAttrs(openingTag);

        // Dispatch to handler (thought/response handled via stream modes above)
        if (tagName == "action")
            handleAction(content, attrs);
        else if (tagName == "response")
            handleResponse(content, attrs);
        else if (tagName == "result")
            handleResult(content, attrs);
        else if (tagName == "context_feed")
            handleContextFeed(content, attrs);
        else if (tagName == "thought" && selfClosing)
            handleThought(content);

        // Advance past closing tag
        if (selfClosing) {
            readPos_ = contentStart;  // past />
        } else {
            readPos_ = closingPos;  // past </tagName>
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

    // Normalize: <think> (HTML-style, used by some models including minimax-m3
    // when emitting native reasoning tokens) and <thinking> are the same as
    // <thought>. Without this, those tokens would be stripped as bare text and
    // the user would see no reasoning in the TUI. All three forms fuse to
    // the same TokenEvent::THOUGHT stream and the same Thought timeline row,
    // so the harness context is uniform regardless of which alias the model
    // emits — real test-time-compute thinking and harness-time thinking look
    // identical downstream.
    if (tagName == "think" || tagName == "thinking")
        tagName = "thought";

    static const std::vector<std::string> known = {"thought", "thinking", "action", "response", "result",
                                                   "context_feed"};
    for (auto& k : known) {
        if (tagName == k)
            return tagName;
    }
    return "";
}

// ---------------------------------------------------------------------------
// Find closing tag
//
// PP01: depth-counting + JSON-string-aware so that
//   <action ...>{"snippet": "<action></action>"}</action>
// closes at the OUTER </action>, not the one inside the JSON body.
// ---------------------------------------------------------------------------
size_t Parser::findClosingTag(const std::string& tagName, size_t contentStart) {
    const std::string openMarker = "<" + tagName;
    const std::string closeMarker = "</" + tagName + ">";
    // For the thought stream, accept </think> AND </thinking> as valid closes
    // (the open tag may be <thought>, <think>, or <thinking>; all three fuse
    // to tagName="thought" above, so all three closes must work too).
    const std::string altCloseMarker = tagName == "thought" ? "</think>" : "";
    const std::string altCloseMarker2 = tagName == "thought" ? "</thinking>" : "";
    // <thinking> normalizes to "thought" above, so the parser also needs to
    // accept </thinking> as a valid close for the thought stream (models that
    // open <thinking>...</thinking> would otherwise never close).

    if (closingScanTag_ != tagName || closingScanContentStart_ != contentStart) {
        closingScanTag_ = tagName;
        closingScanContentStart_ = contentStart;
        closingScanPos_ = contentStart;
        closingScanDepth_ = 1;
        closingScanInString_ = false;
        closingScanEscape_ = false;
    }

    auto resetScan = [&] {
        closingScanTag_.clear();
        closingScanContentStart_ = std::string::npos;
        closingScanPos_ = 0;
        closingScanDepth_ = 1;
        closingScanInString_ = false;
        closingScanEscape_ = false;
    };
    auto partialAtEnd = [&](size_t pos, const std::string& marker) {
        size_t remaining = buffer_.size() - pos;
        return remaining < marker.size() && marker.compare(0, remaining, buffer_, pos, remaining) == 0;
    };

    size_t i = closingScanPos_;
    while (i < buffer_.size()) {
        char c = buffer_[i];
        if (closingScanEscape_) {
            closingScanEscape_ = false;
            ++i;
            continue;
        }
        if (closingScanInString_) {
            if (c == '\\') closingScanEscape_ = true;
            else if (c == '"') closingScanInString_ = false;
            ++i;
            continue;
        }
        if (c == '"') {
            closingScanInString_ = true;
            ++i;
            continue;
        }

        if (c == '<') {
            if (partialAtEnd(i, closeMarker) ||
                (!altCloseMarker.empty() && partialAtEnd(i, altCloseMarker)) ||
                (!altCloseMarker2.empty() && partialAtEnd(i, altCloseMarker2)) ||
                partialAtEnd(i, openMarker)) {
                closingScanPos_ = i;
                return std::string::npos;
            }
            if (buffer_.compare(i, closeMarker.size(), closeMarker) == 0) {
                if (--closingScanDepth_ == 0) {
                    lastCloseLen_ = closeMarker.size();
                    size_t end = i + closeMarker.size();

                    resetScan();
                    return end;
                }
                i += closeMarker.size();
                continue;
            }
            if (!altCloseMarker.empty() &&
                buffer_.compare(i, altCloseMarker.size(), altCloseMarker) == 0) {
                if (--closingScanDepth_ == 0) {
                    lastCloseLen_ = altCloseMarker.size();
                    size_t end = i + altCloseMarker.size();
                    resetScan();
                    return end;
                }
                i += altCloseMarker.size();
                continue;
            }
            if (!altCloseMarker2.empty() &&
                buffer_.compare(i, altCloseMarker2.size(), altCloseMarker2) == 0) {
                if (--closingScanDepth_ == 0) {
                    lastCloseLen_ = altCloseMarker2.size();
                    size_t end = i + altCloseMarker2.size();
                    resetScan();
                    return end;
                }
                i += altCloseMarker2.size();
                continue;
            }
            if (buffer_.compare(i, openMarker.size(), openMarker) == 0) {
                size_t after = i + openMarker.size();
                if (after >= buffer_.size()) {
                    closingScanPos_ = i;
                    return std::string::npos;
                }
                char next = buffer_[after];
                if (next == ' ' || next == '\t' || next == '>' || next == '/') {
                    size_t gt = buffer_.find('>', after);
                    if (gt == std::string::npos) {
                        closingScanPos_ = i;
                        return std::string::npos;
                    }
                    if (gt == i || buffer_[gt - 1] != '/') ++closingScanDepth_;
                    i = gt + 1;
                    continue;
                }
            }
        }
        ++i;
    }
    closingScanPos_ = i;
    return std::string::npos;
}

size_t Parser::findResponseClose(size_t contentStart) const {
    const std::string closeMarker = "</response>";
    size_t pos = contentStart;
    while ((pos = buffer_.find(closeMarker, pos)) != std::string::npos) {
        size_t after = pos + closeMarker.size();
        // Literal markdown examples are normally written as `</response>`.
        // Do not treat that as the protocol close; the real close is not
        // followed by a markdown backtick.
        if (after < buffer_.size() && buffer_[after] == '`') {
            pos = after + 1;
            continue;
        }
        return pos;
    }
    return std::string::npos;
}

// ---------------------------------------------------------------------------
// Parse XML attributes
// ---------------------------------------------------------------------------
std::map<std::string, std::string> Parser::parseAttrs(const std::string& tagContent) {
    std::map<std::string, std::string> attrs;
    size_t pos = tagContent.find_first_of(" \t\r\n");  // skip tag name
    while (pos != std::string::npos && pos < tagContent.size()) {
        pos = tagContent.find_first_not_of(" \t\r\n", pos);
        if (pos == std::string::npos) break;
        size_t keyStart = pos;
        while (pos < tagContent.size() &&
               (std::isalnum(static_cast<unsigned char>(tagContent[pos])) || tagContent[pos] == '_'))
            ++pos;
        if (pos == keyStart) {
            ++pos;
            continue;
        }
        std::string key = tagContent.substr(keyStart, pos - keyStart);
        pos = tagContent.find_first_not_of(" \t\r\n", pos);
        if (pos == std::string::npos || tagContent[pos] != '=') continue;
        pos = tagContent.find_first_not_of(" \t\r\n", pos + 1);
        if (pos == std::string::npos || tagContent[pos] != '"') continue;
        size_t valueStart = ++pos;
        size_t valueEnd = tagContent.find('"', valueStart);
        if (valueEnd == std::string::npos) break;
        attrs[std::move(key)] = tagContent.substr(valueStart, valueEnd - valueStart);
        pos = valueEnd + 1;
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
    if (!action)
        return;

    // Enforce: reject duplicate action IDs (unique across the agent run) — BUT
    // replay the retained result instead of stalling. A model that re-emits an
    // already-run action id (seen across iterations via usedActionIds_) is
    // either confused or genuinely re-requesting; if we still have the prior
    // result, return it idempotently so the turn proceeds instead of deadlocking
    // on a duplicate-id error the model cannot resolve.
    if (usedActionIds_.count(action->id)) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto retained = retainedResults_.find(action->id);
        const bool retainedIsReal =
            retained != retainedResults_.end() && retained->second.isObject() &&
            retained->second.type() == Json::objectValue &&
            !retained->second.isMember("protocol_error") &&
            !(retained->second.isMember("success") &&
              retained->second["success"].asBool() == false);
        if (retainedIsReal) {
            // Idempotent replay — the model already ran id and got this result.
            results_[action->id] = retained->second;
            completed_[action->id] = true;
            emit({TokenEvent::ACTION_RESULT,
                  Json::writeString(Json::StreamWriterBuilder(), retained->second),
                  nullptr,
                  {{"id", action->id}}});
            emit({TokenEvent::ERROR,
                  "duplicate id replayed: " + action->id +
                      " (returned retained result; next action needs a new id)",
                  nullptr,
                  {{"id", action->id}, {"reason", "duplicate_action_id_replay"}}});
            return;
        }
        // Auto-suffix a unique id so one collision does not poison the batch.
        // Models often re-emit the same id with a fuller body on the next line.
        std::string base = action->id.empty() ? "act" : action->id;
        std::string uniq = base;
        int n = 2;
        while (usedActionIds_.count(uniq) && n < 1000) {
            uniq = base + "-" + std::to_string(n++);
        }
        const std::string originalId = action->id;
        action->id = uniq;
        emit({TokenEvent::ERROR,
              "duplicate id remapped: " + originalId + " → " + uniq,
              nullptr,
              {{"id", originalId},
               {"remapped_to", uniq},
               {"reason", "duplicate_action_id_remapped"}}});
        // Fall through with remapped id (lock released below path needs insert).
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
    // <result> is runtime-owned. LLM-emitted result tags are ignored so the
    // model cannot forge tool/sub-agent outcomes after a real failure.
    // Track + surface the first forgery so the harness can correct the model
    // instead of silently dropping (silent drop = model never learns).
    ++forgedResultCount_;
    if (forgedResultCount_ == 1) {
        std::string id = "?";
        auto it = attrs.find("id");
        if (it != attrs.end()) id = it->second;
        emit({TokenEvent::ERROR,
              "forged <result id=\"" + id + "\"> ignored — runtime injects "
              "real results; never emit <result> tags yourself",
              nullptr,
              {{"id", id}, {"reason", "forged_result"}}});
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
std::shared_ptr<ParsedAction> Parser::buildAction(const std::string& json,
                                                  const std::map<std::string, std::string>& attrs) {
    auto action = std::make_shared<ParsedAction>();

    // Parse type
    auto typeIt = attrs.find("type");
    action->type = (typeIt != attrs.end()) ? parseType(typeIt->second) : ActionType::TOOL;

    // Parse mode
    auto modeIt = attrs.find("mode");
    action->mode = (modeIt != attrs.end()) ? parseMode(modeIt->second) : ExecutionMode::SYNC;
    // Sub-agents are long-running. ASYNC + waitForActions(30s) is how parents
    // TIMEOUT while the child burns compute for minutes (live dump autopsy).
    // Agent actions always join in-line unless explicitly fire_and_forget.
    if (action->type == ActionType::AGENT &&
        action->mode == ExecutionMode::ASYNC) {
        action->mode = ExecutionMode::SYNC;
    }

    // Name
    auto nameIt = attrs.find("name");
    if (nameIt != attrs.end())
        action->name = nameIt->second;

    // Reject actions with missing/empty name BEFORE dispatch — an empty name
    // would otherwise surface as a cryptic "tool not available: " and stall
    // the loop while the model keeps retrying the same malformed tag.
    if (action->name.empty()) {
        action->params["__protocol_error"] =
            "action missing name attribute — every <action> needs "
            "type=\"tool\" name=\"ACTUAL_TOOL\" id=\"unique\">BODY</action>";
        action->content.clear();
        return action;
    }

    // ID
    auto idIt = attrs.find("id");
    if (idIt != attrs.end()) {
        action->id = idIt->second;
    } else {
        action->id = "__auto_" + std::to_string(++idCounter_);
    }

    // Timeout
    auto timeoutIt = attrs.find("timeout");
    if (timeoutIt != attrs.end())
        action->timeout = std::stoi(timeoutIt->second);

    // Depends on
    auto depIt = attrs.find("depends_on");
    if (depIt != attrs.end()) {
        std::string deps = depIt->second;
        std::regex commaRe(",\\s*");
        std::sregex_token_iterator it(deps.begin(), deps.end(), commaRe, -1);
        std::sregex_token_iterator end;
        for (; it != end; ++it)
            action->dependsOn.push_back(*it);
    }

    // Extra XML attrs become scalar params. Reserved protocol attrs stay structural.
    Json::Value attrParams(Json::objectValue);
    static const std::unordered_set<std::string> reservedAttrs = {"type", "name",       "id",
                                                                  "mode", "depends_on", "timeout"};
    auto attrScalar = [](const std::string& value) -> Json::Value {
        if (value == "true")
            return Json::Value(true);
        if (value == "false")
            return Json::Value(false);
        static const std::regex intRe(R"(^-?\d+$)");
        static const std::regex floatRe(R"(^-?(\d+\.\d*|\d*\.\d+)$)");
        try {
            if (std::regex_match(value, intRe))
                return Json::Value(static_cast<Json::Int64>(std::stoll(value)));
            if (std::regex_match(value, floatRe))
                return Json::Value(std::stod(value));
        } catch (...) {
        }
        return Json::Value(value);
    };
    for (const auto& [key, value] : attrs) {
        if (!reservedAttrs.count(key))
            attrParams[key] = attrScalar(value);
    }

    // Parse JSON body. Do NOT resolve ${...} here — resolution is dispatch-time
    // only (after depends_on producers complete). Silent completeJson repair is
    // forbidden for bodies that look like JSON: fail closed instead.
    std::string cleaned = trimJson(json);
    action->params = attrParams;

    auto looksLikeJson = [](const std::string& s) -> bool {
        if (s.empty())
            return false;
        size_t i = s.find_first_not_of(" \t\n\r");
        if (i == std::string::npos)
            return false;
        return s[i] == '{' || s[i] == '[';
    };

    if (cleaned.empty()) {
        return action;
    }

    // Helper: extract <param name="k">v</param> blocks into a Json::Value.
    auto xmlishParamsToJson = [](const std::string& s) -> Json::Value {
        if (s.find("<param") == std::string::npos)
            return Json::Value();
        Json::Value out(Json::objectValue);
        size_t pos = 0;
        while (true) {
            auto start = s.find("<param name=", pos);
            if (start == std::string::npos) start = s.find("<param name='", pos);
            if (start == std::string::npos) break;
            char q = s[start + 12] == '"' ? '"' : '\'';
            auto nameEnd = s.find(q, start + 13);
            if (nameEnd == std::string::npos) break;
            std::string key = s.substr(start + 13, nameEnd - start - 13);
            auto gt = s.find('>', nameEnd);
            if (gt == std::string::npos) break;
            auto valEnd = s.find("</param>", gt + 1);
            if (valEnd == std::string::npos) break;
            std::string val = s.substr(gt + 1, valEnd - gt - 1);
            // Trim
            auto trim = [](std::string& str) {
                while (!str.empty() &&
                       (str.front() == ' ' || str.front() == '\n' ||
                        str.front() == '\r' || str.front() == '\t'))
                    str.erase(0, 1);
                while (!str.empty() &&
                       (str.back() == ' ' || str.back() == '\n' ||
                        str.back() == '\r' || str.back() == '\t'))
                    str.pop_back();
            };
            trim(key);
            trim(val);
            out[key] = val;
            pos = valEnd + 8;
        }
        return out.empty() ? Json::Value() : out;
    };

    if (looksLikeJson(cleaned)) {
        Json::Value params;
        Json::CharReaderBuilder reader;
        std::string errs;
        std::istringstream ss(cleaned);
        if (!Json::parseFromStream(reader, ss, &params, &errs)) {
            // Mark for protocol_error at execute — keep id/name for the result tag.
            // Include a snippet around the failure column so the model can
            // SEE what broke instead of blind-retrying the same long body.
            std::string detail = errs.empty() ? "invalid JSON" : errs;
            // jsoncpp reports "Line L, Column C\n ..." — extract column if present.
            size_t colPos = detail.find("Column ");
            if (colPos != std::string::npos) {
                size_t start = detail.find_first_of("0123456789", colPos + 7);
                size_t end = detail.find_first_not_of("0123456789", start);
                if (start != std::string::npos) {
                    int col = std::atoi(detail.substr(start, end - start).c_str());
                    int lo = std::max(0, col - 40);
                    int hi = std::min(static_cast<int>(cleaned.size()), col + 40);
                    if (col > 0 && lo < hi) {
                        std::string snip = cleaned.substr(static_cast<size_t>(lo),
                                                          static_cast<size_t>(hi - lo));
                        for (auto& ch : snip)
                            if (ch == '\n' || ch == '\r') ch = ' ';
                        detail += " near: …" + snip + "…";
                    }
                }
            }
            action->params["__protocol_error"] =
                "invalid JSON action body: " + detail;
            action->content.clear();
            return action;
        }
        action->params = params;
        if (action->params.isObject()) {
            for (const auto& key : attrParams.getMemberNames()) {
                if (!action->params.isMember(key))
                    action->params[key] = attrParams[key];
            }
        }
    } else {
        // XML <params> block? Extract into params so the model never gets a
        // silent param-drop → confusing "X is required" → retry loop.
        Json::Value xmlParams = xmlishParamsToJson(cleaned);
        if (!xmlParams.isNull()) {
            action->params = xmlParams;
            for (const auto& key : attrParams.getMemberNames()) {
                if (!action->params.isMember(key))
                    action->params[key] = attrParams[key];
            }
        } else {
            // Plain text body (agent instructions, text-mode tools).
            action->content = json;
        }
    }

    return action;
}

// ---------------------------------------------------------------------------
// Execute action
// ---------------------------------------------------------------------------
void Parser::executeAction(std::shared_ptr<ParsedAction> action) {
    if (!executor_)
        return;

    // Repeated identical failure guard — a collapsing free model re-emits the
    // same broken action (same id, keeps failing). Count per id; past a small
    // threshold inject a hard corrective <thought> so the loop stops instead of
    // streaming KBs indefinitely.
    auto trackRepeatFailure = [&]() {
        int n = ++actionFailCount_[action->id];
        if (n >= 3) {
            emit({TokenEvent::THOUGHT,
                  "System: action '" + action->id +
                      "' has failed multiple times with the same body. STOP "
                      "re-emitting it — fix the action (new unique id, valid "
                      "JSON, correct params) or move on. Repeated identical "
                      "failed actions burn budget.",
                  nullptr, {}});
        }
    };

    // depends_on is only legal with sync (CANON §4).
    if (!action->dependsOn.empty() && action->mode != ExecutionMode::SYNC) {
        trackRepeatFailure();
        Json::Value err;
        err["success"] = false;
        err["protocol_error"] = true;
        err["error"] = "depends_on requires mode=sync";
        {
            std::lock_guard<std::mutex> lock(mtx_);
            results_[action->id] = err;
            completed_[action->id] = true;
        }
        emit({TokenEvent::ACTION_RESULT,
              Json::writeString(Json::StreamWriterBuilder(), err),
              nullptr,
              {{"id", action->id}}});
        emit({TokenEvent::ERROR,
              err["error"].asString(),
              nullptr,
              {{"id", action->id}, {"reason", "depends_on_mode"}}});
        return;
    }

    // Invalid JSON first — hollow checks clear content on parse failure and
    // would otherwise mask invalid_json_body as empty_action_body.
    if (action->params.isObject() && action->params.isMember("__protocol_error")) {
        trackRepeatFailure();
        Json::Value err;
        err["success"] = false;
        err["protocol_error"] = true;
        err["error"] = action->params["__protocol_error"].asString();
        {
            std::lock_guard<std::mutex> lock(mtx_);
            results_[action->id] = err;
            completed_[action->id] = true;
        }
        emit({TokenEvent::ACTION_RESULT,
              Json::writeString(Json::StreamWriterBuilder(), err),
              nullptr,
              {{"id", action->id}}});
        emit({TokenEvent::ERROR,
              err["error"].asString(),
              nullptr,
              {{"id", action->id}, {"reason", "invalid_json_body"}}});
        return;
    }

    // Hollow AGENT: `>{}</action>` then full brief — burns a child with empty
    // instruction or poisons id replay. Require real instruction text.
    if (action->type == ActionType::AGENT) {
        const bool noContent =
            action->content.find_first_not_of(" \t\n\r{}") == std::string::npos;
        bool hasInstr = action->params.isObject() &&
                        ((action->params.isMember("instruction") &&
                          action->params["instruction"].isString() &&
                          !action->params["instruction"].asString().empty()) ||
                         (action->params.isMember("prompt") &&
                          action->params["prompt"].isString() &&
                          !action->params["prompt"].asString().empty()) ||
                         (action->params.isMember("task") &&
                          action->params["task"].isString() &&
                          !action->params["task"].asString().empty()));
        if (noContent && !hasInstr) {
            trackRepeatFailure();
            Json::Value err;
            err["success"] = false;
            err["protocol_error"] = true;
            err["error"] =
                "empty agent body — put the mission text inside <action>…</action>; "
                "hollow {} is ignored (re-emit with full brief + new or same id)";
            {
                std::lock_guard<std::mutex> lock(mtx_);
                results_[action->id] = err;
                completed_[action->id] = true;
            }
            emit({TokenEvent::ACTION_RESULT,
                  Json::writeString(Json::StreamWriterBuilder(), err),
                  nullptr,
                  {{"id", action->id}}});
            emit({TokenEvent::ERROR, err["error"].asString(), nullptr,
                  {{"id", action->id}, {"reason", "empty_action_body"}}});
            return;
        }
    }

    // Hollow filesystem tools: models stream `<action name="tree">{}</action>`
    // then the real body. `{}` on tree/list used to dump cwd and poison ids.
    // Only gate tools that require path/cmd — bare `{}` stays valid for sleep/tests.
    if (action->type == ActionType::TOOL) {
        const std::string& n = action->name;
        const bool needsPath =
            n == "tree" || n == "list" || n == "fs_read" || n == "fs_write" ||
            n == "grep" || n == "simple_fs_write";
        const bool needsCmd = n == "exec";
        if (needsPath || needsCmd) {
            // `{}` alone is hollow — models stream empty object then the real
            // body. Whitespace-only OR braces-only counts as empty content.
            const bool noContent =
                action->content.find_first_not_of(" \t\n\r{}") ==
                std::string::npos;
            bool hasPath = action->params.isObject() &&
                           ((action->params.isMember("path") &&
                             action->params["path"].isString() &&
                             !action->params["path"].asString().empty()) ||
                            (action->params.isMember("file") &&
                             action->params["file"].isString() &&
                             !action->params["file"].asString().empty()));
            bool hasCmd = action->params.isObject() &&
                          ((action->params.isMember("cmd") &&
                            !action->params["cmd"].asString().empty()) ||
                           (action->params.isMember("command") &&
                            !action->params["command"].asString().empty()) ||
                           (action->params.isMember("argv") &&
                            action->params["argv"].isArray() &&
                            !action->params["argv"].empty()));
            if (noContent && ((needsPath && !hasPath) || (needsCmd && !hasCmd))) {
                trackRepeatFailure();
                Json::Value err;
                err["success"] = false;
                err["protocol_error"] = true;
                err["error"] =
                    std::string("empty ") + n +
                    " body — emit required params before </action>; hollow {} ignored";
                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    results_[action->id] = err;
                    completed_[action->id] = true;
                }
                emit({TokenEvent::ACTION_RESULT,
                      Json::writeString(Json::StreamWriterBuilder(), err),
                      nullptr,
                      {{"id", action->id}}});
                emit({TokenEvent::ERROR, err["error"].asString(), nullptr,
                      {{"id", action->id}, {"reason", "empty_action_body"}}});
                return;
            }
        }
    }

    if (!canExecute(*action)) {
        pending_.push_back(action);
        return;
    }

    auto doExecute = [this](std::shared_ptr<ParsedAction> a) {
        // Dispatch-time resolution only — deps are complete when we get here.
        a->params = resolveVars(a->params);
        if (!a->content.empty())
            a->content = resolveVars(a->content);

        Json::Value result = executor_(*a);

        std::lock_guard<std::mutex> lock(mtx_);
        results_[a->id] = result;
        completed_[a->id] = true;
        retainedResults_[a->id] = result;  // survives clearResults for replay
        actionFailCount_.erase(a->id);     // success clears repeat-fail counter

        emit({TokenEvent::ACTION_RESULT,
              Json::writeString(Json::StreamWriterBuilder(), result),
              nullptr,
              {{"id", a->id}}});
    };

    switch (action->mode) {
        case ExecutionMode::SYNC:
            // Agent actions must not block feed(): a sync child (7s) starved
            // the next sibling ACTION_START so the TUI only showed one card.
            // Tools stay in-thread. Generation still joins via waitForActions.
            if (action->type == ActionType::AGENT) {
                std::lock_guard<std::mutex> lock(mtx_);
                futures_.push_back(
                    std::async(std::launch::async, doExecute, action));
            } else {
                doExecute(action);
            }
            break;
        case ExecutionMode::ASYNC:
        case ExecutionMode::FIRE_AND_FORGET: {
            std::lock_guard<std::mutex> lock(mtx_);
            futures_.push_back(std::async(std::launch::async, doExecute, action));
            break;
        }
    }

    dispatchPending();
}

// ---------------------------------------------------------------------------
// Check if action can execute (dependencies satisfied)
// ---------------------------------------------------------------------------
bool Parser::canExecute(const ParsedAction& action) const {
    for (const auto& dep : action.dependsOn) {
        auto it = completed_.find(dep);
        if (it == completed_.end() || !it->second)
            return false;
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
//
// PP03: dispatchPending() acquires mtx_ itself, so we must NOT hold the lock
// across that call (the previous implementation self-deadlocked).
// ---------------------------------------------------------------------------
void Parser::injectResult(const std::string& id, const Json::Value& result) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        results_[id] = result;
        completed_[id] = true;
        retainedResults_[id] = result;  // survives clearResults for idempotent replay
        emit({TokenEvent::ACTION_RESULT,
              Json::writeString(Json::StreamWriterBuilder(), result),
              nullptr,
              {{"id", id}}});
    }
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
    // Vet-fix: Ctrl-C speed. The previous one-shot `wait_for(deadline)` could
    // stall the worker for `actionTimeoutSec` (default ~30s+) because the
    // future was either finished or not — there was no in-band cancellation
    // path. We slice the wait into short segments and consult g_running on
    // every slice so Ctrl-C/quit aborts within ~250ms regardless of the
    // remaining budget. Deadline is honored as an upper bound.
    auto wallStart = std::chrono::steady_clock::now();
    auto deadlineAbs = wallStart + deadline;
    bool hasDeadline = deadline.count() > 0;
    for (auto& f : futs) {
        if (!f.valid())
            continue;
        while (true) {
            if (!g_running)
                return false;
            auto now = std::chrono::steady_clock::now();
            // Both branches emit a duration in the same unit so std::min works.
            std::chrono::milliseconds remaining =
                hasDeadline ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                 deadlineAbs - now)
                            : std::chrono::milliseconds(250);
            if (hasDeadline && remaining <= std::chrono::milliseconds(0))
                return false; // deadline reached, abandon
            std::chrono::milliseconds want =
                hasDeadline ? std::min(remaining, std::chrono::milliseconds(250))
                            : std::chrono::milliseconds(250);
            auto status = f.wait_for(want);
            if (status == std::future_status::ready)
                break;
            // timeout/deferred — loop back, re-read g_running, re-slice.
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
    // Between iterations of the same agent run: drop per-turn results so
    // depends_on does not see stale completion, but KEEP usedActionIds_
    // (CANON §7 — unique across the whole prompt() invocation).
    // Also reset generation-local final/response state so a premature final
    // that was undone does not block the follow-up generation.
    std::lock_guard<std::mutex> lock(mtx_);
    results_.clear();
    completed_.clear();
    pending_.clear();
    finalResponseSeen_ = false;
    inResponse_ = false;
    responseContentStart_ = 0;
    inThought_ = false;
    thoughtContentStart_ = 0;
    responseAttrs_.clear();
    hasProvisionalAction_ = false;
    provisionalActionId_.clear();
    buffer_.clear();
    readPos_ = 0;
    lastCloseLen_ = 0;
    closingScanTag_.clear();
    closingScanContentStart_ = std::string::npos;
    closingScanPos_ = 0;
    closingScanDepth_ = 1;
    closingScanInString_ = false;
    closingScanEscape_ = false;
    // usedActionIds_ intentionally retained
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
    inResponse_ = false;
    responseContentStart_ = 0;
    inThought_ = false;
    thoughtContentStart_ = 0;
    usedActionIds_.clear();
    retainedResults_.clear();
    actionFailCount_.clear();
    responseAttrs_.clear();
    hasProvisionalAction_ = false;
    provisionalActionId_.clear();
    closingScanTag_.clear();
    closingScanContentStart_ = std::string::npos;
    closingScanPos_ = 0;
    closingScanDepth_ = 1;
    closingScanInString_ = false;
    closingScanEscape_ = false;
}

// ---------------------------------------------------------------------------
// Variable resolution — ${id} and ${id.field.subfield}
// ---------------------------------------------------------------------------
namespace {

// Split "a.b[0].c" / "a.b.0.c" into path segments.
std::vector<std::string> splitResolvePath(const std::string& path) {
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        char c = path[i];
        if (c == '.') {
            if (!cur.empty()) {
                parts.push_back(cur);
                cur.clear();
            }
        } else if (c == '[') {
            if (!cur.empty()) {
                parts.push_back(cur);
                cur.clear();
            }
            size_t close = path.find(']', i);
            if (close == std::string::npos) {
                cur.push_back(c);
            } else {
                parts.push_back(path.substr(i + 1, close - i - 1));
                i = close;
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        parts.push_back(cur);
    return parts;
}

Json::Value defaultOutputField(const Json::Value& result) {
    // ${id} shorthand → output, then stdout, then content, else whole object.
    if (result.isObject()) {
        if (result.isMember("output") && !result["output"].isNull())
            return result["output"];
        if (result.isMember("stdout") && !result["stdout"].isNull())
            return result["stdout"];
        if (result.isMember("content") && !result["content"].isNull())
            return result["content"];
    }
    return result;
}

Json::Value navigateResult(const Json::Value& root, const std::string& path) {
    if (path.empty())
        return defaultOutputField(root);
    Json::Value val = root;
    for (const auto& part : splitResolvePath(path)) {
        if (val.isObject() && val.isMember(part)) {
            val = val[part];
        } else if (val.isArray()) {
            try {
                int idx = std::stoi(part);
                if (idx >= 0 && idx < (int)val.size())
                    val = val[idx];
                else
                    return Json::Value();
            } catch (...) {
                return Json::Value();
            }
        } else {
            return Json::Value();
        }
    }
    return val;
}

std::string jsonToResolveString(const Json::Value& val) {
    if (val.isString())
        return val.asString();
    if (val.isNull())
        return "";
    if (val.isBool())
        return val.asBool() ? "true" : "false";
    if (val.isInt64())
        return std::to_string(val.asInt64());
    if (val.isUInt64())
        return std::to_string(val.asUInt64());
    if (val.isDouble())
        return std::to_string(val.asDouble());
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, val);
}

}  // namespace

std::string Parser::resolveVars(const std::string& input) const {
    // ${id} | ${id.field} | ${id.a.b} | ${id.arr[0]}
    std::regex varRe(R"(\$\{([A-Za-z_][A-Za-z0-9_-]*)(?:\.([^}]+))?\})");
    std::string out;
    std::string::const_iterator start = input.cbegin();
    std::smatch match;

    while (std::regex_search(start, input.cend(), match, varRe)) {
        out += match.prefix().str();
        std::string id = match[1].str();
        std::string path = match.size() > 2 ? match[2].str() : "";
        std::string replacement = match[0].str();  // preserve unresolved refs

        auto it = results_.find(id);
        if (it != results_.end()) {
            Json::Value val = navigateResult(it->second, path);
            if (!val.isNull() || path.empty())
                replacement = jsonToResolveString(val);
        }

        out += replacement;
        start = match.suffix().first;
    }
    out.append(start, input.cend());
    return out;
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
        for (auto& v : input)
            arr.append(resolveVars(v));
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
std::string Parser::trimJson(const std::string& raw) {
    size_t start = raw.find_first_not_of(" \t\n\r");
    size_t end = raw.find_last_not_of(" \t\n\r");
    if (start == std::string::npos)
        return "";
    return raw.substr(start, end - start + 1);
}

std::string Parser::cleanJson(const std::string& raw) {
    // Legacy helper: trim only. Do not silently auto-close braces (CANON §4).
    std::string s = trimJson(raw);
    return s.empty() ? "{}" : s;
}

std::string Parser::completeJson(const std::string& raw) {
    if (raw.empty())
        return "{}";
    if (isCompleteJson(raw))
        return raw;

    // Count braces to auto-close
    int braceCount = 0;
    int bracketCount = 0;
    bool inString = false;
    bool escaped = false;

    for (char c : raw) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            inString = !inString;
            continue;
        }
        if (inString)
            continue;
        if (c == '{')
            braceCount++;
        if (c == '}')
            braceCount--;
        if (c == '[')
            bracketCount++;
        if (c == ']')
            bracketCount--;
    }

    std::string result = raw;
    for (int i = 0; i < bracketCount; i++)
        result += "]";
    for (int i = 0; i < braceCount; i++)
        result += "}";

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
    if (s == "async")
        return ExecutionMode::ASYNC;
    if (s == "fire_and_forget" || s == "detached")
        return ExecutionMode::FIRE_AND_FORGET;
    return ExecutionMode::SYNC;
}

ActionType Parser::parseType(const std::string& s) {
    if (s == "agent")
        return ActionType::AGENT;
    if (s == "relic")
        return ActionType::RELIC;
    if (s == "feed")
        return ActionType::FEED;
    if (s == "workflow")
        return ActionType::WORKFLOW;
    if (s == "llm" || s == "llm_call")
        return ActionType::LLM_CALL;
    if (s == "internal")
        return ActionType::INTERNAL;
    return ActionType::TOOL;
}

// ---------------------------------------------------------------------------
// Emit event
// ---------------------------------------------------------------------------
void Parser::emit(const TokenEvent& ev) {
    if (eventCb_)
        eventCb_(ev);
}

}  // namespace cortex::mk3::protocol
