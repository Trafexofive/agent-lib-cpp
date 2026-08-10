#pragma once
// Protocol noise filter — pure helpers.
//
// Small / free-tier models parrot harness scaffolding as bare text:
//   <history>…</history>, <result id=…>…</result>, <context_feed>…,
//   orphan/partial closes (</context_f, </user), salvage banners, etc.
//
// Those must never become Thought timeline rows. Aggressive strip + density
// gate; remaining human prose is kept.

#include <cctype>
#include <string>

namespace cortex::mk3::protocol {

inline bool isWs(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

inline void trimInPlace(std::string& s) {
    size_t a = 0;
    while (a < s.size() && isWs(s[a])) ++a;
    size_t b = s.size();
    while (b > a && isWs(s[b - 1])) --b;
    if (a == 0 && b == s.size()) return;
    s = s.substr(a, b - a);
}

inline bool isAlphaNumUs(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Known harness / protocol tag names (open or close).
inline bool isKnownProtocolName(const std::string& name) {
    if (name.empty()) return false;
    static const char* k[] = {
        "response",   "thought",      "think",         "thinking",
        "action",     "result",       "context_feed",  "context_feeds",
        "context_pin","context_pins", "agent_run_state","history",
        "entry",      "system",       "user",          "assistant",
        "turn_count", "total",        "pin",           "manifest",
        "description","agent",        "tool",          "relic",
        "feed",       "workflow",     "error",         "status",
    };
    for (const char* n : k)
        if (name == n) return true;
    return false;
}

// Extract tag name from "<name ...>" or "</name>" (name only).
inline std::string tagNameAt(const std::string& s, size_t lt) {
    if (lt >= s.size() || s[lt] != '<') return {};
    size_t i = lt + 1;
    if (i < s.size() && s[i] == '/') ++i;
    size_t start = i;
    while (i < s.size() && isAlphaNumUs(s[i])) ++i;
    if (i == start) return {};
    return s.substr(start, i - start);
}

// Remove one complete XML-like tag starting at lt (must be '<').
// Returns end index after the tag, or npos if incomplete.
inline size_t skipCompleteTag(const std::string& s, size_t lt) {
    if (lt >= s.size() || s[lt] != '<') return std::string::npos;
    // Comments / CDATA — rare but skip whole.
    if (lt + 3 < s.size() && s.compare(lt, 4, "<!--") == 0) {
        size_t end = s.find("-->", lt + 4);
        return end == std::string::npos ? std::string::npos : end + 3;
    }
    size_t gt = s.find('>', lt + 1);
    if (gt == std::string::npos) return std::string::npos;
    return gt + 1;
}

// For container tags whose body is pure harness (result/history/context_feed/
// entry/…), drop the whole open…close span when both ends exist.
// Only applies to OPEN tags — a lone </context_feed> must not swallow trailing prose.
inline size_t skipHarnessContainer(const std::string& s, size_t lt,
                                   const std::string& name) {
    if (!isKnownProtocolName(name)) return std::string::npos;
    // Close tags are not containers to swallow.
    if (lt + 1 < s.size() && s[lt] == '<' && s[lt + 1] == '/')
        return std::string::npos;
    // Only swallow full containers for tags that are never "model thought".
    static const char* swallow[] = {
        "result", "history", "entry", "context_feed", "context_feeds",
        "context_pin", "context_pins", "agent_run_state", "system",
        "turn_count", "pin", "description",
    };
    bool doSwallow = false;
    for (const char* n : swallow)
        if (name == n) {
            doSwallow = true;
            break;
        }
    if (!doSwallow) return std::string::npos;

    size_t afterOpen = skipCompleteTag(s, lt);
    if (afterOpen == std::string::npos) return std::string::npos;
    // Self-closing already handled by skipCompleteTag if "/>".
    if (afterOpen >= 2 && s[afterOpen - 2] == '/') return afterOpen;

    const std::string close = "</" + name + ">";
    size_t cpos = s.find(close, afterOpen);
    if (cpos == std::string::npos) {
        // Unclosed OPEN container — drop from open through end (mid-echo).
        return s.size();
    }
    return cpos + close.size();
}

// Strip all known protocol markup; keep human prose between tags.
inline std::string stripAllProtocolMarkup(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] != '<') {
            out.push_back(raw[i++]);
            continue;
        }
        std::string name = tagNameAt(raw, i);
        if (name.empty()) {
            // Lone '<' — keep unless it's a broken partial at end.
            out.push_back(raw[i++]);
            continue;
        }

        // Partial tag name that is a prefix of a known protocol tag
        // (e.g. "</context_f", "<resul") — drop through end of chunk.
        auto isPrefixOfKnown = [&](const std::string& n) -> bool {
            if (n.empty()) return false;
            static const char* k[] = {
                "response",   "thought",      "think",          "thinking",
                "action",     "result",       "context_feed",   "context_feeds",
                "context_pin","context_pins", "agent_run_state","history",
                "entry",      "system",       "user",           "assistant",
            };
            for (const char* full : k) {
                std::string f(full);
                if (f.size() > n.size() && f.compare(0, n.size(), n) == 0)
                    return true;
            }
            return false;
        };

        size_t after = skipCompleteTag(raw, i);
        if (after == std::string::npos) {
            // Incomplete tag (no '>'). Drop if known or prefix-of-known.
            if (isKnownProtocolName(name) || isPrefixOfKnown(name)) break;
            out.push_back(raw[i++]);
            continue;
        }

        // Try full harness container swallow (result/history/context_feed/…).
        size_t swallowed = skipHarnessContainer(raw, i, name);
        if (swallowed != std::string::npos) {
            i = swallowed;
            continue;
        }

        // Single known tag (open or close) — drop the tag bytes only.
        if (isKnownProtocolName(name)) {
            i = after;
            continue;
        }

        // Unknown complete tag — keep as text (might be code sample).
        out.push_back(raw[i++]);
    }
    return out;
}

