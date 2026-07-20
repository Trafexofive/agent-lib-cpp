// src/tools/builtins/json.hpp — json builtin entry points
#pragma once

#include <json/json.h>
#include <functional>
#include <string>

namespace cortex::mk3::tools::builtins {

std::string json(const Json::Value& params);
std::string jsonStreaming(const Json::Value& params,
                          const std::function<void(const std::string&, bool)>& stream);

}  // namespace cortex::mk3::tools::builtins
