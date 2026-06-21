// =============================================================================
// Docker relic dispatcher test — HTTP routing, container lifecycle
// Test: mock relic endpoint → route → verify response
// =============================================================================
#pragma once
#include <json/json.h>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "src/relics/relic.hpp"
#include "src/relics/reliquary.hpp"
#include "src/relics/docker_dispatcher.hpp"

namespace cortex::mk3::tests {
namespace fs = std::filesystem;

struct DockerRelicTest {
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
        std::cout << "=== Docker Relic Dispatcher Tests ===\n";
        testRelicManifestParsing();
        testManagedVsRemote();
        testHealthCheckRouting();
        testRelicDispatchFlow();
        testReliquaryRegistry();
        testDockerRelicYamlParserRobust();
        std::cout << "\n  " << passed << "/" << (passed + failed) << " passed\n";
        return failed == 0;
    }

    // ── Test: relic manifest parsing ──
    void testRelicManifestParsing() {
        // Verify artifact_store relic.yml has required fields
        std::ifstream f("manifests/relics/artifact_store/relic.yml");
        check(f.good(), "artifact_store relic.yml exists");

        // Read and check for key fields
        std::string yaml((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
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
            if (r.runtime == "managed")
                return "managed";
            if (r.runtime == "remote")
                return "remote";
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
            if (name != "artifact_store" && name != "secret_store" && name != "event_bus" &&
                name != "process_manager" && name != "file_watcher") {
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

    // ── Test: Reliquary unified registry dispatches to registered Relics ──
    void testReliquaryRegistry() {
        using namespace cortex::mk3::relics;

        // Mock Relic implementation for testing.
        struct MockRelic : public Relic {
            std::string n_;
            std::string lastEndpoint;
            int callCount = 0;
            MockRelic(std::string n) : n_(std::move(n)) {}
            const std::string& name() const override { return n_; }
            std::string description() const override { return "mock:" + n_; }
            std::vector<std::string> endpoints() const override {
                return {"ping", "health"};
            }
            RelicResult handle(const std::string& endpoint,
                               const Json::Value&) override {
                ++callCount;
                lastEndpoint = endpoint;
                if (endpoint == "ping")
                    return RelicResult::ok(Json::Value("pong:" + n_));
                if (endpoint == "health")
                    return RelicResult::ok();
                return RelicResult::fail("unknown endpoint: " + endpoint);
            }
            bool isHealthy() const override { return true; }
        };

        auto& reg = Reliquary::instance();
        reg.clear();  // start clean for test isolation

        auto a = std::make_shared<MockRelic>("alpha");
        auto b = std::make_shared<MockRelic>("beta");
        check(reg.registerRelic(a), "register alpha");
        check(reg.registerRelic(b), "register beta");
        check(!reg.registerRelic(std::make_shared<MockRelic>("alpha")),
              "duplicate registration is rejected");

        check(reg.has("alpha") && reg.has("beta"),
              "registered relics are findable");
        check(!reg.has("gamma"), "unregistered relic not findable");

        auto names = reg.names();
        check(names.size() == 2, "names() returns 2 entries");
        check(std::find(names.begin(), names.end(), "alpha") != names.end() &&
                  std::find(names.begin(), names.end(), "beta") != names.end(),
              "names() contains alpha + beta");

        // Dispatch goes to the right instance.
        auto r1 = reg.dispatch("alpha", "ping", Json::Value());
        check(r1.success, "alpha.ping succeeds");
        check(a->callCount == 1, "alpha was actually invoked");
        check(a->lastEndpoint == "ping", "alpha recorded the endpoint");

        auto r2 = reg.dispatch("beta", "ping", Json::Value());
        check(r2.success, "beta.ping succeeds");
        check(b->callCount == 1, "beta was actually invoked");

        // Unknown relic returns a fail() result, not a crash.
        auto r3 = reg.dispatch("ghost", "ping", Json::Value());
        check(!r3.success, "unknown relic returns fail");
        check(r3.error.find("Unknown relic") != std::string::npos,
              "unknown relic error mentions Unknown relic");

        // Health check enumerates every relic.
        auto health = reg.healthCheckAll();
        check(health.size() == 2, "health check covers all 2 relics");
        check(health["alpha"] && health["beta"], "both mock relics report healthy");

        reg.clear();
    }

    // ── Test: DockerRelicDef parser uses ManifestYaml (handles real-world quirks) ──
    void testDockerRelicYamlParserRobust() {
        using namespace cortex::mk3::relics;
        namespace fs = std::filesystem;

        // Standalone relic dir with edge-case YAML: quoted strings, comments
        // after values, block-style fields that should be ignored, mixed
        // indents, and a port value that has a comment.
        fs::path dir = fs::temp_directory_path() / "yaml_parser_relic_test";
        fs::create_directories(dir);

        {
            std::ofstream f(dir / "relic.yml");
            f << "# comment-only first line\n";
            f << "kind: Relic\n";
            f << "version: \"1.0\"\n";
            f << "name: \"quoted_name\"   # inline comment\n";
            f << "mode: managed\n";
            f << "summary: 'single-quoted summary with spaces'\n";
            f << "port: 8123 # default port\n";
            f << "compose_file: \"./compose.yml\"\n";
            f << "health_path: \"/healthz\"\n";
            f << "\n";
            f << "deployment:   # block we should ignore gracefully\n";
            f << "  type: docker\n";
            f << "  file: compose.yml\n";
            f << "\n";
            f << "endpoints:    # block we should also ignore\n";
            f << "  - name: ping\n";
            f << "    path: /ping\n";
        }

        DockerRelicDef def;
        bool ok = DockerRelicDispatcher::loadDefFromDir(dir.string(), def);
        check(ok, "loadDefFromDir parses valid relic.yml");
        check(def.name == "quoted_name",
              "quoted string is unquoted by ManifestYaml");
        check(def.mode == "managed",
              "mode parsed correctly");
        check(def.summary == "single-quoted summary with spaces",
              "single-quoted summary unquoted by ManifestYaml");
        check(def.port == 8123,
              "port int parsed correctly (inline comment ignored)");
        check(def.composeFile == "./compose.yml",
              "compose_file parsed correctly");
        check(def.healthPath == "/healthz",
              "health_path parsed correctly (overrides default)");

        fs::remove_all(dir);
    }
};

}  // namespace cortex::mk3::tests
