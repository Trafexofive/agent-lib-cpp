// =============================================================================
// agent-lib-MK3 — Manifest import-path classifier tests
// Covers ML01: `builtin/exec` and similar bare prefixed names must NOT route
// into the path branch.
// =============================================================================

#include "src/core/manifest_loader.hpp"
#include <iostream>
#include <string>

using cortex::mk3::ManifestLoader;

static int passed = 0, failed = 0;

#define TEST(name)  do { std::cout << "  " << name << "... "; } while (0)
#define PASS()      do { passed++; std::cout << "PASS\n"; } while (0)
#define FAIL(msg)   do { failed++; std::cout << "FAIL: " << msg << "\n"; return; } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); } } while (0)

void test_isPath_bare_name() {
    TEST("bare name (`exec`) classified as built-in");
    CHECK(!ManifestLoader::isPathImport("exec"), "exec should NOT be a path");
    CHECK(!ManifestLoader::isPathImport("fs_read"), "fs_read should NOT be a path");
    CHECK(!ManifestLoader::isPathImport("context_pin"), "context_pin should NOT be a path");
    PASS();
}

void test_isPath_builtin_prefix() {
    TEST("ML01: `builtin/exec` is NOT a path");
    CHECK(!ManifestLoader::isPathImport("builtin/exec"), "builtin/exec must be built-in");
    CHECK(!ManifestLoader::isPathImport("builtin/fs_read"), "builtin/fs_read must be built-in");
    PASS();
}

void test_isPath_relative() {
    TEST("relative paths (`./` and `../`) ARE paths");
    CHECK(ManifestLoader::isPathImport("./tools/scaffold/tool.yml"), "./ relative is a path");
    CHECK(ManifestLoader::isPathImport("../shared/tool.yml"), "../ relative is a path");
    PASS();
}

void test_isPath_absolute() {
    TEST("absolute paths (`/...`) ARE paths");
    CHECK(ManifestLoader::isPathImport("/abs/path/tool.yml"), "/abs is a path");
    PASS();
}

void test_isPath_yml_suffix() {
    TEST("anything ending in .yml is a path");
    CHECK(ManifestLoader::isPathImport("my-tool.yml"), "bare *.yml should be path");
    CHECK(ManifestLoader::isPathImport("nested/path/x.yml"), "nested *.yml is path");
    PASS();
}

void test_stripPrefix() {
    TEST("stripBuiltinPrefix removes only `builtin/`");
    CHECK(ManifestLoader::stripBuiltinPrefix("builtin/exec") == "exec", "strip builtin/");
    CHECK(ManifestLoader::stripBuiltinPrefix("exec") == "exec", "no prefix → unchanged");
    CHECK(ManifestLoader::stripBuiltinPrefix("builtin/") == "builtin/", "size <= prefix → unchanged");
    CHECK(ManifestLoader::stripBuiltinPrefix("builtin/fs_read") == "fs_read", "strip fs_read");
    PASS();
}

int main() {
    std::cout.setf(std::ios::unitbuf);
    std::cout << "\n╔══════════════════════════════════════════╗\n";
    std::cout << "║   MK3 Manifest Classifier Tests          ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n\n";

    test_isPath_bare_name();
    test_isPath_builtin_prefix();
    test_isPath_relative();
    test_isPath_absolute();
    test_isPath_yml_suffix();
    test_stripPrefix();

    std::cout << "\n──────────────────────────────────────────\n";
    std::cout << "  " << passed << " passed, " << failed << " failed\n";
    std::cout << "──────────────────────────────────────────\n";
    return failed > 0 ? 1 : 0;
}
