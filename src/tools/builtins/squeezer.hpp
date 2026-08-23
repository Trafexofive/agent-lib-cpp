// src/tools/builtins/squeezer.hpp — signature-only functional map
#pragma once

#include <json/json.h>
#include <functional>
#include <string>

namespace cortex::mk3::tools::builtins {

std::string squeezer(const Json::Value& params);
std::string squeezerStreaming(const Json::Value& params,
                              const std::function<void(const std::string&, bool)>& stream);

}  // namespace cortex::mk3::tools::builtins
