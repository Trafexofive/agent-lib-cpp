// ─────────────────────────────────────────────────────────────────────────────
// Context compaction — hybrid of minimal / recommended / profile sugar.
// Canonical docs: docs/manifests/compaction.md
//
// Applies BEFORE history_cap windowing when building the LLM prompt.
// history_cap remains the dumb seatbelt (recomputed every historyCapEveryTurns).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "types.hpp"

namespace cortex {
namespace mk3 {
namespace compaction {

inline CompactionTagPolicy tagOrDefault(const CompactionConfig& cfg, const std::string& name) {
    auto it = cfg.tags.find(name);
    if (it != cfg.tags.end())
        return it->second;
    return cfg.defaultPolicy;
}

// Expand profile → base policy (overrides merge later in loader).
inline void applyProfile(CompactionConfig& cfg) {
    std::string p = cfg.profile;
    for (char& c : p)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (p.empty() || p == "none")
        return;

    // Shared baseline
    cfg.defaultPolicy.keep = "tail";
    cfg.defaultPolicy.keepLast = 6;
    cfg.defaultPolicy.truncateChars = 2000;
    cfg.tags["user"] = CompactionTagPolicy{"all", 0, 0, true};
    cfg.tags["parent"] = CompactionTagPolicy{"all", 0, 0, true};
    cfg.tags["thought"] = CompactionTagPolicy{"none", 0, 0, true};
    cfg.tags["action"] = CompactionTagPolicy{"tail", 12, 1200, true};
    cfg.tags["result"] = CompactionTagPolicy{"tail", 12, 1000, true};
    cfg.tags["response"] = CompactionTagPolicy{"tail", 4, 4000, true};
    // Line-level buckets used by the engine
    cfg.tags["agent"] = CompactionTagPolicy{"tail", 12, 4000, true};
    cfg.tags["system"] = CompactionTagPolicy{"tail", 12, 1200, true};
    if (cfg.neverDrop.empty())
        cfg.neverDrop = {"pin", "open_ask"};
    if (cfg.outputMode.empty())
        cfg.outputMode = "summarize_rules";

    if (p == "light") {
        if (cfg.triggerContextPct <= 0)
            cfg.triggerContextPct = 0.85;
        cfg.tags["thought"] = CompactionTagPolicy{"tail", 2, 800, true};
        cfg.tags["result"].keepLast = 24;
        cfg.tags["agent"].keepLast = 24;
        cfg.tags["system"].keepLast = 24;
        cfg.archiveEnabled = false;
    } else if (p == "aggressive") {
        if (cfg.triggerContextPct <= 0)
            cfg.triggerContextPct = 0.50;
        if (cfg.triggerContextTokens <= 0)
            cfg.triggerContextTokens = 40000;
        cfg.defaultPolicy.keepLast = 4;
        cfg.tags["thought"] = CompactionTagPolicy{"none", 0, 0, true};
        cfg.tags["action"] = CompactionTagPolicy{"tail", 6, 600, true};
        cfg.tags["result"] = CompactionTagPolicy{"tail", 6, 600, true};
        cfg.tags["response"] = CompactionTagPolicy{"tail", 2, 2000, true};
        cfg.tags["agent"] = CompactionTagPolicy{"tail", 6, 2000, true};
        cfg.tags["system"] = CompactionTagPolicy{"tail", 6, 600, true};
        cfg.archiveEnabled = false;
    } else if (p == "archive_first") {
        if (cfg.triggerContextPct <= 0)
            cfg.triggerContextPct = 0.60;
        cfg.archiveEnabled = true;
        if (cfg.archiveSink.empty())
            cfg.archiveSink = "artifact";
        cfg.outputMode = "summarize_rules";
    } else {
        // balanced (default profile)
        if (cfg.triggerContextPct <= 0)
            cfg.triggerContextPct = 0.65;
        if (cfg.triggerContextTokens <= 0)
            cfg.triggerContextTokens = 60000;
        cfg.archiveEnabled = true;
        if (cfg.archiveSink.empty())
            cfg.archiveSink = "artifact";
    }

    if (cfg.cooldownMinTurns <= 0)
        cfg.cooldownMinTurns = 2;
    // Default turn trigger aligns with history_cap_every_turns spirit
    if (cfg.triggerTurns <= 0)
        cfg.triggerTurns = 15;
}

inline size_t estimateTokens(const std::string& s) {
    // Better-than-chars/4 without a real tokenizer.
    // Multi-byte glyphs ≈ 1 tok; ASCII ≈ /4; light word floor.
    if (s.empty()) return 0;
    size_t ascii = 0, multi = 0, words = 0;
    bool inWord = false;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++ascii;
            bool w = std::isalnum(c) || c == '_' || c == '-';
            if (w && !inWord) ++words;
            inWord = w;
            ++i;
        } else {
            int len = 1;
            if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            ++multi;
            inWord = false;
            i += static_cast<size_t>(len);
            if (i > s.size()) break;
        }
    }
    return std::max<size_t>(1, multi + ascii / 4 + words / 8);
}

inline size_t estimateTokens(const std::vector<std::string>& lines) {
    size_t n = 0;
    for (const auto& l : lines)
        n += estimateTokens(l);
    return n;
}

inline int countUserTurns(const std::vector<std::string>& history) {
    int n = 0;
    for (const auto& h : history) {
        if (h.rfind("User: ", 0) == 0)
            ++n;
    }
    return n;
}

enum class LineKind { User, Parent, Agent, System, Other };

inline LineKind classifyLine(const std::string& h) {
    if (h.rfind("User: ", 0) == 0)
        return LineKind::User;
    if (h.rfind("Parent(", 0) == 0)
        return LineKind::Parent;
    if (h.rfind("Agent: ", 0) == 0)
        return LineKind::Agent;
    if (h.rfind("System: ", 0) == 0)
        return LineKind::System;
    return LineKind::Other;
}

inline std::string kindName(LineKind k) {
    switch (k) {
        case LineKind::User:
            return "user";
        case LineKind::Parent:
            return "parent";
        case LineKind::Agent:
            return "agent";
        case LineKind::System:
            return "system";
        default:
            return "other";
    }
}

inline bool isNeverDropLine(const std::string& h, const std::vector<std::string>& neverDrop) {
    std::string lower = h;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto& nd : neverDrop) {
        std::string key = nd;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (key == "pin" &&
            (lower.find("context_pin") != std::string::npos ||
             lower.find("<pin") != std::string::npos || lower.find("[pin]") != std::string::npos))
            return true;
        if (key == "open_ask" &&
            (lower.find("ask_tool") != std::string::npos ||
             lower.find("open_ask") != std::string::npos ||
             lower.find("[ask pending]") != std::string::npos))
            return true;
        if (!key.empty() && lower.find(key) != std::string::npos)
            return true;
    }
    return false;
}

