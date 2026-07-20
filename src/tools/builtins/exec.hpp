// src/tools/builtins/exec.hpp — exec builtin entry points
#pragma once

#include <json/json.h>
#include <functional>
#include <string>

namespace cortex::mk3::tools::builtins {

std::string exec(const Json::Value& params);
std::string execStreaming(const Json::Value& params,
                          const std::function<void(const std::string&, bool)>& stream);

}  // namespace cortex::mk3::tools::builtins
