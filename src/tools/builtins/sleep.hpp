// src/tools/builtins/sleep.hpp — sleep builtin entry points
#pragma once

#include <json/json.h>
#include <functional>
#include <string>

namespace cortex::mk3::tools::builtins {

std::string sleep(const Json::Value& params);
std::string sleepStreaming(const Json::Value& params,
                           const std::function<void(const std::string&, bool)>& stream);

}  // namespace cortex::mk3::tools::builtins
