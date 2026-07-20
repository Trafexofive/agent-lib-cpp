// src/tools/builtins/ask_tool.cpp — ask_tool native builtin fallback
#include "ask_tool.hpp"
#include "common.hpp"

#include <iostream>

#include "../../utils/ansi.hpp"

namespace cortex::mk3::tools::builtins {

static std::string readAnswer(const Json::Value& card) {
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        if (card.isMember("default"))
            return card["default"].asString();
        if (card.isMember("defaultValue"))
            return card["defaultValue"].asString();
        return "";
    }
    if (answer.empty()) {
        if (card.isMember("default"))
            return card["default"].asString();
        if (card.isMember("defaultValue"))
            return card["defaultValue"].asString();
    }
    return answer;
}

std::string ask_tool(const Json::Value& p) {
    return askToolStreaming(p, {});
}

std::string askToolStreaming(const Json::Value& p,
                             const std::function<void(const std::string&, bool)>& stream) {
    if (p.isMember("cards") && !p["cards"].isArray())
        return jsonErr("cards must be an array");

    std::string title = p.get("title", "Agent asks:").asString();
    std::string message = p.get("message", "").asString();
    if (stream) {
        stream("ask_tool: " + title + "\n", false);
        if (!message.empty())
            stream(message + "\n", false);
    }
    std::cerr << "\n" << ansi::bold << ansi::green << "[AGENT] " << ansi::reset << title << "\n";
    if (!message.empty())
        std::cerr << ansi::dim << message << ansi::reset << "\n";

    Json::Value results(Json::objectValue);
    Json::Value answered(Json::arrayValue);
    if (p.isMember("cards")) {
        const Json::Value& cards = p["cards"];
        for (Json::ArrayIndex i = 0; i < cards.size(); ++i) {
            const auto& card = cards[i];
            if (!card.isObject())
                return jsonErr("card " + std::to_string(i) + " must be an object");
            std::string id = card.get("id", "").asString();
            if (id.empty())
                return jsonErr("card " + std::to_string(i) + " missing id");
            std::string ct = card.get("title", id).asString();
            std::string cm = card.get("message", "").asString();
            if (stream) {
                stream("[" + id + "] " + ct + "\n", false);
                if (!cm.empty())
                    stream(cm + "\n", false);
            }
            std::cerr << "\n" << ansi::cyan << "[" << id << "]" << ansi::reset << " " << ct << "\n";
            if (!cm.empty())
                std::cerr << ansi::dim << cm << ansi::reset << "\n";
            std::cerr << ansi::bold << ansi::green << "> " << ansi::reset << std::flush;
            results[id] = readAnswer(card);
            answered.append(id);
        }
    } else {
        std::cerr << ansi::bold << ansi::green << "> " << ansi::reset << std::flush;
        Json::Value pseudo;
        pseudo["default"] = p.get("default", "").asString();
        results["response"] = readAnswer(pseudo);
        answered.append("response");
    }

    Json::Value out;
    out["success"] = true;
    out["results"] = results;
    out["answered"] = answered;
    out["count"] = static_cast<Json::UInt64>(answered.size());
    return jsonStr(out);
}

}  // namespace cortex::mk3::tools::builtins
