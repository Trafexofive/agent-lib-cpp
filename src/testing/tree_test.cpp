// Native tree builtin — FAIL if bare "tree" is unknown (dump 1787595442342).
#include <json/json.h>

#include <iostream>
#include <sstream>
#include <string>

#include "src/tools/registry.hpp"

static int passed = 0, failed = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        std::cout << "  " << (msg) << "... ";                                  \
        if (cond) {                                                            \
            ++passed;                                                          \
            std::cout << "PASS\n";                                             \
        } else {                                                               \
            ++failed;                                                          \
            std::cout << "FAIL\n";                                             \
        }                                                                      \
    } while (0)

static Json::Value parse(const std::string& raw) {
    Json::Value v;
    Json::CharReaderBuilder r;
    std::string err;
    std::istringstream ss(raw);
    Json::parseFromStream(r, ss, &v, &err);
    return v;
}

int main() {
    std::cout << "tree_test\n";
    cortex::mk3::tools::registerDefaults();
    auto fn = cortex::mk3::tools::ToolRegistry::instance().get("tree");
    CHECK(fn != nullptr, "tree registered in ToolRegistry");
    if (!fn) {
        std::cout << passed << " passed, " << failed << " failed\n";
        return 1;
    }

    Json::Value miss;
    miss["path"] = "/no/such/tree/path";
    auto missJ = parse(fn(miss));
    CHECK(missJ["success"].asBool() == false, "missing path fails");

    Json::Value file;
    file["path"] = "Makefile";
    auto fileJ = parse(fn(file));
    CHECK(fileJ["success"].asBool() == false, "file is not a directory");

    Json::Value src;
    src["path"] = "src";
    src["max_depth"] = 0;  // immediate children only
    src["max_entries"] = 80;
    src["format"] = "tree";
    auto srcJ = parse(fn(src));
    CHECK(srcJ["success"].asBool(), "tree src depth 1 succeeds");
    std::string ascii = srcJ["tree"].asString();
    CHECK(ascii.find("core") != std::string::npos, "ascii mentions src/core");
    CHECK(ascii.find("tools") != std::string::npos, "ascii mentions src/tools");
    CHECK(srcJ["stats"]["dirs"].asInt() > 0, "stats.dirs > 0");

    Json::Value js;
    js["path"] = "src/tools";
    js["max_depth"] = 1;
    js["format"] = "json";
    auto jsJ = parse(fn(js));
    CHECK(jsJ["success"].asBool(), "format=json succeeds");
    CHECK(jsJ["tree"].isObject(), "json tree is an object");
    CHECK(jsJ["tree"]["type"].asString() == "dir", "json root is a dir");

    std::string unk = cortex::mk3::tools::dispatch("tree_not_a_tool", src);
    CHECK(unk.find("unknown tool") != std::string::npos, "unknown name still errors");

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
