// src/ui/chat/ask_dialog_model.hpp — ask_tool card state and validation (renderer-independent)
#pragma once

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>


namespace cortex::mk3::ui::chat {

struct DialogOption {
    std::string value;
    std::string label;
    std::string description;
    bool disabled = false;
};

struct DialogCard {
    std::string id;
    std::string type = "text";
    std::string title;
    std::string message;
    std::string help;
    std::string defaultValue;
    std::string confirmWord;
    std::vector<DialogOption> options;
    bool required = true;
    int numberMin = 0;
    int numberMax = 0;
    bool hasNumberMin = false;
    bool hasNumberMax = false;
    int minSelect = 0;
    int maxSelect = 0;
};

struct DialogState {
    std::string chainTitle = "Agent asks";
    std::string message;
    std::vector<DialogCard> cards;
    size_t index = 0;
    Json::Value results = Json::objectValue;
    std::string error;
    bool cancelled = false;
    bool completed = false;
    int selectedOption = 0;  // current highlight for choice/multi_choice/ranker

    bool done() const {
        return completed || cancelled;
    }
    const DialogCard* current() const {
        if (index >= cards.size())
            return nullptr;
        return &cards[index];
    }
    bool currentInteractive() const {
        const DialogCard* card = current();
        if (!card)
            return false;
        if (card->type == "note" || card->type == "info" || card->type == "section_header")
            return false;
        return true;
    }
};

static inline std::string trimDialog(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace((unsigned char)s[start]))
        start++;
    size_t end = s.size();
    while (end > start && std::isspace((unsigned char)s[end - 1]))
        end--;
    return s.substr(start, end - start);
}

static inline std::vector<std::string> splitDialogCsv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trimDialog(item);
        if (!item.empty())
            out.push_back(item);
    }
    return out;
}

static inline DialogOption optionFromJson(const Json::Value& v) {
    DialogOption opt;
    if (v.isString()) {
        opt.value = v.asString();
        opt.label = v.asString();
    } else if (v.isObject()) {
        opt.value = v.get("value", v.get("id", "")).asString();
        opt.label = v.get("label", opt.value).asString();
        opt.description = v.get("description", "").asString();
        opt.disabled = v.get("disabled", false).asBool();
    }
    return opt;
}

static inline DialogCard cardFromJson(const Json::Value& v) {
    DialogCard card;
    card.id = v.get("id", "").asString();
    card.type = v.get("type", "text").asString();
    card.title = v.get("title", card.id).asString();
    card.message = v.get("message", v.get("body", "")).asString();
    card.help = v.get("help", "").asString();
    card.defaultValue = v.get("defaultValue", "").asString();
    card.confirmWord = v.get("confirmWord", "CONFIRM").asString();
    card.required = v.get("required", true).asBool();
    card.numberMin = v.get("numberMin", 0).asInt();
    card.numberMax = v.get("numberMax", 0).asInt();
    card.hasNumberMin = v.isMember("numberMin");
    card.hasNumberMax = v.isMember("numberMax");
    card.minSelect = v.get("minSelect", 0).asInt();
    card.maxSelect = v.get("maxSelect", 0).asInt();
    if (card.id.empty())
        card.id = "card_" + std::to_string(0);
    if (card.type.empty())
        card.type = "text";
    if (v.isMember("options")) {
        for (const auto& opt : v["options"])
            card.options.push_back(optionFromJson(opt));
    }
    return card;
}

static inline DialogState parseDialogState(const Json::Value& params) {
    DialogState state;
    state.chainTitle = params.get("title", params.get("chainTitle", "Agent asks")).asString();
    state.message = params.get("message", "").asString();
    if (params.isMember("cards") && params["cards"].isArray()) {
        for (size_t i = 0; i < params["cards"].size(); i++) {
            DialogCard card = cardFromJson(params["cards"][(Json::ArrayIndex)i]);
            if (card.id.empty())
                card.id = "card_" + std::to_string(i);
            state.cards.push_back(card);
        }
    } else {
        DialogCard card;
        card.id = "response";
        card.type = "text";
        card.title = "Type anything";
        card.message = params.get("message", "").asString();
        state.cards.push_back(card);
    }
    return state;
}

static inline void advanceDialog(DialogState& state, const Json::Value& value) {
    const DialogCard* card = state.current();
    if (!card) {
        state.completed = true;
        return;
    }
    if (card->type != "note" && card->type != "info" && card->type != "section_header")
        state.results[card->id] = value;
    state.index++;
    state.error.clear();
    if (state.index >= state.cards.size())
        state.completed = true;
}

static inline void completeNonInteractiveCards(DialogState& state) {
    while (!state.done()) {
        const DialogCard* card = state.current();
        if (!card || state.currentInteractive())
            return;
        advanceDialog(state, Json::Value());
    }
}

static inline bool parseBoolAnswer(const std::string& raw, bool& value) {
    std::string s = trimDialog(raw);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    if (s == "y" || s == "yes" || s == "true" || s == "1" || s == "confirm") {
        value = true;
        return true;
    }
    if (s == "n" || s == "no" || s == "false" || s == "0" || s == "cancel") {
        value = false;
        return true;
    }
    return false;
}

