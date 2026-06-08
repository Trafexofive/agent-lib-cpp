// Minimal streaming parser debug
#include "src/protocol/parser.hpp"
#include <iostream>
using namespace cortex::mk3::protocol;

int main() {
    std::string responseText;
    bool responseFinal = false;

    Parser parser([](const ParsedAction& a) -> Json::Value {
        Json::Value r; r["ok"] = true; return r;
    });
    parser.onEvent([&](const TokenEvent& ev) {
        std::cerr << "[EVENT] type=";
        switch (ev.type) {
            case TokenEvent::TEXT: std::cerr << "TEXT"; break;
            case TokenEvent::THOUGHT: std::cerr << "THOUGHT"; break;
            case TokenEvent::ACTION_START: std::cerr << "ACTION_START"; break;
            case TokenEvent::ACTION_RESULT: std::cerr << "ACTION_RESULT"; break;
            case TokenEvent::RESPONSE: std::cerr << "RESPONSE"; break;
            case TokenEvent::ERROR: std::cerr << "ERROR"; break;
        }
        std::cerr << " content='" << ev.content.substr(0, 50) << "'";
        if (!ev.metadata.empty()) {
            std::cerr << " meta={";
            for (auto& [k,v] : ev.metadata) std::cerr << k << "=" << v << " ";
            std::cerr << "}";
        }
        std::cerr << "\n";

        if (ev.type == TokenEvent::RESPONSE) {
            responseText += ev.content;
            if (ev.metadata.count("is_final") && ev.metadata.at("is_final") == "true")
                responseFinal = true;
        }
    });

    std::cerr << "=== Test: 2-token response ===\n";
    parser.feed("<response final=\"true\">Hi</response>", false);
    parser.feed("", true);

    std::cerr << "responseText='" << responseText << "' final=" << responseFinal << "\n";
    parser.reset(); responseText.clear(); responseFinal = false;

    std::cerr << "\n=== Test: character-by-character response ===\n";
    const char* chars = "<response final=\"true\">Hi</response>";
    for (const char* p = chars; *p; p++) {
        parser.feed(std::string(1, *p), false);
    }
    parser.feed("", true);
    std::cerr << "responseText='" << responseText << "' final=" << responseFinal << "\n";

    return 0;
}