// Remove glued attribute debris models leave after stripping tags:
//   final="true"  status="ok"  id="ping1"  ms="1074.0"  bytes="8"
inline std::string stripAttrDebris(const std::string& raw) {
    std::string s = raw;
    // Walk and remove key="value" / key='value' tokens when key is protocol-ish.
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        // Match [A-Za-z_][A-Za-z0-9_]*\s*=\s*"[^"]*"
        if (std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '_') {
            size_t k = i;
            while (k < s.size() && isAlphaNumUs(s[k])) ++k;
            size_t eq = k;
            while (eq < s.size() && isWs(s[eq])) ++eq;
            if (eq < s.size() && s[eq] == '=') {
                size_t q = eq + 1;
                while (q < s.size() && isWs(s[q])) ++q;
                if (q < s.size() && (s[q] == '"' || s[q] == '\'')) {
                    char quote = s[q];
                    size_t end = s.find(quote, q + 1);
                    if (end != std::string::npos) {
                        std::string key = s.substr(i, k - i);
                        if (key == "final" || key == "status" || key == "id" ||
                            key == "ms" || key == "bytes" || key == "mode" ||
                            key == "type" || key == "name" || key == "ephemeral" ||
                            key == "ok" || key == "exit" || key == "turn" ||
                            key == "active") {
                            i = end + 1;
                            continue;
                        }
                    }
                }
            }
        }
        out.push_back(s[i++]);
    }
    return out;
}

