#pragma once
// Protocol noise filter — pure helpers.
//
// Models (esp. small / free-tier) often echo harness scaffolding into the
// stream as bare text or pseudo-thoughts:
//   - orphan closes: </context_feed> </response> </thought> </result>
//   - injected runtime results: <result id="…">…</result>
//   - salvage / recovery banners the runtime itself put in the next prompt
//
// Those must never become Thought / Response timeline rows.

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

// True if the whole (trimmed) blob is only whitespace + zero or more
// complete protocol close tags of known names. No human prose.
inline bool isOnlyOrphanCloses(const std::string& raw) {
    std::string s = raw;
    trimInPlace(s);
    if (s.empty()) return true;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isWs(s[i])) ++i;
        if (i >= s.size()) break;
        if (s[i] != '<' || i + 1 >= s.size() || s[i + 1] != '/') return false;
        size_t gt = s.find('>', i);
        if (gt == std::string::npos) return false;
        // name between </ and >
        std::string name = s.substr(i + 2, gt - (i + 2));
        // strip trailing junk attrs if any (shouldn't appear on closes)
        auto sp = name.find_first_of(" \t/");
        if (sp != std::string::npos) name = name.substr(0, sp);
        if (name != "response" && name != "thought" && name != "think" &&
            name != "thinking" && name != "action" && name != "result" &&
            name != "context_feed" && name != "system" && name != "user") {
            return false;
        }
        i = gt + 1;
    }
    return true;
}

// True if blob is dominated by protocol markup with no meaningful prose.
// Catches: <result …>…</result>, bare <context_feed>, salvage banners
// that are almost entirely tags, etc.
inline bool isProtocolEchoBlob(const std::string& raw) {
    std::string s = raw;
    trimInPlace(s);
    if (s.empty()) return true;
    if (isOnlyOrphanCloses(s)) return true;

    // Pure single open+close pair of a known runtime tag (result / context_feed).
    if (s.size() >= 3 && s[0] == '<') {
        size_t gt = s.find('>');
        if (gt != std::string::npos) {
            std::string open = s.substr(1, gt - 1);
            auto sp = open.find_first_of(" \t/");
            std::string name = sp == std::string::npos ? open : open.substr(0, sp);
            if (name == "result" || name == "context_feed" || name == "system" ||
                name == "agent_run_state" || name == "context_pins") {
                // If almost everything is inside tags (high < density) treat as echo.
                size_t tags = 0, letters = 0;
                for (char c : s) {
                    if (c == '<' || c == '>') ++tags;
                    else if (std::isalnum(static_cast<unsigned char>(c))) ++letters;
                }
                if (tags >= 2 && letters < 24) return true;
                // Full <result…>…</result> with short body → echo of runtime inject.
                if (name == "result" && s.find("</result>") != std::string::npos)
                    return true;
                if (name == "context_feed" &&
                    s.find("</context_feed>") != std::string::npos)
                    return true;
            }
        }
    }

    // Salvage / recovery banners the runtime injects into the next prompt —
    // models often parrot them. Drop when they dominate the blob.
    static const char* kEchoPhrases[] = {
        "The previous turn's output was discarded",
        "Your new output is the only output that will be processed",
        "status=\"salvage\"",
        "id=\"salvage\"",
        "id=\"init\"",
        "[AUTO-PROMOTED",
        "[EMPTY RESPONSE]",
        "protocol_error",
    };
    for (const char* p : kEchoPhrases) {
        if (s.find(p) != std::string::npos) {
            // If the blob is mostly that phrase + tags, drop; if long human
            // text after, keep (model may have continued past the banner).
            if (s.size() < 400) return true;
        }
    }
    return false;
}

// Strip leading/trailing orphan closes and pure tag noise. Returns remaining
// human text, or empty if nothing usable remains.
inline std::string stripProtocolNoise(const std::string& raw) {
    if (raw.empty()) return {};
    if (isProtocolEchoBlob(raw) || isOnlyOrphanCloses(raw)) return {};

    std::string s = raw;
    // Peel orphan closes from the front.
    auto peelFront = [](std::string& t) {
        trimInPlace(t);
        while (!t.empty() && t[0] == '<' && t.size() > 1 && t[1] == '/') {
            size_t gt = t.find('>');
            if (gt == std::string::npos) break;
            std::string name = t.substr(2, gt - 2);
            auto sp = name.find_first_of(" \t/");
            if (sp != std::string::npos) name = name.substr(0, sp);
            if (name != "response" && name != "thought" && name != "think" &&
                name != "thinking" && name != "action" && name != "result" &&
                name != "context_feed" && name != "system" && name != "user") {
                break;
            }
            t = t.substr(gt + 1);
            trimInPlace(t);
        }
    };
    auto peelBack = [](std::string& t) {
        trimInPlace(t);
        while (!t.empty()) {
            size_t lt = t.rfind("</");
            if (lt == std::string::npos) break;
            size_t gt = t.find('>', lt);
            if (gt != t.size() - 1) break;  // not a trailing close
            // only whitespace after? gt is last char
            std::string name = t.substr(lt + 2, gt - (lt + 2));
            auto sp = name.find_first_of(" \t/");
            if (sp != std::string::npos) name = name.substr(0, sp);
            if (name != "response" && name != "thought" && name != "think" &&
                name != "thinking" && name != "action" && name != "result" &&
                name != "context_feed" && name != "system" && name != "user") {
                break;
            }
            // ensure only whitespace between previous content end and this close
            size_t end = lt;
            while (end > 0 && isWs(t[end - 1])) --end;
            t = t.substr(0, end);
            trimInPlace(t);
        }
    };
    peelFront(s);
    peelBack(s);
    if (isProtocolEchoBlob(s) || isOnlyOrphanCloses(s)) return {};
    return s;
}

// True if this content should never become a Thought/Response row.
inline bool isThoughtNoise(const std::string& raw) {
    return stripProtocolNoise(raw).empty();
}

}  // namespace cortex::mk3::protocol
