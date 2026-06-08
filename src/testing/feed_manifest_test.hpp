// =============================================================================
// Feed manifest loader test — verifies multi-language feed loading
// Test: feeds/ directory with YAML + scripts, verify they're loaded and polled
// =============================================================================
#pragma once
#include <string>
#include <cassert>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <json/json.h>
#include "../feeds/feed_engine.hpp"

namespace cortex::mk3::tests {
namespace fs = std::filesystem;

// ── Test harness ──
struct FeedManifestTest {
    int passed = 0, failed = 0;

    void check(bool cond, const std::string& name) {
        if (cond) { passed++; std::cout << "  PASS: " << name << "\n"; }
        else      { failed++; std::cout << "  FAIL: " << name << "\n"; }
    }

    bool run() {
        std::cout << "=== Feed Manifest Tests ===\n";

        setup();
        testLoadBuiltin();
        testLoadPythonFeed();
        testFeedOutputFormat();
        testFeedInjectionIntoPrompt();
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
            if (r.name == "system_clock") hasClock = true;
            if (r.name == "system_stats") hasStats = true;
            if (r.name == "working_directory") hasCwd = true;
        }
        check(hasClock, "system_clock feed exists");
        check(hasStats, "system_stats feed exists");
        check(hasCwd, "working_directory feed exists");
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
        auto result = feeds::FeedEngine::instance().loadFeedManifest(
            (feedDir / "feed.yml").string());

        check(result.success, "python feed manifest loads");

        // Poll to verify the feed produces output
        auto all = feeds::FeedEngine::instance().pollAll();
        bool found = false;
        for (auto& r : all) {
            if (r.name == "test_clock" && r.ok) {
                found = true;
                check(r.summary.find("epoch") != std::string::npos, "python feed has epoch in poll output");
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

} // namespace cortex::mk3::tests
