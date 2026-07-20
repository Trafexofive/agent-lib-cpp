// src/tools/builtins/ask_tool.hpp — ask_tool builtin entry points
#pragma once

#include <json/json.h>
#include <functional>
#include <string>

namespace cortex::mk3::tools::builtins {

std::string ask_tool(const Json::Value& params);
std::string askToolStreaming(const Json::Value& params,
                             const std::function<void(const std::string&, bool)>& stream);

}  // namespace cortex::mk3::tools::builtins