// Collapse runs of spaces/tabs; preserve newlines (paragraph structure for UI).
// Old behavior flattened \n → ' ' which made long thoughts one mega-line and
// interacted badly with truncate + wrap (looked like mashed/overlapping text).
inline void collapseWs(std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool prevSpace = true;  // trim leading spaces/tabs on each line
    for (char c : s) {
        if (c == '\n') {
            // trim trailing spaces on the line we just finished
            while (!out.empty() && out.back() == ' ') out.pop_back();
            out.push_back('\n');
            prevSpace = true;  // trim indent-ish leading spaces after newline
            continue;
        }
        if (c == '\r') continue;
        if (c == ' ' || c == '\t') {
            if (!prevSpace) out.push_back(' ');
            prevSpace = true;
        } else {
            out.push_back(c);
            prevSpace = false;
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
    s.swap(out);
}

// Markup density: fraction of original that was tags / angle brackets.
inline double markupDensity(const std::string& raw, const std::string& stripped) {
    if (raw.empty()) return 0.0;
    size_t removed = raw.size() > stripped.size() ? raw.size() - stripped.size() : 0;
    return static_cast<double>(removed) / static_cast<double>(raw.size());
}

inline bool looksLikeHarnessPhrase(const std::string& s) {
    static const char* k[] = {
        "The previous turn's output was discarded",
        "Your new output is the only output that will be processed",
        "Continue from the inline transcript",
        "Use runtime results only",
        "if enough information is available, emit",
        "emit <response final",
        "[AUTO-PROMOTED",
        "[EMPTY RESPONSE]",
        "protocol_error",
        "no result",
        "LLM-requested dynamic context",
    };
    for (const char* p : k)
        if (s.find(p) != std::string::npos) return true;
    return false;
}

// nm / readelf / c++filt dumps: printable ASCII, so binary sanitizer misses them.
// Floods THOUGHT/RESULT and stalls the chat (wrap + rebuild on every tick).
inline bool looksLikeSymbolDump(const std::string& s) {
    if (s.size() < 160) return false;
    int mangled = 0;
    int cxx11 = 0;
    int underscores = 0;
    int alpha = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '_') ++underscores;
        else if (std::isalpha(c)) ++alpha;
        if (i + 3 < s.size() && s[i] == '_' && s[i + 1] == 'Z' &&
            (s[i + 2] == 'N' || s[i + 2] == 'K' || s[i + 2] == 'T' || s[i + 2] == 'S' ||
             s[i + 2] == 'I' || s[i + 2] == 'St'))
            ++mangled;
        if (i + 14 <= s.size() && s.compare(i, 14, "std::__cxx11::") == 0) ++cxx11;
        if (i + 10 <= s.size() && s.compare(i, 10, "basic_string") == 0) ++cxx11;
    }
    if (mangled >= 4) return true;
    if (cxx11 >= 8) return true;
    if (mangled >= 2 && s.size() > 1500) return true;
    // Long underscore-heavy blobs (mangled names packed on few lines).
    if (s.size() > 400 && underscores > 30 && alpha > 0 &&
        underscores * 100 / (underscores + alpha) > 12)
        return true;
    return false;
}

// Main entry: strip protocol noise; return remaining human prose or empty.
inline std::string stripProtocolNoise(const std::string& raw) {
    if (raw.empty()) return {};

    // Symbol tables are not protocol markup — kill early before strip work.
    if (looksLikeSymbolDump(raw)) return {};

    std::string s = stripAllProtocolMarkup(raw);
    s = stripAttrDebris(s);
    collapseWs(s);
    trimInPlace(s);
    if (s.empty()) return {};

    if (looksLikeSymbolDump(s)) return {};

    // Pure harness phrases (often leftover after stripping tags).
    if (looksLikeHarnessPhrase(s) && s.size() < 500) return {};

    // Leftover that is only punctuation / digits / quotes.
    bool anyAlpha = false;
    int alpha = 0;
    for (char c : s)
        if (std::isalpha(static_cast<unsigned char>(c))) {
            anyAlpha = true;
            ++alpha;
        }
    if (!anyAlpha) return {};

    // Density gate: only kill leftovers that are tiny debris after heavy
    // markup strip — never wipe a real multi-word answer just because it
    // sat next to a large <history>/<result> echo.
    double dens = markupDensity(raw, s);
    if (dens > 0.90 && alpha < 12) return {};
    if (dens > 0.95 && s.size() < 24) return {};

    return s;
}

inline bool isOnlyOrphanCloses(const std::string& raw) {
    return stripProtocolNoise(raw).empty() && !raw.empty();
}

inline bool isProtocolEchoBlob(const std::string& raw) {
    return stripProtocolNoise(raw).empty();
}

inline bool isThoughtNoise(const std::string& raw) {
    if (looksLikeSymbolDump(raw)) return true;
    return stripProtocolNoise(raw).empty();
}

}  // namespace cortex::mk3::protocol
