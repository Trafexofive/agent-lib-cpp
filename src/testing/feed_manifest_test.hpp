// =============================================================================
// Feed manifest loader test — verifies multi-language feed loading
// Test: feeds/ directory with YAML + scripts, verify they're loaded and polled
// =============================================================================
#pragma once
#include <json/json.h>
#include <unistd.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "../core/dispatch.hpp"
#include "../feeds/feed_engine.hpp"

namespace cortex::mk3::tests {
namespace fs = std::filesystem;

// ── Test harness ──
struct FeedManifestTest {
    int passed = 0, failed = 0;

    void check(bool cond, const std::string& name) {
        if (cond) {
            passed++;
            std::cout << "  PASS: " << name << "\n";
        } else {
            failed++;
            std::cout << "  FAIL: " << name << "\n";
        }
    }

    bool run() {
        std::cout << "=== Feed Manifest Tests ===\n";

        setup();
        testLoadBuiltin();
        testLoadPythonFeed();
        testFeedOutputFormat();
        testFeedInjectionIntoPrompt();
        testFeedTools();
        testUnknownDottedFeedToolDispatch();
        testFeedManifestTools();
        testFeedManifestToolInvocation();
        testFeedRuntimePerCallEnv();
        testFeedToolPerCallEnv();
        cleanup();

        std::cout << "\n  " << passed << "/" << (passed + failed) << " passed\n";
        return failed == 0;
    }

   private:
    fs::path testDir;

    void setup() {
        testDir = fs::temp_directory_path() / ("cortex-feed-test-" + std::to_string(getpid()));
        fs::create_directories(testDir);
    }

    void cleanup() {
        fs::remove_all(testDir);
    }

    // ── Test: built-in feeds load ──
    void testLoadBuiltin() {
        // Built-in feeds are registered via registerFeeds()
        // They should produce valid output on poll
        auto results = feeds::FeedEngine::instance().pollAll();
        check(!results.empty(), "built-in feeds produce results");
        bool hasClock = false, hasStats = false, hasCwd = false;
        for (auto& r : results) {
            if (r.name == "system_clock")
                hasClock = true;
            if (r.name == "system_stats")
                hasStats = true;
            if (r.name == "working_directory")
                hasCwd = true;
        }
        check(hasClock, "system_clock feed exists");
        check(hasStats, "system_stats feed exists");
        check(hasCwd, "working_directory feed exists");
    }

    // ── Test: Feed tools — register, expose, dispatch via engine ──
    void testFeedTools() {
        auto& engine = feeds::FeedEngine::instance();

        // Built-in feeds expose real refresh tools only; no fake/no-op demo tools.
        check(engine.feedHasTool("working_directory", "refresh"),
              "working_directory.refresh tool exists");
        check(!engine.feedHasTool("working_directory", "touch"),
              "working_directory.touch demo tool is not exposed");
        check(engine.feedHasTool("system_clock", "refresh"),
              "system_clock.refresh tool exists");
        check(!engine.feedHasTool("system_clock", "missing"),
              "unknown tool reported as missing");

        // Tool descriptors should be exposed for prompt injection.
        auto specs = engine.feedToolSpecs();
        check(specs.count("working_directory") > 0, "working_directory tool specs exposed");
        check(specs.count("system_clock") > 0, "system_clock tool specs exposed");

        // refresh tool returns success + data
        auto refresh = engine.callFeedTool("system_clock", "refresh", Json::Value());
        check(refresh.get("success", false).asBool(), "refresh tool returns success");
        check(refresh.isMember("data"), "refresh tool returns data");

        // Unknown feed/tool returns clean error.
        auto err = engine.callFeedTool("nope", "missing", Json::Value());
        check(!err.get("success", true).asBool(), "unknown feed returns success=false");
        check(err.isMember("error"), "unknown feed returns error message");
    }

