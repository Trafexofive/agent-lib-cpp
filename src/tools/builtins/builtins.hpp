// src/tools/builtins/builtins.hpp — native builtin tool entry points
#pragma once

#include <json/json.h>
#include <string>

namespace cortex::mk3::tools::builtins {

std::string exec(const Json::Value& params);
std::string list(const Json::Value& params);
std::string grep(const Json::Value& params);
std::string fs_read(const Json::Value& params);
std::string fs_write(const Json::Value& params);
std::string json(const Json::Value& params);
std::string web_fetch(const Json::Value& params);
std::string ask_tool(const Json::Value& params);
std::string sleep(const Json::Value& params);

}  // namespace cortex::mk3::tools::builtins
