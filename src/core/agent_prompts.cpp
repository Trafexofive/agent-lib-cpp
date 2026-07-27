// =============================================================================
// Agent prompt builders + output sanitize (peeled from agent.cpp)
// =============================================================================

#include "agent.hpp"
#include "agent_xml.hpp"
#include "manifest_loader.hpp"

#include <filesystem>
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
        // Hardcoded fallback
        ss << "    ⚠ ABSOLUTELY REQUIRED: Each turn, output EXACTLY ONE of "
              "these XML formats. Nothing else.\n";
        ss << "    Bare text (not inside <response>...</response>) is "
              "DISCARDED. The user will NOT see it.\n";
        ss << "    \n";
        ss << "    FORMAT A — Final answer:\n";
        ss << "    <response final=\"true\">answer here</response>\n";
        ss << "    \n";
        ss << "    FORMAT B — Call a tool:\n";
        ss << "    <action type=\"tool\" name=\"list\" id=\"ls1\" "
              "mode=\"sync\">{\"path\":\".\"}</action>\n";
        ss << "    id must be unique. Use short ids like ls1, grep1, read1.\n";
        ss << "    \n";
        ss << "    After a tool call, you receive a <result> message. Read the "
              "result, then respond.\n";
        ss << "    Do not call the same tool twice with the same parameters.\n";
        ss << "    Stop after giving your final answer.\n";
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

    // System prompt block — capabilities/tools/behavior (loaded from
    // systemPromptPath)
    if (!systemPrompt_.empty()) {
        ss << "  <system_prompt>\n";
        ss << indentText(systemPrompt_, 4) << "\n";
        ss << "  </system_prompt>\n";
    }

    ss << "  <action_available>\n";
    ss << "    <description>Callable runtime surfaces. Use these with <action "
          "type=\"...\"> only "
          "when needed.</description>\n";

    ss << "    <tools>\n        <description>Functions callable with <action "
          "type=\"tool\">. "
          "Prefer declared JSON params; if a tool declares text input, small "
          "scalar attrs plus a "
          "text body are allowed.</description>\n";
    auto schemaIt = env_.find("__TOOL_SCHEMAS__");
    bool hasSchemas = (schemaIt != env_.end() && !schemaIt->second.empty());
    if (hasSchemas) {
        ss << schemaIt->second << "\n";
    }
    for (const auto &[name, tool] : tools_) {
        // Only emit tools NOT already covered by manifest-loaded schemas.
        if (hasSchemas &&
            schemaIt->second.find("name=\"" + name + "\"") != std::string::npos)
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
        ss << "\n            <params unavailable=\"true\">schema not loaded; "
              "do not guess required "
              "JSON keys</params>\n";
        ss << "        </tool>\n";
    }
    ss << "    </tools>\n";

    if (!relics_.empty()) {
        ss << "    <relics>\n        <description>Persistent stores callable "
              "with <action "
              "type=\"relic\">.</description>\n";
        for (auto &name : relics_) {
            ss << "        <relic name=\"" << xmlAttr(name) << "\"/>\n";
        }
        ss << "    </relics>\n";
    }

    if (!feedNames().empty()) {
        ss << "    <feeds>\n        <description>Ambient context feeds. "
              "Callable with <action "
              "type=\"feed\"> when fresh params are needed.</description>\n";
        for (const auto &name : feedNames()) {
            ss << "        <feed name=\"" << xmlAttr(name)
               << "\" action=\"feed\"/>\n";
        }
        ss << "    </feeds>\n";
    }

    if (!subAgents_.empty()) {
        ss << "    <sub_agents>\n"
              "        <description>Delegatable agents callable with <action "
              "type=\"agent\" "
              "name=\"AGENT_NAME\" id=\"a1\" mode=\"sync\" "
              "ephemeral=\"true|false\" "
              "dump_context=\"true|false\">plain text instruction</action>. "
              "Inputs and outputs are plain text unless the sub-agent says "
              "otherwise. "
              "Default result contains only the sub-agent final response. Set "
              "dump_context=\"true\" "
              "only when you explicitly need its prompts/runtime "
              "trace.</description>\n";
        for (const auto &[name, agent] : subAgents_) {
            const auto &cfg = agent->config();
            ss << "        <sub_agent name=\"" << xmlAttr(name) << "\"";
            if (!cfg.version.empty())
                ss << " version=\"" << xmlAttr(cfg.version) << "\"";
            if (!cfg.summary.empty())
                ss << " summary=\"" << xmlAttr(cfg.summary) << "\"";
            if (!cfg.provider.empty())
                ss << " provider=\"" << xmlAttr(cfg.provider) << "\"";
            if (!cfg.model.empty())
                ss << " model=\"" << xmlAttr(cfg.model) << "\"";
            if (!cfg.manifestDir.empty())
                ss << " manifest_dir=\"" << xmlAttr(cfg.manifestDir) << "\"";
            ss << ">\n";

            auto names = agent->toolNames();
            if (!names.empty()) {
                ss << "            <tools>\n";
                for (const auto &toolName : names) {
                    const auto *tool = agent->findTool(toolName);
                    ss << "                <tool name=\"" << xmlAttr(toolName)
                       << "\"";
                    if (tool && !tool->description().empty() &&
                        tool->description() !=
                            "See input_schema for parameters")
                        ss << " desc=\"" << xmlAttr(tool->description())
                           << "\"";
                    ss << "/>\n";
                }
                ss << "            </tools>\n";
            }
            ss << "        </sub_agent>\n";
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

    // Capability counts — a compact at-a-glance summary of what the active
    // manifest granted. Helps the model self-check before attempting an
    // action: if tools c=0, no <action type="tool"> can succeed.
    ss << "  <manifest_count>";
    ss << "<tools c=" << tools_.size() << ">";
    ss << "<relics c=" << relicNames().size() << ">";
    ss << "<feeds c=" << feedNames().size() << ">";
    ss << "<agents c=" << subAgentNames().size() << ">";
    ss << "</manifest_count>\n";

    ss << "  <cwd>" << std::filesystem::current_path().string() << "</cwd>\n";
    ss << "</system>\n\n";

    // ═══ INLINE EXECUTION TRANSCRIPT ═══
    // Replay what actually happened. Agent/System prefixes are storage detail;
    // the model should see the same inline action → result → response stream.
    if (!history_.empty()) {
        // Apply history cap — only include the most recent N entries
        size_t histStart = 0;
        if (config_.historyCap > 0 &&
            history_.size() > (size_t)config_.historyCap) {
            histStart = history_.size() - config_.historyCap;
        }

        size_t userTurn = 0;
        for (size_t hi = histStart; hi < history_.size(); hi++) {
            const auto &h = history_[hi];
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


}  // namespace cortex::mk3