static inline bool handleDialogLine(DialogState& state, const std::string& rawLine) {
    completeNonInteractiveCards(state);
    if (state.done())
        return true;
    const DialogCard* card = state.current();
    if (!card || !state.currentInteractive())
        return false;

    std::string line = rawLine;
    if (line.empty() && !card->defaultValue.empty() &&
        (card->type == "text" || card->type == "textarea" || card->type == "secret" ||
         card->type == "number")) {
        line = card->defaultValue;
    }

    if (card->type == "text" || card->type == "textarea" || card->type == "secret") {
        if (line.empty() && card->required) {
            state.error = "Answer is required. Press Esc to cancel.";
            return false;
        }
        advanceDialog(state, line);
        return true;
    }

    if (card->type == "number") {
        char* end = nullptr;
        double number = std::strtod(line.c_str(), &end);
        if (!end || *end != '\0') {
            state.error = "Enter a number.";
            return false;
        }
        if (card->hasNumberMin && number < card->numberMin) {
            state.error = "Number must be >= " + std::to_string(card->numberMin) + ".";
            return false;
        }
        if (card->hasNumberMax && number > card->numberMax) {
            state.error = "Number must be <= " + std::to_string(card->numberMax) + ".";
            return false;
        }
        advanceDialog(state, number);
        return true;
    }

    if (card->type == "confirm") {
        bool ok = false;
        if (!parseBoolAnswer(line, ok)) {
            state.error = "Type y/yes or n/no.";
            return false;
        }
        advanceDialog(state, ok);
        return true;
    }

    if (card->type == "type_confirm") {
        if (trimDialog(line) != card->confirmWord) {
            state.error = "Type exactly: " + card->confirmWord;
            return false;
        }
        advanceDialog(state, true);
        return true;
    }

    if (card->type == "choice") {
        std::string answer = trimDialog(line);
        if (answer.empty()) {
            state.error = "Choose one option.";
            return false;
        }
        char* end = nullptr;
        long idx = std::strtol(answer.c_str(), &end, 10);
        if (end && *end == '\0' && idx > 0 && (size_t)idx <= card->options.size() &&
            !card->options[idx - 1].disabled) {
            advanceDialog(state, card->options[idx - 1].value);
            return true;
        }
        for (const auto& opt : card->options) {
            if (!opt.disabled && opt.value == answer) {
                advanceDialog(state, opt.value);
                return true;
            }
        }
        state.error = "Unknown choice.";
        return false;
    }

    if (card->type == "multi_choice") {
        auto parts = splitDialogCsv(line);
        if ((int)parts.size() < card->minSelect) {
            state.error = "Select at least " + std::to_string(card->minSelect) + " options.";
            return false;
        }
        if (card->maxSelect > 0 && (int)parts.size() > card->maxSelect) {
            state.error = "Select at most " + std::to_string(card->maxSelect) + " options.";
            return false;
        }
        Json::Value arr(Json::arrayValue);
        for (const auto& part : parts) {
            bool matched = false;
            char* end = nullptr;
            long idx = std::strtol(part.c_str(), &end, 10);
            if (end && *end == '\0' && idx > 0 && (size_t)idx <= card->options.size()) {
                const auto& opt = card->options[idx - 1];
                if (!opt.disabled) {
                    arr.append(opt.value);
                    matched = true;
                }
            } else {
                for (const auto& opt : card->options) {
                    if (!opt.disabled && opt.value == part) {
                        arr.append(opt.value);
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched) {
                state.error = "Unknown choice: " + part;
                return false;
            }
        }
        advanceDialog(state, arr);
        return true;
    }

    if (card->type == "ranker") {
        auto parts = splitDialogCsv(line);
        Json::Value arr(Json::arrayValue);
        std::vector<bool> used(card->options.size(), false);
        for (const auto& part : parts) {
            char* end = nullptr;
            long idx = std::strtol(part.c_str(), &end, 10);
            if (!end || *end != '\0' || idx <= 0 || (size_t)idx > card->options.size() ||
                used[idx - 1]) {
                state.error = "Enter option numbers in order, separated by commas.";
                return false;
            }
            used[idx - 1] = true;
            arr.append(card->options[idx - 1].value);
        }
        advanceDialog(state, arr);
        return true;
    }

    if (card->type == "key_value") {
        Json::Value obj(Json::objectValue);
        for (const auto& part : splitDialogCsv(line)) {
            auto eq = part.find('=');
            if (eq == std::string::npos) {
                state.error = "Use key=value pairs separated by commas.";
                return false;
            }
            obj[trimDialog(part.substr(0, eq))] = trimDialog(part.substr(eq + 1));
        }
        advanceDialog(state, obj);
        return true;
    }

    if (card->type == "note" || card->type == "info" || card->type == "section_header") {
        advanceDialog(state, Json::Value());
        return true;
    }

    state.error = "Unsupported card type: " + card->type;
    return false;
}

}  // namespace cortex::mk3::ui::chat
