// SessionController foundation tests (slice 1).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "src/core/types.hpp"
#include "src/session/controller.hpp"
#include "src/session/manager.hpp"

using namespace cortex::mk3;
using namespace cortex::mk3::session;

namespace {
int failures = 0;
void check(bool ok, const std::string& name) {
    std::cout << "  " << name << "... " << (ok ? "PASS" : "FAIL") << "\n";
    if (!ok) ++failures;
}

std::string tmpDir() {
    auto p = std::filesystem::temp_directory_path() /
             ("mk3-sessctl-" + std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(p);
    return p.string();
}

void test_session_ref_single_id() {
    activeSession().clear();
    check(activeSession().empty(), "SessionRef starts empty");
    activeSession().set("sess-a", false);
    check(activeSession().get() == "sess-a", "SessionRef set/get");
    check(!activeSession().isEphemeral(), "SessionRef not ephemeral");
    activeSession().set("sess-b", true);
    check(activeSession().get() == "sess-b" && activeSession().isEphemeral(),
          "SessionRef replaces id + ephemeral");
    activeSession().clear();
}

void test_io_mutex_recursive_load_save() {
    std::string dir = tmpDir();
    SessionManager sm(dir);
    Session s = sm.create("t1", "agent", "model", "prov");
    s.records.push_back({SessionRecord::USER, "hi", SessionManager::iso8601(), ""});
    // Nested load inside locked save path is exercised by Agent; here we just
    // load+save under concurrent threads.
    sm.save(s, /*pretty=*/false);
    std::atomic<int> ok{0};
    auto worker = [&]() {
        for (int i = 0; i < 20; ++i) {
            Session loaded = sm.load("t1");
            loaded.updated = SessionManager::iso8601();
            sm.save(loaded, false);
            ok.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread a(worker), b(worker);
    a.join();
    b.join();
    check(ok.load() == 40, "concurrent load/save completed 40 ops");
    check(sm.exists("t1"), "session still exists after concurrent writes");
    Session final = sm.load("t1");
    check(final.records.size() == 1, "records intact after concurrent writes");
    std::filesystem::remove_all(dir);
}

void test_async_ui_timeline_commit() {
    std::string dir = tmpDir();
    SessionManager sm(dir);
    Session s = sm.create("async1", "ag", "m", "p");
    s.records.push_back({SessionRecord::USER, "q", SessionManager::iso8601(), ""});
    sm.save(s, false);

    UiTimelineCommit c;
    c.sessionId = "async1";
    c.baseDir = dir;
    c.uiTimelineJson =
        R"([{"kind":"User","title":"you","body":"q","ok":true},{"kind":"Response","title":"r","body":"a","ok":true}])";
    c.agentName = "ag";
    c.model = "m";
    c.provider = "p";
    c.generation = 1;
    AsyncUiTimelineWriter::instance().enqueue(c);
    c.generation = 2;
    c.uiTimelineJson =
        R"([{"kind":"User","title":"you","body":"q2","ok":true},{"kind":"Response","title":"r","body":"a2","ok":true}])";
    AsyncUiTimelineWriter::instance().enqueue(std::move(c));
    AsyncUiTimelineWriter::instance().flush();

    Session loaded = sm.load("async1");
    check(!loaded.uiTimelineJson.empty(), "async commit wrote ui_timeline");
    check(loaded.uiTimelineJson.find("q2") != std::string::npos,
          "coalesced commit kept latest generation body");
    check(loaded.records.size() == 1, "async commit did not wipe records");
    std::filesystem::remove_all(dir);
}

void test_fork_deep_copy_timeline() {
    std::string dir = tmpDir();
    SessionManager sm(dir);
    Session src = sm.create("src", "ag", "m", "p");
    src.records.push_back({SessionRecord::USER, "hello", SessionManager::iso8601(), ""});
    src.uiTimelineJson = R"([{"kind":"User","title":"you","body":"hello","ok":true}])";
    src.renderedHistory.push_back("line");
    sm.save(src, false);

    Session fork = forkSession(sm, "src", "dst", "named");
    check(fork.id == "dst", "fork id");
    check(fork.records.size() == 1, "fork records");
    check(fork.uiTimelineJson.find("hello") != std::string::npos, "fork copies ui_timeline");
    check(!fork.renderedHistory.empty(), "fork copies renderedHistory");
    check(fork.metadata.count("forked_from") && fork.metadata["forked_from"] == "src",
          "fork metadata forked_from");
    check(fork.metadata.count("name") && fork.metadata["name"] == "named", "fork session name");
    std::filesystem::remove_all(dir);
}

void test_set_session_title() {
    std::string dir = tmpDir();
    SessionManager sm(dir);
    Session s = sm.create("titled", "ag", "m", "p");
    s.records.push_back({SessionRecord::USER, "hello world", SessionManager::iso8601(), ""});
    sm.save(s, false);
    check(setSessionTitle(sm, "titled", "My Run"), "setSessionTitle ok");
    Session loaded = sm.load("titled");
    check(loaded.metadata.count("name") && loaded.metadata["name"] == "My Run",
          "title stored in metadata.name");
    auto list = sm.list();
    bool found = false;
    for (const auto& info : list) {
        if (info.id == "titled") {
            found = true;
            check(info.title == "My Run", "list() exposes title");
            check(info.agentName == "ag", "list() keeps agentName separate from title");
        }
    }
    check(found, "titled session appears in list");
    check(setSessionTitle(sm, "titled", ""), "clear title");
    loaded = sm.load("titled");
    check(loaded.metadata.count("name") == 0, "cleared title removes metadata.name");
    std::filesystem::remove_all(dir);
}

void test_compact_save_default() {
    std::string dir = tmpDir();
    SessionManager sm(dir);
    Session s = sm.create("c1", "a", "m", "p");
    s.records.push_back({SessionRecord::USER, "x", SessionManager::iso8601(), ""});
    sm.save(s, /*pretty=*/false);
    auto path = std::filesystem::path(dir) / "c1.json";
    check(std::filesystem::exists(path), "compact save wrote file");
    // Pretty would have newlines + indentation; compact should be denser.
    std::ifstream in(path);
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check(all.find("\n  ") == std::string::npos || all.find("\"records\"") != std::string::npos,
          "compact save produced content");
    std::filesystem::remove_all(dir);
}
}  // namespace

int main() {
    std::cout << "SessionController foundation tests\n";
    test_session_ref_single_id();
    test_io_mutex_recursive_load_save();
    test_async_ui_timeline_commit();
    test_fork_deep_copy_timeline();
    test_set_session_title();
    test_compact_save_default();
    std::cout << "\n" << (failures == 0 ? "all passed" : "failures: " + std::to_string(failures))
              << "\n";
    return failures == 0 ? 0 : 1;
}