    // ── Test: Dotted feed tool syntax hard-errors for unknown tools ──
    void testUnknownDottedFeedToolDispatch() {
        protocol::ParsedAction action;
        action.type = protocol::ActionType::FEED;
        action.name = "working_directory.missing_tool";
        action.params = Json::Value(Json::objectValue);

        Json::Value result = dispatch::dispatchFeed(action);
        check(!result.get("success", true).asBool(),
              "unknown dotted feed tool returns success=false");
        check(result.get("feed", "").asString() == "working_directory",
              "unknown dotted feed tool reports feed name");
        check(result.get("tool", "").asString() == "missing_tool",
              "unknown dotted feed tool reports tool name");
        check(result.get("error", "").asString().find("unknown feed tool") != std::string::npos,
              "unknown dotted feed tool reports tool error");
    }

    // ── Test: Python feed loads from manifest ──
    void testLoadPythonFeed() {
        // Create feed manifest
        fs::path feedDir = testDir / "feeds" / "test_clock";
        fs::create_directories(feedDir);

        // feed.yml
        {
            std::ofstream f(feedDir / "feed.yml");
            f << "kind: Feed\n";
            f << "name: test_clock\n";
            f << "version: \"1.0\"\n";
            f << "summary: \"Python-based clock feed\"\n";
            f << "runtime: python3\n";
            f << "entrypoint: ./feed.py\n";
            f << "interval_secs: 60\n";
            f << "output_schema:\n";
            f << "  type: object\n";
            f << "  properties:\n";
            f << "    time:\n";
            f << "      type: string\n";
            f << "    epoch:\n";
            f << "      type: integer\n";
        }

        // feed.py
        {
            std::ofstream f(feedDir / "feed.py");
            f << "#!/usr/bin/env python3\n";
            f << "import json, time, os\n";
            f << "print(json.dumps({\n";
            f << "    \"time\": time.ctime(),\n";
            f << "    \"epoch\": int(time.time()),\n";
            f << "    \"pid\": os.getpid()\n";
            f << "}))\n";
        }
        fs::permissions(feedDir / "feed.py",
                        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add);

        // Load and verify
        auto result =
            feeds::FeedEngine::instance().loadFeedManifest((feedDir / "feed.yml").string());

        check(result.success, "python feed manifest loads");

        // Poll to verify the feed produces output
        auto all = feeds::FeedEngine::instance().pollAll();
        bool found = false;
        for (auto& r : all) {
            if (r.name == "test_clock" && r.ok) {
                found = true;
                check(r.summary.find("epoch") != std::string::npos,
                      "python feed has epoch in poll output");
                check(!r.summary.empty(), "python feed produces summary on poll");
                break;
            }
        }
        check(found, "python feed appears in poll results");
    }

    // ── Test: feed output format ──
    void testFeedOutputFormat() {
        auto results = feeds::FeedEngine::instance().pollAll();

        for (auto& r : results) {
            // Each result should have a name and non-empty summary
            check(!r.name.empty(), "feed " + r.name + " has name");
            check(!r.summary.empty(), "feed " + r.name + " has summary");

            // Summary should be parseable as a single-line key-value format
            check(r.summary.find('\n') != 0 || r.summary.size() < 500,
                  "feed " + r.name + " summary is reasonable size");
        }
    }