inline std::string truncateBody(const std::string& s, int maxChars) {
    if (maxChars <= 0 || (int)s.size() <= maxChars)
        return s;
    if (maxChars < 16)
        return s.substr(0, static_cast<size_t>(maxChars));
    return s.substr(0, static_cast<size_t>(maxChars - 15)) + "\n…[truncated]";
}

// Strip closed <thought>...</thought> blocks (non-greedy-ish scan).
inline std::string stripThoughts(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        auto open = s.find("<thought", i);
        if (open == std::string::npos) {
            out.append(s, i, std::string::npos);
            break;
        }
        out.append(s, i, open - i);
        auto gt = s.find('>', open);
        if (gt == std::string::npos) {
            out.append(s, open, std::string::npos);
            break;
        }
        auto close = s.find("</thought>", gt + 1);
        if (close == std::string::npos) {
            // unclosed — drop rest of thought start
            break;
        }
        i = close + 10;
    }
    return out;
}

inline bool looksLikeErrorResult(const std::string& s) {
    auto p = s.find("\"success\"");
    if (p != std::string::npos) {
        auto t = s.find("true", p);
        auto f = s.find("false", p);
        if (f != std::string::npos && (t == std::string::npos || f < t))
            return true;
    }
    return s.find("\"error\"") != std::string::npos || s.find("ERROR") != std::string::npos;
}

