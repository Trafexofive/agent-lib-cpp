// src/tools/builtins/web_fetch.hpp — web_fetch builtin entry points
#pragma once

#include <json/json.h>
#include <functional>
#include <string>

namespace cortex::mk3::tools::builtins {

std::string web_fetch(const Json::Value& params);
std::string webFetchStreaming(const Json::Value& params,
                              const std::function<void(const std::string&, bool)>& stream);

}  // namespace cortex::mk3::tools::builtins