    // ── Test: feed.yml tools: block parses into engine-side spec store ──
    void testFeedManifestTools() {
        // Create a feed manifest with a tools: block.
        fs::path feedDir = testDir / "feeds" / "manifest_tools";
        fs::create_directories(feedDir);

        {
            std::ofstream f(feedDir / "feed.yml");
            f << "kind: Feed\n";
            f << "name: manifest_tools_feed\n";
            f << "runtime: builtin\n";
            f << "tools:\n";
            f << "  - name: refresh\n";
            f << "    description: Force a fresh poll\n";
            f << "  - name: reset\n";
            f << "    description: Reset feed state\n";
            f << "    runtime: process\n";
            f << "    entrypoint: ./reset.sh\n";
        }

        auto result = feeds::FeedEngine::instance().loadFeedManifest((feedDir / "feed.yml").string());
        check(result.success, "feed with tools: block loads");

        auto tools = feeds::FeedEngine::instance().feedManifestTools("manifest_tools_feed");
        check(tools.size() == 2, "feed with tools: block stores 2 tools");

        bool sawRefresh = false, sawReset = false;
        for (const auto& t : tools) {
            if (t.name == "refresh") {
                sawRefresh = true;
                check(t.description == "Force a fresh poll",
                      "refresh tool description parsed");
                check(t.runtime == "builtin",
                      "refresh tool inherits feed runtime");
            } else if (t.name == "reset") {
                sawReset = true;
                check(t.description == "Reset feed state",
                      "reset tool description parsed");
                check(t.runtime == "process",
                      "reset tool overrides runtime");
                check(t.entrypoint == "./reset.sh",
                      "reset tool entrypoint parsed");
            }
        }
        check(sawRefresh, "refresh tool in manifest feed tools");
        check(sawReset, "reset tool in manifest feed tools");

        // Unknown feed returns empty tool list (no false positives).
        auto empty = feeds::FeedEngine::instance().feedManifestTools("does_not_exist");
        check(empty.empty(), "unknown feed has no manifest tools");

        // Tools without a `name` field are silently dropped (can't address
        // them, so storing them would just be noise).
        fs::path namelessDir = testDir / "feeds" / "nameless_tools";
        fs::create_directories(namelessDir);
        {
            std::ofstream f(namelessDir / "feed.yml");
            f << "kind: Feed\n";
            f << "name: nameless_tools_feed\n";
            f << "runtime: builtin\n";
            f << "tools:\n";
            f << "  - description: nameless tool entry\n";
            f << "  - name: keep\n";
            f << "    description: named tool entry\n";
        }
        feeds::FeedEngine::instance().loadFeedManifest((namelessDir / "feed.yml").string());
        auto nameless =
            feeds::FeedEngine::instance().feedManifestTools("nameless_tools_feed");
        check(nameless.size() == 1, "feed with nameless tool entry keeps only named tool");
        if (!nameless.empty())
            check(nameless[0].name == "keep",
                  "only the named tool survives manifest-tools parse");
    }