inline std::string transformLine(const std::string& h, const CompactionConfig& cfg) {
    LineKind lk = classifyLine(h);
    std::string body = h;
    // Thought stripping uses thought policy even on Agent lines
    auto thoughtPol = tagOrDefault(cfg, "thought");
    if (lk == LineKind::Agent && thoughtPol.keep == "none") {
        if (body.rfind("Agent: ", 0) == 0) {
            body = "Agent: " + stripThoughts(body.substr(7));
        } else {
            body = stripThoughts(body);
        }
    }

    std::string kn = kindName(lk);
    auto pol = tagOrDefault(cfg, kn);
    // Prefer finer tags when content looks like result
    if (lk == LineKind::System) {
        auto rp = cfg.tags.find("result");
        if (rp != cfg.tags.end())
            pol = rp->second;
    }

    int trunc = pol.truncateChars;
    if (lk == LineKind::System && pol.onErrorKeepFull && looksLikeErrorResult(body))
        trunc = 0;

    if (trunc > 0) {
        // Preserve prefix ("Agent: " / "System: ")
        size_t prefix = 0;
        if (body.rfind("Agent: ", 0) == 0)
            prefix = 7;
        else if (body.rfind("System: ", 0) == 0)
            prefix = 8;
        else if (body.rfind("User: ", 0) == 0)
            prefix = 6;
        std::string head = body.substr(0, prefix);
        std::string rest = body.substr(prefix);
        body = head + truncateBody(rest, trunc);
    }
    return body;
}

struct CompactResult {
    std::vector<std::string> lines;  // compacted history slice (full vector replacement view)
    bool didCompact = false;
    int dropped = 0;
    int kept = 0;
    std::string note;  // [COMPACTED] system note body (no prefix)
    // Optional archive payload (markdown/jsonl text) — caller may persist
    std::string archiveBody;
};

inline bool shouldTrigger(const CompactionConfig& cfg, size_t promptTokens, int userTurns,
                          int lastCompactUserTurn, int64_t nowMs = 0,
                          int64_t lastCompactWallMs = 0) {
    if (!cfg.enabled)
        return false;
    int since = userTurns - lastCompactUserTurn;
    if (cfg.cooldownMinTurns > 0 && lastCompactUserTurn >= 0 && since < cfg.cooldownMinTurns)
        return false;
    if (cfg.cooldownMinSeconds > 0 && lastCompactWallMs > 0 && nowMs > 0) {
        int64_t elapsed = nowMs - lastCompactWallMs;
        if (elapsed < static_cast<int64_t>(cfg.cooldownMinSeconds) * 1000)
            return false;
    }

    bool anyTrigger = false;
    bool fired = false;

    if (cfg.triggerContextTokens > 0) {
        anyTrigger = true;
        if ((int)promptTokens >= cfg.triggerContextTokens)
            fired = true;
    }
    if (cfg.triggerContextPct > 0.0 && cfg.modelContextTokens > 0) {
        anyTrigger = true;
        double pct = static_cast<double>(promptTokens) / static_cast<double>(cfg.modelContextTokens);
        if (pct >= cfg.triggerContextPct)
            fired = true;
    }
    if (cfg.triggerTurns > 0) {
        anyTrigger = true;
        if (lastCompactUserTurn < 0 || since >= cfg.triggerTurns)
            fired = true;
    }

    // No triggers configured → do not auto-fire
    if (!anyTrigger)
        return false;
    return fired;
}

