// list builtin — FAIL if repo-root listing is 98MB binaries before src/.
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

static bool hasPath(const Json::Value& entries, const std::string& name) {
    for (const auto& e : entries)
        if (e["name"].asString() == name || e["path"].asString() == name)
            return true;
    return false;
}

int main() {
    std::cout << "list_test\n";
    cortex::mk3::tools::registerDefaults();
    auto fn = cortex::mk3::tools::ToolRegistry::instance().get("list");
    CHECK(fn != nullptr, "list registered");
    if (!fn) return 1;

    Json::Value root;
    root["path"] = ".";
    root["max_entries"] = 50;
    auto j = parse(fn(root));
    CHECK(j["success"].asBool(), "list . succeeds");
    CHECK(hasPath(j["entries"], "src"), "src is present");
    CHECK(hasPath(j["entries"], "docs"), "docs is present");
    CHECK(!hasPath(j["entries"], "build"), "build skipped as noise");
    CHECK(!hasPath(j["entries"], ".git"), ".git skipped");
    CHECK(!hasPath(j["entries"], "cortex-mk3"), "98MB binary skipped");
    CHECK(!hasPath(j["entries"], "libagent-mk3.so"), ".so skipped");
    CHECK(!hasPath(j["entries"], "libagent-mk3.a"), ".a skipped");
    CHECK(j["skipped_noise"].asInt() + j["skipped_large"].asInt() > 0,
          "noise counters increment");

    // dirs first
    CHECK(j["entries"].size() > 1, "more than one entry");
    if (j["entries"].size() > 1) {
        CHECK(j["entries"][0]["type"].asString() == "dir", "first entry is a dir");
    }

    Json::Value src;
    src["path"] = "src";
    auto s = parse(fn(src));
    CHECK(s["success"].asBool(), "list src succeeds");
    CHECK(hasPath(s["entries"], "core"), "src/core present");
    CHECK(hasPath(s["entries"], "tools"), "src/tools present");

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