    // ── Test: manifest-declared tool is wired to a real invocation handler ──
    void testFeedManifestToolInvocation() {
        // Standalone feed manifest with a tool whose entrypoint is a shell
        // script that echoes a JSON document. No C++-registered handlers
        // exist for this feed, so the manifest handler should be installed
        // and callFeedTool should invoke the entrypoint.
        fs::path feedDir = testDir / "feeds" / "manifest_tools_wired";
        fs::create_directories(feedDir);

        // tool.py — reads FEED_TOOL_PARAMS, echoes back a success JSON with
        // a derived field. Exits 0. Python is a more reliable JSON producer
        // than bash heredocs with embedded JSON values.
        {
            std::ofstream f(feedDir / "tool.py");
            f << "#!/usr/bin/env python3\n";
            f << "import json, os\n";
            f << "params = os.environ.get('FEED_TOOL_PARAMS', '{}')\n";
            f << "try:\n";
            f << "    parsed = json.loads(params) if params else {}\n";
            f << "except Exception:\n";
            f << "    parsed = {'_raw': params}\n";
            f << "out = {'success': True, 'echo': parsed, 'ran': 'tool'}\n";
            f << "print(json.dumps(out))\n";
        }
        fs::permissions(feedDir / "tool.py",
                        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add);

        {
            std::ofstream f(feedDir / "feed.yml");
            f << "kind: Feed\n";
            f << "name: wired_feed\n";
            f << "runtime: builtin\n";  // builtin = no actual feed execution
            f << "tools:\n";
            f << "  - name: run_tool\n";
            f << "    description: Invokes ./tool.py with params\n";
            f << "    runtime: python3\n";
            f << "    entrypoint: ./tool.py\n";
        }

        auto loadResult =
            feeds::FeedEngine::instance().loadFeedManifest((feedDir / "feed.yml").string());
        check(loadResult.success, "wired feed manifest loads");

        auto& engine = feeds::FeedEngine::instance();
        check(engine.feedHasTool("wired_feed", "run_tool"),
              "wired manifest tool is registered with engine");

        Json::Value params(Json::objectValue);
        params["name"] = "mlam";
        auto result = engine.callFeedTool("wired_feed", "run_tool", params);
        check(result.get("success", false).asBool(),
              "wired manifest tool returns success");
        check(result.get("ran", "").asString() == "tool",
              "wired manifest tool ran the entrypoint");
        check(result.isMember("echo"),
              "wired manifest tool received and returned the params");

        // Spec should now also be exposed via feedToolSpecs() so the prompt
        // can advertise it.
        auto specs = engine.feedToolSpecs();
        bool sawWiredSpec = false;
        if (auto it = specs.find("wired_feed"); it != specs.end()) {
            for (const auto& s : it->second) {
                if (s.name == "run_tool") {
                    sawWiredSpec = true;
                    check(s.description == "Invokes ./tool.py with params",
                          "wired manifest tool spec carries the manifest description");
                }
            }
        }
        check(sawWiredSpec, "wired manifest tool spec appears in feedToolSpecs()");

        // Manifest tool handler should not collide with C++-registered ones.
        // system_clock.refresh is C++-registered; the feed has no manifest
        // tools, so the C++ handler must still be the one called.
        check(engine.feedHasTool("system_clock", "refresh"),
              "C++-registered refresh tool still present on system_clock");
        auto clk = engine.callFeedTool("system_clock", "refresh", Json::Value());
        check(clk.get("success", false).asBool(),
              "C++-registered refresh tool still succeeds");
        check(!clk.isMember("ran"),
              "C++-registered refresh tool did not run a manifest handler");
    }

    // ── Test: feed poll runs on the hardened process::run substrate ─────
    // The key correctness benefit over the old popen path is per-call env:
    // the child sees CALL_TOOL set, but the parent process env is not
    // mutated. This test would have failed under the old setenv-based
    // implementation if a prior call left the env in a weird state.
    void testFeedRuntimePerCallEnv() {
        fs::path feedDir = testDir / "feeds" / "per_call_env";
        fs::create_directories(feedDir);

        {
            std::ofstream f(feedDir / "check.sh");
            f << "#!/usr/bin/env bash\n";
            f << "if [ -z \"${CALL_TOOL:-}\" ]; then\n";
            f << "  echo 'no_call_tool'\n";
            f << "else\n";
            f << "  echo 'has_call_tool'\n";
            f << "fi\n";
        }
        fs::permissions(feedDir / "check.sh",
                        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add);

        {
            std::ofstream f(feedDir / "feed.yml");
            f << "kind: Feed\n";
            f << "name: per_call_env_feed\n";
            f << "runtime: bash\n";
            f << "entrypoint: ./check.sh\n";
        }

        unsetenv("CALL_TOOL");

        auto& engine = feeds::FeedEngine::instance();
        auto r = engine.loadFeedManifest((feedDir / "feed.yml").string());
        check(r.success, "per-call env feed manifest loads");
        check(r.summary == "has_call_tool",
              "feed poll child sees CALL_TOOL set in its env");

        // Parent process env must not be polluted.
        check(getenv("CALL_TOOL") == nullptr,
              "CALL_TOOL is not leaked to parent process after feed poll");

        // Subsequent poll must still work and not be affected by any state
        // the first call might have left behind.
        auto results = engine.pollAll();
        bool found = false;
        for (const auto& fr : results) {
            if (fr.name == "per_call_env_feed") {
                found = true;
                check(fr.summary == "has_call_tool",
                      "per-call env feed re-poll still has CALL_TOOL set");
                break;
            }
        }
        check(found, "per-call env feed present in pollAll");
        check(getenv("CALL_TOOL") == nullptr,
              "CALL_TOOL still not leaked after subsequent poll");

        unsetenv("CALL_TOOL");
    }

