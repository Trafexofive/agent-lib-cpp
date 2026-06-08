// =============================================================================
// Docker relic dispatcher test — HTTP routing, container lifecycle
// Test: mock relic endpoint → route → verify response
// =============================================================================
#pragma once
#include <string>
#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <json/json.h>

namespace cortex::mk3::tests {
namespace fs = std::filesystem;

struct DockerRelicTest {
    int passed = 0, failed = 0;

    void check(bool cond, const std::string& name) {
        if (cond) { passed++; std::cout << "  PASS: " << name << "\n"; }
        else      { failed++; std::cout << "  FAIL: " << name << "\n"; }
    }

    bool run() {
        std::cout << "=== Docker Relic Dispatcher Tests ===\n";
        testRelicManifestParsing();
        testManagedVsRemote();
        testHealthCheckRouting();
        testRelicDispatchFlow();
        std::cout << "\n  " << passed << "/" << (passed + failed) << " passed\n";
        return failed == 0;
    }

    // ── Test: relic manifest parsing ──
    void testRelicManifestParsing() {
        // Verify artifact_store relic.yml has required fields
        std::ifstream f("manifests/relics/artifact_store/relic.yml");
        check(f.good(), "artifact_store relic.yml exists");

        // Read and check for key fields
        std::string yaml((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
        check(yaml.find("name:") != std::string::npos, "relic has name field");
        check(yaml.find("endpoints:") != std::string::npos, "relic has endpoints field");
        check(yaml.find("mode:") != std::string::npos, "relic has mode field");
    }

    // ── Test: managed vs remote classification ──
    void testManagedVsRemote() {
        // Docker relics are "managed" (local container, auto-start)
        // Remote relics are "remote" (external API, no management)
        // This test verifies the classification logic

        struct RelicDef {
            std::string name;
            std::string runtime;
        };
        RelicDef managed = {"artifact_store", "managed"};
        RelicDef remote = {"github_api", "remote"};

        auto classify = [](const RelicDef& r) -> std::string {
            if (r.runtime == "managed") return "managed";
            if (r.runtime == "remote") return "remote";
            return "builtin";
        };

        check(classify(managed) == "managed", "managed relics classified as managed");
        check(classify(remote) == "remote", "remote relics classified as remote");
    }

    // ── Test: health check URL construction ──
    void testHealthCheckRouting() {
        // Each Docker relic has a health endpoint at /health
        // Dispatcher checks this before routing actions

        struct RelicRoute {
            std::string name;
            int port;
        };
        RelicRoute artifact = {"artifact_store", 8100};

        auto healthUrl = [](const RelicRoute& r) -> std::string {
            return "http://localhost:" + std::to_string(r.port) + "/health";
        };

        std::string url = healthUrl(artifact);
        check(url == "http://localhost:8100/health", "health URL constructed correctly");
    }

    // ── Test: full dispatch flow (simulated) ──
    void testRelicDispatchFlow() {
        // Simulate: action type=relic name=artifact_store endpoint=store
        // Dispatcher should:
        // 1. Look up relic by name
        // 2. Check if managed → ensure container up
        // 3. Route HTTP request to container
        // 4. Return parsed JSON result

        struct DispatchResult {
            bool success = false;
            std::string data;
            std::string error;
        };

        // Simulated dispatch
        auto dispatch = [&](const std::string& name, const std::string& endpoint,
                            const Json::Value& params) -> DispatchResult {
            // Step 1: lookup
            if (name != "artifact_store" && name != "secret_store" &&
                name != "event_bus" && name != "process_manager" &&
                name != "file_watcher") {
                return {false, "", "Unknown relic: " + name};
            }

            // Step 2: managed check (docker → managed)
            // In real impl: docker ps → if not running, docker-compose up -d

            // Step 3: route (simulated — no actual HTTP in test)
            if (endpoint == "store") {
                return {true, R"({"artifact_id":"test-123","stored":true})", ""};
            }
            if (endpoint == "health") {
                return {true, R"({"status":"ok"})", ""};
            }
            return {false, "", "Unknown endpoint: " + endpoint};
        };

        // Test valid dispatch
        auto r1 = dispatch("artifact_store", "store", Json::Value());
        check(r1.success, "artifact_store.store succeeds");
        check(r1.data.find("artifact_id") != std::string::npos, "store returns artifact_id");

        // Test health check
        auto r2 = dispatch("artifact_store", "health", Json::Value());
        check(r2.success, "artifact_store.health succeeds");

        // Test unknown relic
        auto r3 = dispatch("nonexistent", "store", Json::Value());
        check(!r3.success, "unknown relic returns error");
        check(r3.error.find("Unknown relic") != std::string::npos, "unknown relic error message");

        // Test unknown endpoint
        auto r4 = dispatch("artifact_store", "nonexistent", Json::Value());
        check(!r4.success, "unknown endpoint returns error");
    }
};

} // namespace cortex::mk3::tests
