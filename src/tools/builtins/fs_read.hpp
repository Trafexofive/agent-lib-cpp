// src/tools/builtins/fs_read.hpp — fs_read builtin entry points
#pragma once

#include <json/json.h>
#include <functional>
#include <string>

namespace cortex::mk3::tools::builtins {

std::string fs_read(const Json::Value& params);
std::string fsReadStreaming(const Json::Value& params,
                            const std::function<void(const std::string&, bool)>& stream);

}  // namespace cortex::mk3::tools::builtins
