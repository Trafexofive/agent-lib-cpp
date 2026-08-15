// =============================================================================
// Agent prompt builders + output sanitize (peeled from agent.cpp)
// =============================================================================

#include "agent.hpp"
#include "agent_xml.hpp"
#include "compaction.hpp"

#include <chrono>
#include "manifest_loader.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../feeds/feed_engine.hpp"

namespace cortex::mk3 {

std::string Agent::buildSystemPrompt(const AgentContext &ctx) const {
    std::ostringstream ss;

    // ═══ <harness> — protocol spec (pre-indented in constructor) ═══
    ss << "<harness>\n  <protocol>\n";
    if (!harnessText_.empty()) {
        ss << harnessText_;
    } else {
        // Harness file missing — minimal self-contained contract (no external refs).
        ss << "    Emit protocol tags only. Bare text does not finalize.\n"
              "    <thought>…</thought>\n"
              "    <action type=\"tool|agent|relic|feed|workflow\" "
              "name=\"EXACT_FROM_ACTION_AVAILABLE\" id=\"unique\" "
              "mode=\"sync\">BODY</action>\n"
              "    <response>…</response>\n"
              "    <response final=\"true\">…</response>  only normal stop\n"
              "    Runtime injects <result status=\"ok|error|…\"> — never forge.\n"
              "    Never final=\"true\" in the same generation as <action>.\n"
              "    After results: act once more, recover once, or final.\n";
    }
    ss << "  </protocol>\n";
    ss << "  <info name=\"" << xmlAttr(config_.name) << "\" version=\""
       << xmlAttr(config_.version) << "\"/>\n";
    ss << "</harness>\n\n";

    // ═══ <system> — persona, system, tools, relics, context ═══
    ss << "<system>\n";

    // Persona block — identity/values (loaded from personaPath)
    if (!personaText_.empty()) {
        ss << "  <persona>\n";
        ss << indentText(personaText_, 4) << "\n";
        ss << "  </persona>\n";
    }

    // Operator / user context (context.user → USER.md). Ground truth about
    // who is driving the session — defaults, prefs, hard constraints.
    if (!userText_.empty()) {
        ss << "  <user_context path=\"" << xmlAttr(config_.userPath) << "\">\n";
        ss << indentText(userText_, 4) << "\n";
        ss << "  </user_context>\n";
    }

    // System prompt block — capabilities/tools/behavior (loaded from
    // systemPromptPath)
    if (!systemPrompt_.empty()) {
        ss << "  <system_prompt>\n";
        ss << indentText(systemPrompt_, 4) << "\n";
        ss << "  </system_prompt>\n";
    }

    // Reasoning policy — thinking_level hint + hard requireThought rule.
    if (!config_.thinkingLevel.empty() || config_.requireThought) {
        ss << "  <reasoning_policy";
        if (!config_.thinkingLevel.empty())
            ss << " level=\"" << xmlAttr(config_.thinkingLevel) << "\"";
        ss << ">\n";
        if (config_.thinkingLevel == "minimal") {
            ss << "    Act over deliberating: keep <thought> to one short line "
                  "only when it genuinely changes the next step. Do not "
                  "narrate plans.\n";
        } else if (config_.thinkingLevel == "low") {
            ss << "    Think briefly: a short <thought> before actions is "
                  "enough. Skip deep deliberation.\n";
        } else if (config_.thinkingLevel == "high") {
            ss << "    Think thoroughly: reason through the plan in <thought> "
                  "BEFORE any <action>. Weigh tradeoffs, then act.\n";
        }
        if (config_.requireThought) {
            ss << "    Hard rule: emit at least one <thought> before each "
                  "<action>.\n";
        }
        ss << "  </reasoning_policy>\n";
    }

    // Live skill laws (import.skills → SKILL.md). Absent = not claimed.
    auto skillIt = env_.find("__SKILLS_XML__");
    if (skillIt != env_.end() && !skillIt->second.empty()) {
        ss << "  <skills>\n";
        ss << indentText(skillIt->second, 4) << "\n";
        ss << "  </skills>\n";
    }

    // Optional prompt modules (import.files) — contracts/templates.
    auto modIt = env_.find("__PROMPT_MODULES_XML__");
    if (modIt != env_.end() && !modIt->second.empty()) {
        ss << "  <prompt_modules>\n";
        ss << indentText(modIt->second, 4) << "\n";
        ss << "  </prompt_modules>\n";
    }

    ss << "  <action_available>\n";

    ss << "    <tools>\n";
    auto schemaIt = env_.find("__TOOL_SCHEMAS__");
    bool hasSchemas = (schemaIt != env_.end() && !schemaIt->second.empty());
    // Dedup by tool name (set) — string find on the schema blob was fragile
    // and still allowed duplicate git_status / path-tool blocks.
    std::set<std::string> emittedTools;
    if (hasSchemas) {
        ss << schemaIt->second << "\n";
        // Record names already emitted in the schema blob.
        for (const auto &[name, tool] : tools_) {
            const std::string needle = "name=\"" + name + "\"";
            if (schemaIt->second.find(needle) != std::string::npos)
                emittedTools.insert(name);
        }
    }
    for (const auto &[name, tool] : tools_) {
        if (emittedTools.count(name))
            continue;

        // Session-restored script tools keep scriptPath but historically lost
        // schema context. Recover nearest tool.yml so the model sees params.
        bool emittedRecoveredSchema = false;
        if (!tool.scriptPath().empty()) {
            std::filesystem::path scriptPath(tool.scriptPath());
            std::vector<std::filesystem::path> candidates = {
                scriptPath.parent_path() / "tool.yml",
                scriptPath.parent_path().parent_path() / "tool.yml"};
            for (const auto &candidate : candidates) {
                if (!std::filesystem::exists(candidate))
                    continue;
                auto recovered =
                    ManifestLoader::loadToolManifest(candidate.string());
                if (recovered.name.empty() || recovered.name != name)
                    continue;
                const auto &rc = config_.promptBuilding.runtimeCapabilities;
                ss << ManifestLoader::toolSchemasToXml(
                    {recovered}, 8, rc.inputSchemas, rc.returnSchemas,
                    rc.usageExamples);
                emittedRecoveredSchema = true;
                emittedTools.insert(name);
                break;
            }
        }
        if (emittedRecoveredSchema)
            continue;

        ss << "        <tool name=\"" << xmlAttr(name) << "\"";
        if (!tool.description().empty() &&
            tool.description() != "See input_schema for parameters")
            ss << " desc=\"" << xmlAttr(tool.description()) << "\"";
        ss << ">\n";
        // Last-resort card from ToolDef.params when tool.yml was not found.
        // Never leave builtins as bare "schema not loaded" if registry has params.
        if (!tool.params().empty()) {
            ss << "            <params card=\"true\">keys: ";
            bool first = true;
            for (const auto& p : tool.params()) {
                if (!first)
                    ss << ", ";
                first = false;
                ss << p.name;
                if (p.required)
                    ss << "*";
            }
            ss << "</params>\n";
        } else {
            ss << "            <params unavailable=\"true\">schema not loaded; "
                  "do not guess required JSON keys — check "
                  "manifests/built-in/tools/"
               << xmlAttr(name)
               << "/tool.yml resolution (CWD-independent)</params>\n";
        }
        ss << "        </tool>\n";
        emittedTools.insert(name);
    }
    ss << "    </tools>\n";

    if (!relics_.empty()) {
        ss << "    <relics>\n";
        for (auto &name : relics_) {
            ss << "        <relic name=\"" << xmlAttr(name) << "\"/>\n";
        }
        ss << "    </relics>\n";
    }

    if (!feedNames().empty()) {
        ss << "    <feeds>\n";
        for (const auto &name : feedNames()) {
            ss << "        <feed name=\"" << xmlAttr(name) << "\"/>\n";
        }
        ss << "    </feeds>\n";
    }

    if (!subAgents_.empty()) {
        // Slim cards: name + summary only. Child tools stay on the child.
        ss << "    <sub_agents>\n";
        for (const auto &[name, agent] : subAgents_) {
            const auto &cfg = agent->config();
            ss << "        <sub_agent name=\"" << xmlAttr(name) << "\"";
            if (!cfg.summary.empty())
                ss << " summary=\"" << xmlAttr(cfg.summary) << "\"";
            if (!cfg.model.empty())
                ss << " model=\"" << xmlAttr(cfg.model) << "\"";
            ss << "/>\n";
        }
        ss << "    </sub_agents>\n";
    }

    auto wfIt = env_.find("__WORKFLOW_XML__");
    if (wfIt != env_.end() && !wfIt->second.empty()) {
        ss << "    <workflows>\n";
        ss << indentText(wfIt->second, 8) << "\n";
        ss << "    </workflows>\n";
    }

    ss << "  </action_available>\n";

    ss << "  <manifest_count tools=\"" << tools_.size() << "\" relics=\""
       << relicNames().size() << "\" feeds=\"" << feedNames().size()
       << "\" agents=\"" << subAgentNames().size() << "\"/>\n";

    ss << "  <cwd>" << std::filesystem::current_path().string() << "</cwd>\n";
    ss << "</system>\n\n";

    // ═══ INLINE EXECUTION TRANSCRIPT ═══
    // Replay what actually happened. Agent/System prefixes are storage detail;
    // the model should see the same inline action → result → response stream.
    //
    // Order: optional compaction → history_cap window (recomputed every N user
    // turns, default 15) → emit. Session history_ on disk stays full.
    if (!history_.empty()) {
        const int userTurnsTotal = compaction::countUserTurns(history_);

        // Working copy for prompt only
        std::vector<std::string> promptHist = history_;

        // ── Compaction (smart) ──────────────────────────────────────
        if (config_.compaction.enabled) {
            size_t tokEst = compaction::estimateTokens(promptHist);
            // Rough system overhead so pressure trips before the model chokes
            tokEst += 4000;
            const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count();
            if (compaction::shouldTrigger(config_.compaction, tokEst, userTurnsTotal,
                                          lastCompactAtUserTurn_, nowMs,
                                          lastCompactWallMs_)) {
                auto cr = compaction::compactHistory(promptHist, config_.compaction);
                if (cr.didCompact) {
                    promptHist = std::move(cr.lines);
                    lastCompactAtUserTurn_ = userTurnsTotal;
                    lastCompactWallMs_ = nowMs;
                    lastCompactNote_ = cr.note;
                    lastCompactUiPending_ = cr.note;  // UI badge once
                    lastCompactArchive_ = cr.archiveBody;
                    // Archive: file and artifact both land under .cortex/compact/
                    // (artifact sink = same durable dump until full artifact store wire).
                    const bool wantArchive =
                        config_.compaction.archiveEnabled &&
                        config_.compaction.archiveSink != "none" &&
                        !cr.archiveBody.empty() && !ctx.sessionId.empty();
                    if (wantArchive) {
                        namespace fs = std::filesystem;
                        fs::path dir =
                            fs::path(".cortex") / "compact" / ctx.sessionId;
                        std::error_code ec;
                        fs::create_directories(dir, ec);
                        if (!ec) {
                            const char* ext =
                                (config_.compaction.archiveFormat == "jsonl") ? ".jsonl"
                                                                               : ".md";
                            std::ofstream out(dir / ("t" + std::to_string(userTurnsTotal) +
                                                     ext));
                            if (out)
                                out << cr.archiveBody;
                        }
                    }
                }
            }
        }

        // ── history_cap seatbelt (dumb tail; not every turn) ────────
        size_t histStart = compaction::resolveHistoryWindowStart(
            promptHist.size(), config_.historyCap, config_.maxTurnsPerCycle,
            userTurnsTotal, historyCapAppliedAtUserTurn_, historyWindowStart_);

        // Inject last compact note once at the head of the visible window
        if (!lastCompactNote_.empty()) {
            ss << "System: " << lastCompactNote_ << "\n\n";
        }

        size_t userTurn = 0;
        for (size_t hi = histStart; hi < promptHist.size(); hi++) {
            const auto &h = promptHist[hi];
            std::string emitted;
            if (h.rfind("Agent: ", 0) == 0) {
                emitted = h.substr(7);
            } else if (h.rfind("System: ", 0) == 0) {
                emitted = h.substr(8);
            } else if (h.rfind("User: ", 0) == 0) {
                userTurn++;
                // On iteration 1 the current request is sent as a real user
                // message, not replayed inside the system prompt transcript.
                if (ctx.iteration <= 1 && hi + 1 == history_.size() &&
                    h.substr(6) == ctx.userInput)
                    continue;
                emitted = "<user turn=\"" + std::to_string(userTurn) +
                          "\" source=\"human\"";
                if (!ctx.sessionId.empty())
                    emitted += " session=\"" + xmlAttr(ctx.sessionId) + "\"";
                emitted += ">" + h.substr(6) + "</user>";
            } else if (h.rfind("Parent(", 0) == 0) {
                // Parent-agent delegate — distinct from human operator turns.
                auto close = h.find(')');
                std::string from = "parent";
                std::string body = h;
                if (close != std::string::npos) {
                    from = h.substr(7, close - 7);
                    body = (close + 1 < h.size() && h[close + 1] == ':')
                               ? h.substr(close + 2)
                               : h.substr(close + 1);
                    if (!body.empty() && body[0] == ' ')
                        body = body.substr(1);
                }
                if (ctx.iteration <= 1 && hi + 1 == history_.size() &&
                    body == ctx.userInput)
                    continue;
                emitted =
                    "<user turn=\"parent\" source=\"parent_agent\" from=\"" +
                    xmlAttr(from) + "\">" + body + "</user>";
            } else {
                emitted = h;
            }
            ss << emitted;
            if (!emitted.empty() && emitted.back() != '\n')
                ss << '\n';
        }
    }

    return ss.str();
}

std::string Agent::buildUserPrompt(const AgentContext &ctx) const {
    // Surface initiator identity in the live user message so the model can
    // tell a human operator apart from a parent-agent delegate without
    // relying solely on history replay.
    if (ctx.source == PromptSource::ParentAgent) {
        std::string from = ctx.sourceName.empty() ? "parent" : ctx.sourceName;
        return std::string("[FROM parent agent \"") + from + "\"]\n" +
               ctx.userInput;
    }
    return ctx.userInput;
}

std::string Agent::buildDynamicContextPrompt() const {
    std::ostringstream ss;

    if (!pinned_.empty()) {
        ss << "<pinned_context>\n"
              "  <description>Files the agent pinned via context_pin. "
              "Persist until context_unpin.</description>\n";
        for (auto &[key, e] : pinned_) {
            ss << "  <file path=\"" << xmlAttr(e.displayPath) << "\" bytes=\""
               << e.bytes << "\">\n";
            ss << e.content;
            if (!e.content.empty() && e.content.back() != '\n')
                ss << '\n';
            ss << "  </file>\n";
        }
        ss << "</pinned_context>\n";
    }

    if (!peeking_.empty()) {
        if (ss.tellp() > 0)
            ss << "\n";
        ss << "<ephemeral_context>\n"
              "  <description>Files peeked via context_peek. Auto-evicted "
              "after their cycle count expires.</description>\n";
        for (auto &[key, e] : peeking_) {
            ss << "  <file path=\"" << xmlAttr(e.displayPath) << "\" bytes=\""
               << e.bytes << "\" cycles_remaining=\"" << e.cyclesRemaining
               << "\">\n";
            ss << e.content;
            if (!e.content.empty() && e.content.back() != '\n')
                ss << '\n';
            ss << "  </file>\n";
        }
        ss << "</ephemeral_context>\n";
    }

    if (!feeds_.empty()) {
        auto feedResults = feeds::FeedEngine::instance().pollAll();
        auto toolSpecs = feeds::FeedEngine::instance().feedToolSpecs();
        bool any = false;
        for (auto &fr : feedResults) {
            if (!feeds_.count(fr.name))
                continue;
            if (!any) {
                if (ss.tellp() > 0)
                    ss << "\n";
                ss << "<feeds>\n  <description>Dynamic system context "
                      "refreshed each turn. "
                      "Bottom-loaded for prompt-cache stability. "
                      "Each feed may also expose tools — invoke via "
                      "<action type=\"feed\" name=\"<feed>.<tool>\" "
                      ".../>.</description>\n";
                any = true;
            }
            ss << "  <" << fr.name << ">\n";
            ss << "  " << fr.summary << "\n";
            auto specIt = toolSpecs.find(fr.name);
            if (specIt != toolSpecs.end() && !specIt->second.empty()) {
                ss << "    <tools>\n";
                for (const auto &spec : specIt->second) {
                    ss << "      <tool name=\"" << spec.name
                       << "\" description=\"" << spec.description << "\"/>\n";
                }
                ss << "    </tools>\n";
            }
            ss << "  </" << fr.name << ">\n";
        }
        if (any)
            ss << "</feeds>\n";
    }

    if (!contextFeeds_.empty()) {
        if (ss.tellp() > 0)
            ss << "\n";
        ss << "<context_feeds>\n  <description>LLM-requested dynamic context "
              "from prior "
              "turns.</description>\n";
        for (auto &feed : contextFeeds_) {
            ss << "  " << feed << "\n";
        }
        ss << "</context_feeds>\n";
    }

    return ss.str();
}

// ═══════════════════════════════════════════════════════════════════════
// Sanitize output — strip protocol XML tags
// ═══════════════════════════════════════════════════════════════════════

std::string Agent::sanitize(const std::string &output) {
    // Linear state-machine tag stripper — 10-20x faster than regex on large
    // outputs
    static const std::vector<std::string> tags = {"action", "result", "thought",
                                                  "context_feed", "response"};
    std::string out;
    out.reserve(output.size());
    size_t i = 0, n = output.size();
    while (i < n) {
        if (output[i] != '<') {
            out += output[i++];
            continue;
        }
        bool matched = false;
        for (auto &tag : tags) {
            size_t tagLen = tag.size();
            // <tag> or </tag>
            bool isClose = (i + 1 < n && output[i + 1] == '/');
            size_t nameStart = isClose ? i + 2 : i + 1;
            if (n - nameStart >= tagLen &&
                output.compare(nameStart, tagLen, tag) == 0) {
                size_t close = output.find('>', nameStart + tagLen);
                if (close == std::string::npos)
                    break;
                if (isClose) {
                    i = close + 1;
                    matched = true;
                    break;
                }
                // Find matching closing tag
                std::string endTag = "</" + tag + ">";
                size_t endPos = output.find(endTag, close);
                if (endPos == std::string::npos)
                    break;
                i = endPos + endTag.size();
                matched = true;
                break;
            }
        }
        if (!matched)
            out += output[i++];
    }
    size_t start = out.find_first_not_of(" \t\n\r");
    size_t end = out.find_last_not_of(" \t\n\r");
    return (start == std::string::npos) ? ""
                                        : out.substr(start, end - start + 1);
}



// Chat message assembly for one runLoop iteration
// ═══════════════════════════════════════════════════════════════════════
// Prompt Building
// ═══════════════════════════════════════════════════════════════════════

ChatMessages Agent::buildChatPrompt(const AgentContext &ctx) const {
    ChatMessages msgs;
    msgs.push_back(ChatMessage::system(buildSystemPrompt(ctx)));
    // First iteration: current user request is a real user message, not
    // synthetic history inside the system prompt. Later iterations replay the
    // inline action/result transcript from history, but still need a minimal
    // provider-level user message; some OpenRouter routes reject system-only
    // continuation calls as "No user query found".
    if (ctx.iteration <= 1) {
        msgs.push_back(ChatMessage::user(buildUserPrompt(ctx)));
    }
    // Iteration > 1: NO synthetic user message. The inline transcript inside
    // the system prompt carries full context; the model continues from where
    // history stopped. No "…", no "Continue from..." — both were slop.
    std::string dynamicTail = buildDynamicContextPrompt();
    if (!dynamicTail.empty()) {
        // Dynamic context is intentionally last for prompt-cache friendliness,
        // but it is runtime/system context, not another user request.
        msgs.push_back(ChatMessage::system(dynamicTail));
    }
    return msgs;
}

void Agent::compactHistoryInPlaceIfConfigured() {
    if (!config_.compaction.enabled || history_.empty())
        return;
    auto cr = compaction::compactHistory(history_, config_.compaction);
    if (!cr.didCompact)
        return;
    history_ = std::move(cr.lines);
    lastCompactNote_ = cr.note;
    lastCompactUiPending_ = std::string("[child] ") + cr.note;
    lastCompactAtUserTurn_ = compaction::countUserTurns(history_);
    lastCompactWallMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
}

}  // namespace cortex::mk3
