// src/tools/builtins/grep.hpp — grep builtin entry points
#pragma once

#include <json/json.h>
#include <functional>
#include <string>

namespace cortex::mk3::tools::builtins {

std::string grep(const Json::Value& params);
std::string grepStreaming(const Json::Value& params,
                          const std::function<void(const std::string&, bool)>& stream);

}  // namespace cortex::mk3::tools::builtins
