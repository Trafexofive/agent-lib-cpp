// =============================================================================
// call-tool — feed script helper. Calls built-in tools, returns JSON to stdout.
// Usage: call-tool <tool_name> '<json_params>'
// Example: call-tool grep '{"pattern":"TODO","path":"src/"}'
// =============================================================================
#include "../tools/registry.hpp"
#include "../tools/dispatch.hpp"
#include <json/json.h>
#include <iostream>
#include <sstream>
#include <cstdlib>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: call-tool <tool_name> '<json_params>'\n";
        return 1;
    }

    std::string toolName = argv[1];
    std::string paramsJson = (argc >= 3) ? argv[2] : "{}";

    // Register built-in tools
    cortex::mk3::tools::registerDefaults();

    // Parse params
    Json::Value params;
    Json::CharReaderBuilder r;
    std::string errs;
    std::istringstream ss(paramsJson);
    if (!Json::parseFromStream(r, ss, &params, &errs)) {
        Json::Value err;
        err["success"] = false;
        err["error"] = "Invalid JSON params: " + errs;
        std::cout << Json::writeString(Json::StreamWriterBuilder(), err) << "\n";
        return 1;
    }

    // Dispatch
    auto& reg = cortex::mk3::tools::ToolRegistry::instance();
    auto fn = reg.get(toolName);
    if (!fn) {
        Json::Value err;
        err["success"] = false;
        err["error"] = "Unknown tool: " + toolName;
        std::cout << Json::writeString(Json::StreamWriterBuilder(), err) << "\n";
        return 1;
    }

    std::string result = fn(params);
    std::cout << result << "\n";
    return 0;
}