// Compact a full history vector. Returns filtered/transformed lines (same order).
inline CompactResult compactHistory(const std::vector<std::string>& history,
                                    const CompactionConfig& cfg) {
    CompactResult r;
    if (!cfg.enabled || history.empty()) {
        r.lines = history;
        r.kept = (int)history.size();
        return r;
    }

    // Index by kind for tail selection
    std::map<std::string, std::vector<size_t>> byKind;
    std::vector<bool> protect(history.size(), false);
    for (size_t i = 0; i < history.size(); ++i) {
        if (isNeverDropLine(history[i], cfg.neverDrop))
            protect[i] = true;
        byKind[kindName(classifyLine(history[i]))].push_back(i);
    }

    std::vector<bool> keep(history.size(), false);
    for (size_t i = 0; i < history.size(); ++i) {
        if (protect[i])
            keep[i] = true;
    }

    auto applyKind = [&](const std::string& kn) {
        auto pol = tagOrDefault(cfg, kn);
        auto it = byKind.find(kn);
        if (it == byKind.end())
            return;
        const auto& idxs = it->second;
        if (pol.keep == "all") {
            for (size_t i : idxs)
                keep[i] = true;
            return;
        }
        if (pol.keep == "none") {
            // only protected already marked
            return;
        }
        // tail
        int n = pol.keepLast > 0 ? pol.keepLast : cfg.defaultPolicy.keepLast;
        if (n < 0)
            n = 0;
        int start = std::max(0, (int)idxs.size() - n);
        for (int j = start; j < (int)idxs.size(); ++j)
            keep[idxs[static_cast<size_t>(j)]] = true;
    };

    // Apply known kinds + default for other
    for (const auto& kn : {"user", "parent", "agent", "system", "other"})
        applyKind(kn);

    // Also honor explicit action/result/response by not dropping agent/system tails further
    // (line-level engine maps those into agent/system transforms).

    std::ostringstream arch;
    if (cfg.archiveEnabled && cfg.archiveSink != "none") {
        arch << "# compact archive\n\n";
    }

    for (size_t i = 0; i < history.size(); ++i) {
        if (!keep[i]) {
            r.dropped++;
            if (cfg.archiveEnabled && cfg.archiveSink != "none") {
                arch << "## dropped [" << kindName(classifyLine(history[i])) << "]\n\n";
                arch << history[i] << "\n\n";
            }
            continue;
        }
        r.lines.push_back(transformLine(history[i], cfg));
        r.kept++;
    }

    r.didCompact = r.dropped > 0 || r.lines.size() != history.size();
    // Always mark didCompact when we ran transforms that shrink thoughts even if count same
    if (!r.didCompact) {
        for (size_t i = 0; i < history.size() && i < r.lines.size(); ++i) {
            if (history[i] != r.lines[i]) {
                r.didCompact = true;
                break;
            }
        }
    }

    if (r.didCompact && (cfg.outputMode == "summarize_rules" || cfg.outputMode == "summarize_llm")) {
        std::ostringstream note;
        note << "[COMPACTED] dropped=" << r.dropped << " kept=" << r.kept
             << " tokens≈" << estimateTokens(r.lines);
        if (cfg.archiveEnabled && cfg.archiveSink != "none")
            note << " archive=" << cfg.archiveSink;
        r.note = note.str();
    }
    if (cfg.archiveEnabled && cfg.archiveSink != "none" && r.dropped > 0)
        r.archiveBody = arch.str();

    return r;
}

// Resolve history window start under history_cap + every_turns.
// userTurns = count of User: lines in full history.
// Returns histStart index into history.
inline size_t resolveHistoryWindowStart(size_t historySize, int historyCap, int everyTurns,
                                        int userTurns, int& ioAppliedAtUserTurn,
                                        size_t& ioFrozenStart) {
    if (historyCap <= 0 || historySize <= (size_t)historyCap) {
        ioFrozenStart = 0;
        return 0;
    }
    size_t desired = historySize - static_cast<size_t>(historyCap);

    // First apply
    if (ioAppliedAtUserTurn < -999999) {
        ioFrozenStart = desired;
        ioAppliedAtUserTurn = userTurns;
        return ioFrozenStart;
    }

    int every = everyTurns;
    if (every < 0)
        every = 15;
    // every == 0 → never recompute after first
    // every == 1 → every turn
    bool recompute = (every == 1) ||
                     (every > 1 && (userTurns - ioAppliedAtUserTurn) >= every);
    if (recompute) {
        ioFrozenStart = desired;
        ioAppliedAtUserTurn = userTurns;
    }
    // Frozen start must not exceed desired max (if history shrank) or go past end
    if (ioFrozenStart > desired)
        ioFrozenStart = desired;
    if (ioFrozenStart >= historySize)
        ioFrozenStart = historySize > 0 ? historySize - 1 : 0;
    return ioFrozenStart;
}

}  // namespace compaction
}  // namespace mk3
}  // namespace cortex
