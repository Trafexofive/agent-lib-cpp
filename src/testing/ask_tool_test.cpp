// =============================================================================
// ask_tool built-in tool test
// =============================================================================
#include <iostream>
#include <sstream>
#include <json/json.h>
#include "../tools/registry.hpp"

int main() {
    std::cout << "=== ask_tool built-in tool test ===\n";
    int passed = 0, failed = 0;
    auto check = [&](bool c, const std::string& n) {
        if (c) { passed++; std::cout << "  PASS: " << n << "\n"; }
        else   { failed++; std::cout << "  FAIL: " << n << "\n"; }
    };

    cortex::mk3::tools::registerDefaults();

    auto& reg = cortex::mk3::tools::ToolRegistry::instance();
    auto fn = reg.get("ask_tool");
    check(fn != nullptr, "ask_tool registered in ToolRegistry");

    // Test: card chain with text input (simulate via stdin)
    Json::Value params;
    params["cards"] = Json::Value(Json::arrayValue);
    Json::Value card;
    card["id"] = "q1";
    card["type"] = "text";
    card["title"] = "Test question";
    card["message"] = "What is your answer?";
    params["cards"].append(card);

    // Write mock stdin
    std::istringstream mockInput("test-answer\n");
    std::streambuf* origCin = std::cin.rdbuf(mockInput.rdbuf());

    std::string output = fn(params);

    std::cin.rdbuf(origCin);

    Json::Value result;
    Json::CharReaderBuilder r;
    std::string errs;
    std::istringstream ss(output);
    check(Json::parseFromStream(r, ss, &result, &errs), "ask_tool returns valid JSON");
    check(result["success"].asBool(), "ask_tool succeeds");
    check(result["results"].isObject(), "ask_tool has results object");
    check(result["results"]["q1"].asString() == "test-answer", "ask_tool captures user input");

    std::cout << "\n  " << passed << "/" << (passed + failed) << " passed\n";
    return failed == 0 ? 0 : 1;
}