    // ── Test: feed tool handler uses per-call env (no global setenv leak) ──
    void testFeedToolPerCallEnv() {
        fs::path feedDir = testDir / "feeds" / "tool_per_call_env";
        fs::create_directories(feedDir);

        // tool.py: reads FEED_TOOL_PARAMS, echoes it as JSON. Exits 0.
        {
            std::ofstream f(feedDir / "tool.py");
            f << "#!/usr/bin/env python3\n";
            f << "import json, os\n";
            f << "params = os.environ.get('FEED_TOOL_PARAMS', 'unset')\n";
            f << "print(json.dumps({'success': True, 'params_seen': params}))\n";
        }
        fs::permissions(feedDir / "tool.py",
                        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add);

        {
            std::ofstream f(feedDir / "feed.yml");
            f << "kind: Feed\n";
            f << "name: tool_per_call_env_feed\n";
            f << "runtime: builtin\n";
            f << "tools:\n";
            f << "  - name: see_params\n";
            f << "    description: Reads FEED_TOOL_PARAMS\n";
            f << "    runtime: python3\n";
            f << "    entrypoint: ./tool.py\n";
        }

        // Parent env must start clean.
        unsetenv("FEED_TOOL_PARAMS");

        auto& engine = feeds::FeedEngine::instance();
        auto r = engine.loadFeedManifest((feedDir / "feed.yml").string());
        check(r.success, "tool per-call env feed manifest loads");

        // Call the tool with a known param value.
        Json::Value params(Json::objectValue);
        params["token"] = "first-call";
        auto result = engine.callFeedTool("tool_per_call_env_feed", "see_params", params);
        check(result.get("success", false).asBool(),
              "tool per-call env tool returns success");
        check(result.get("params_seen", "").asString().find("first-call") != std::string::npos,
              "tool per-call env tool saw FEED_TOOL_PARAMS set to first-call");

        // Parent env must not be polluted.
        check(getenv("FEED_TOOL_PARAMS") == nullptr,
              "FEED_TOOL_PARAMS is not leaked to parent process after tool call");

        // Second call with a different value: must not see leakage from the
        // first call (would have been a real bug under the old setenv path).
        Json::Value params2(Json::objectValue);
        params2["token"] = "second-call";
        auto result2 = engine.callFeedTool("tool_per_call_env_feed", "see_params", params2);
        check(result2.get("success", false).asBool(),
              "tool per-call env second call returns success");
        check(result2.get("params_seen", "").asString().find("second-call") != std::string::npos,
              "second tool call sees fresh FEED_TOOL_PARAMS, not stale first-call");
        check(result2.get("params_seen", "").asString().find("first-call") == std::string::npos,
              "second tool call did not see stale first-call FEED_TOOL_PARAMS");

        unsetenv("FEED_TOOL_PARAMS");
    }

    // ── Test: feed injection into prompt produces XML ──
    void testFeedInjectionIntoPrompt() {
        // Simulate what buildSystemPrompt does
        auto results = feeds::FeedEngine::instance().pollAll();
        std::ostringstream ss;
        if (!results.empty()) {
            ss << "<feeds>\n";
            for (auto& r : results) {
                ss << "  <" << r.name << ">\n";
                ss << r.summary << "\n";
                ss << "  </" << r.name << ">\n";
            }
            ss << "</feeds>\n";
        }
        std::string xml = ss.str();
        check(xml.find("<feeds>") != std::string::npos, "feed XML has opening tag");
        check(xml.find("</feeds>") != std::string::npos, "feed XML has closing tag");
        check(xml.find("<system_clock>") != std::string::npos, "system_clock tag present");
        check(xml.find("</system_clock>") != std::string::npos, "system_clock tag closed");
    }
};

}  // namespace cortex::mk3::tests
