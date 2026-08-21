// ask_tool stdin fallback — TTY only. TUI owns the real path via AgentBridge.
#include "ask_tool.hpp"
#include "common.hpp"

#include <cctype>
#include <cstdlib>
#include <iostream>

#include "../ask_protocol.hpp"

namespace cortex::mk3::tools::builtins {

static std::string readLineOrDefault(const Json::Value& card) {
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        if (card.isMember("defaultValue"))
            return card["defaultValue"].asString();
        if (card.isMember("default"))
            return card["default"].asString();
        return {};
    }
    if (answer.empty()) {
        if (card.isMember("defaultValue"))
            return card["defaultValue"].asString();
        if (card.isMember("default"))
            return card["default"].asString();
    }
    return answer;
}

static std::string optionLabel(const Json::Value& opt) {
    if (opt.isString())
        return opt.asString();
    if (opt.isObject())
        return opt.get("label", opt.get("value", opt.get("id", ""))).asString();
    return {};
}

static std::string optionValue(const Json::Value& opt) {
    if (opt.isString())
        return opt.asString();
    if (opt.isObject())
        return opt.get("value", opt.get("id", opt.get("label", ""))).asString();
    return {};
}

std::string ask_tool(const Json::Value& p) {
    return askToolStreaming(p, {});
}

std::string askToolStreaming(const Json::Value& raw,
                             const std::function<void(const std::string&, bool)>& stream) {
    Json::Value p = tools::normalizeAskParams(raw);
    if (p.isMember("cards") && !p["cards"].isArray())
        return jsonErr("cards must be an array");

    const std::string title = p.get("title", "Agent asks").asString();
    const std::string message = p.get("message", "").asString();
    if (stream) {
        stream("ask_tool: " + title + "\n", false);
        if (!message.empty())
            stream(message + "\n", false);
    }
    std::cerr << "ask_tool: " << title << "\n";
    if (!message.empty())
        std::cerr << message << "\n";

    Json::Value results(Json::objectValue);
    const Json::Value& cards = p["cards"];
    for (Json::ArrayIndex i = 0; i < cards.size(); ++i) {
        const auto& card = cards[i];
        if (!card.isObject())
            return jsonErr("card " + std::to_string(i) + " must be an object");
        std::string id = card.get("id", "").asString();
        if (id.empty())
            return jsonErr("card " + std::to_string(i) + " missing id");
        const std::string type = card.get("type", "text").asString();
        if (type == "note" || type == "info" || type == "section_header")
            continue;

        const std::string ct = card.get("title", id).asString();
        const std::string cm = card.get("message", "").asString();
        std::cerr << "[" << id << "] " << ct << "\n";
        if (!cm.empty())
            std::cerr << cm << "\n";
        if (stream) {
            stream("[" + id + "] " + ct + "\n", false);
            if (!cm.empty())
                stream(cm + "\n", false);
        }

        if (type == "choice" && card.isMember("options") && card["options"].isArray()) {
            for (Json::ArrayIndex o = 0; o < card["options"].size(); ++o) {
                std::cerr << "  " << (o + 1) << ") " << optionLabel(card["options"][o]) << "\n";
            }
        } else if (type == "confirm") {
            std::cerr << "  y/n\n";
        } else if (type == "type_confirm") {
            std::cerr << "  type exactly: " << card.get("confirmWord", "CONFIRM").asString()
                      << "\n";
        }

        std::cerr << "> " << std::flush;
        std::string line = readLineOrDefault(card);

        if (type == "choice" && card.isMember("options")) {
            char* end = nullptr;
            long idx = std::strtol(line.c_str(), &end, 10);
            if (end && *end == '\0' && idx > 0 &&
                static_cast<Json::ArrayIndex>(idx) <= card["options"].size())
                line = optionValue(card["options"][static_cast<Json::ArrayIndex>(idx - 1)]);
        } else if (type == "confirm") {
            std::string s = line;
            for (char& c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (s == "y" || s == "yes" || s == "true" || s == "1")
                results[id] = true;
            else
                results[id] = false;
            continue;
        } else if (type == "type_confirm") {
            const std::string word = card.get("confirmWord", "CONFIRM").asString();
            results[id] = (line == word);
            continue;
        }
        results[id] = line;
    }

    return jsonStr(tools::askResult(true, false, false, results));
}

}  // namespace cortex::mk3::tools::builtins
