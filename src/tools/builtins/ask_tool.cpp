// src/tools/builtins/ask_tool.cpp — ask_tool native builtin fallback
#include "builtins.hpp"
#include "common.hpp"

#include <iostream>

#include "../../utils/ansi.hpp"

namespace cortex::mk3::tools::builtins {

std::string ask_tool(const Json::Value& p) {
    std::string title = p.get("title", "Agent asks:").asString();
    std::string message = p.get("message", "").asString();
    std::cerr << "\n" << ansi::bold << ansi::green << "[AGENT] " << ansi::reset << title << "\n";
    if (!message.empty())
        std::cerr << ansi::dim << message << ansi::reset << "\n";

    Json::Value results;
    if (p.isMember("cards")) {
        const Json::Value& cards = p["cards"];
        for (const auto& card : cards) {
            std::string id = card.get("id", "").asString();
            std::string ct = card.get("title", id).asString();
            std::string cm = card.get("message", "").asString();
            std::cerr << "\n" << ansi::cyan << "[" << id << "]" << ansi::reset << " " << ct << "\n";
            if (!cm.empty())
                std::cerr << ansi::dim << cm << ansi::reset << "\n";
            std::cerr << ansi::bold << ansi::green << "> " << ansi::reset << std::flush;
            std::string answer;
            if (!std::getline(std::cin, answer) || answer.empty()) {
                results[id] = "";
                break;
            }
            results[id] = answer;
        }
    } else {
        std::cerr << ansi::bold << ansi::green << "> " << ansi::reset << std::flush;
        std::string response;
        if (!std::getline(std::cin, response))
            response = "";
        results["response"] = response;
    }

    Json::Value out;
    out["success"] = true;
    out["results"] = results;
    return jsonStr(out);
}

}  // namespace cortex::mk3::tools::builtins
