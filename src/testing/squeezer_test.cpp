// Native squeezer builtin — FAIL if bare "squeezer" is unknown.
#include <json/json.h>

#include <fstream>
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
    std::cout << "squeezer_test\n";
    cortex::mk3::tools::registerDefaults();
    auto& reg = cortex::mk3::tools::ToolRegistry::instance();
    auto fn = reg.get("squeezer");
    CHECK(fn != nullptr, "squeezer registered in ToolRegistry");
    if (!fn) {
        std::cout << passed << " passed, " << failed << " failed\n";
        return 1;
    }

    Json::Value miss;
    miss["path"] = "/no/such/squeezer/path";
    auto missJ = parse(fn(miss));
    CHECK(missJ["success"].asBool() == false, "missing path fails");
    CHECK(missJ["error"].asString().find("not found") != std::string::npos,
          "missing path says not found");

    Json::Value nop;
    auto nopJ = parse(fn(nop));
    CHECK(nopJ["success"].asBool() == false, "path is required");

    Json::Value self;
    self["path"] = "src/tools/builtins/squeezer.cpp";
    self["recursive"] = false;
    auto selfJ = parse(fn(self));
    CHECK(selfJ["success"].asBool(), "squeeze this .cpp succeeds");
    CHECK(selfJ["stats"]["total_signatures"].asInt() > 0, "cpp yields signatures");
    std::string dump = selfJ["output"].asString();
    CHECK(dump.find("squeezerStreaming") != std::string::npos ||
              dump.find("squeezePython") != std::string::npos ||
              dump.find("squeezeCFamily") != std::string::npos,
          "cpp map mentions a squeezer symbol");

    Json::Value md;
    md["path"] = "Makefile";
    auto mdJ = parse(fn(md));
    CHECK(mdJ["success"].asBool(), "unsupported single file still succeeds");
    CHECK(mdJ["files"].size() == 1, "one skipped file record");
    CHECK(mdJ["files"][0].isMember("skipped_reason"), "Makefile skipped_reason");

    std::string alias = cortex::mk3::tools::dispatch("squeeze", self);
    auto aliasJ = parse(alias);
    CHECK(aliasJ["success"].asBool(), "alias squeeze → squeezer");

    std::string unk = cortex::mk3::tools::dispatch("squeezer_not_a_tool", self);
    CHECK(unk.find("unknown tool") != std::string::npos, "unknown name still errors");

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
