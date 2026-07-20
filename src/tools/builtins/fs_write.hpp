// src/tools/builtins/fs_write.hpp — fs_write builtin entry points
#pragma once

#include <json/json.h>
#include <functional>
#include <string>

namespace cortex::mk3::tools::builtins {

std::string fs_write(const Json::Value& params);
std::string fsWriteStreaming(const Json::Value& params,
                             const std::function<void(const std::string&, bool)>& stream);

}  // namespace cortex::mk3::tools::builtins
