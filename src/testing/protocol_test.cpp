// =============================================================================
// agent-lib-MK3 — Protocol Test Runner
// Streams protocol XML through the MK3 parser, validates expectations.
// No LLM, no API calls — pure deterministic parser validation.
// Ported from MK2, stripped of bloat.
// =============================================================================

#include "src/protocol/parser.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <filesystem>
#include <algorithm>

using namespace cortex::mk3::protocol;

struct TestResult {
    std::string id, name, error;
    bool passed = false;
    std::vector<std::string> events;
    std::vector<std::string> actions;
};

TestResult runProtocolTest(const std::string& filePath) {
    TestResult r;

    // Read file
    std::ifstream f(filePath);
    if (!f) { r.error = "cannot open file"; return r; }
    std::stringstream buf; buf << f.rdbuf();
    std::string content = buf.str();

    // Extract test name from "# Test N: description"
    size_t nameStart = content.find("# Test ");
    if (nameStart != std::string::npos) {
        size_t colon = content.find(':', nameStart);
        size_t nl = content.find('\n', nameStart);
        if (colon != std::string::npos && nl != std::string::npos)
            r.name = content.substr(colon + 1, nl - colon - 1);

        size_t numEnd = content.find_first_of(" :-", nameStart + 7);
        if (numEnd != std::string::npos) {
            std::string num = content.substr(nameStart + 7, numEnd - nameStart - 7);
            r.id = "test_" + num;
        }
    }

    // Extract XML section (before ---EXPECTATIONS---)
    size_t expPos = content.find("---EXPECTATIONS---");
    std::string xmlSection = (expPos != std::string::npos)
        ? content.substr(0, expPos) : content;

    std::string xml;
    std::istringstream xss(xmlSection);
    std::string line;
    while (std::getline(xss, line)) {
        if (!line.empty() && line[0] == '#') continue;  // skip comments
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        xml += line + "\n";
    }

    // Trim
    size_t xs = xml.find_first_not_of(" \t\n\r");
    size_t xe = xml.find_last_not_of(" \t\n\r");
    if (xs != std::string::npos && xe != std::string::npos)
        xml = xml.substr(xs, xe - xs + 1);

    // ── Parser setup ──
    Parser parser([&r](const ParsedAction& action) -> Json::Value {
        r.actions.push_back(action.name);
        Json::Value v;
        v["success"] = true;
        v["result"] = "mocked_" + action.name;
        return v;
    });

    // Track unique event types
    bool sawThought = false, sawResponse = false, sawText = false;
    parser.onEvent([&](const TokenEvent& ev) {
        std::string label;
        switch (ev.type) {
        case TokenEvent::TEXT:
            if (!sawText) { r.events.push_back("TEXT"); sawText = true; }
            break;
        case TokenEvent::THOUGHT:
            if (!sawThought) { r.events.push_back("THOUGHT"); sawThought = true; }
            break;
        case TokenEvent::ACTION_START:
            r.events.push_back("ACTION_START");
            break;
        case TokenEvent::ACTION_RESULT:
            r.events.push_back("ACTION_RESULT");
            break;
        case TokenEvent::RESPONSE:
            if (!sawResponse) { r.events.push_back("RESPONSE"); sawResponse = true; }
            break;
        case TokenEvent::CONTEXT_FEED:
            r.events.push_back("CONTEXT_FEED");
            break;
        case TokenEvent::ERROR:
            r.events.push_back("ERROR");
            break;
        default: break;
        }
    });

    // ── Stream XML through parser ──
    // Feed by complete tags (not arbitrary token splits — this tests tag-level parsing)
    std::string token;
    bool inTag = false;
    for (char c : xml) {
        token += c;
        if (c == '<') inTag = true;
        if (c == '>' && inTag) {
            parser.feed(token, false);
            token.clear();
            inTag = false;
        }
    }
    if (!token.empty()) parser.feed(token, false);
    parser.feed("", true);  // final flush
    parser.waitForActions();

    // Also collect action names from events
    std::string actionLabel;
    for (const auto& a : r.actions) {
        actionLabel = "ACTION(" + a + ")";
        if (std::find(r.events.begin(), r.events.end(), actionLabel) == r.events.end())
            r.events.push_back(actionLabel);
    }

    // ── Validate expectations ──
    std::vector<std::string> errors;
    if (expPos != std::string::npos) {
        std::string exp = content.substr(expPos + 18);  // ---EXPECTATIONS--- = 18 chars

        // events: THOUGHT, RESPONSE
        size_t evPos = exp.find("events:");
        if (evPos != std::string::npos) {
            size_t nl = exp.find('\n', evPos);
            std::string expectedStr = exp.substr(evPos + 7, nl - evPos - 7);

            std::vector<std::string> expected;
            std::istringstream ess(expectedStr);
            std::string item;
            while (std::getline(ess, item, ',')) {
                size_t s = item.find_first_not_of(" \t");
                size_t e = item.find_last_not_of(" \t");
                if (s != std::string::npos)
                    expected.push_back(item.substr(s, e - s + 1));
            }

            for (const auto& expEv : expected) {
                bool found = false;
                for (const auto& actEv : r.events) {
                    if (expEv == actEv ||
                        (expEv.find("ACTION") != std::string::npos && actEv.find("ACTION") != std::string::npos)) {
                        found = true; break;
                    }
                }
                if (!found) errors.push_back("Missing event: " + expEv);
            }
        }

        // output_contains: "text"
        // (not validated here — requires actual LLM output, not parser-only test)

        // complete: true
        if (exp.find("complete: true") != std::string::npos) {
            bool hasResponse = std::find(r.events.begin(), r.events.end(), "RESPONSE") != r.events.end();
            if (!hasResponse) errors.push_back("Missing final response");
        }
    }

    r.passed = errors.empty();
    if (!errors.empty()) r.error = errors[0];

    return r;
}

// ═══════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════
int main(int argc, char* argv[]) {
    std::string protocolDir = "tests/protocols";
    std::string specificProtocol;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-d" || arg == "--dir") protocolDir = argv[++i];
        else if (arg == "-p") specificProtocol = argv[++i];
        else if (arg == "--list" || arg == "-l") {
            for (auto& e : std::filesystem::directory_iterator(protocolDir)) {
                if (e.path().extension() == ".protocol")
                    std::cout << "  " << e.path().stem().string() << "\n";
            }
            return 0;
        }
    }

    std::vector<TestResult> results;

    if (!specificProtocol.empty()) {
        std::string path = specificProtocol;
        if (path.find(".protocol") == std::string::npos)
            path = protocolDir + "/" + specificProtocol + ".protocol";
        results.push_back(runProtocolTest(path));
    } else {
        for (auto& e : std::filesystem::directory_iterator(protocolDir)) {
            if (e.path().extension() == ".protocol")
                results.push_back(runProtocolTest(e.path().string()));
        }
    }

    int passed = 0;
    for (auto& r : results) {
        std::cout << "  " << r.id << " — " << r.name << " ... "
                  << (r.passed ? "PASS" : "FAIL");
        if (!r.error.empty()) std::cout << " (" << r.error << ")";
        std::cout << "  [" << r.events.size() << " events";
        if (!r.actions.empty()) std::cout << ", " << r.actions.size() << " actions";
        std::cout << "]\n";
        if (r.passed) passed++;
    }

    std::cout << "\n  " << passed << "/" << results.size() << " passed\n";
    return passed == (int)results.size() ? 0 : 1;
}
