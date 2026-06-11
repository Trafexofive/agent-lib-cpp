// =============================================================================
// agent-lib-MK3 — mini_yaml unit tests
// Captures the parser bugs the audit named Y01-Y05.
// =============================================================================

#include "src/core/mini_yaml.hpp"
#include <iostream>
#include <string>

using cortex::mk3::ManifestYaml;

static int passed = 0, failed = 0;

#define TEST(name) do { std::cout << "  " << name << "... "; } while(0)
#define PASS() do { passed++; std::cout << "PASS\n"; } while(0)
#define FAIL(msg) do { failed++; std::cout << "FAIL: " << msg << "\n"; return; } while(0)
#define EXPECT_EQ(a, b, msg) do { if (!((a) == (b))) { std::cout << "    got=" << (a) << "\n    want=" << (b) << "\n"; FAIL(msg); } } while(0)
#define EXPECT_NE_STR(a, b, msg) do { if ((a) == (b)) { std::cout << "    got=" << (a) << " (unexpected)\n"; FAIL(msg); } } while(0)

// ─────────────────────────────────────────────────────────────────────────────
// Y01 — inline flow maps: `primary: { provider: x, model: y }`
//   The cognitive_engine.primary block uses this in nearly every modern manifest.
//   Current behavior: value stored as raw "{ provider: x, model: y }", no children.
// ─────────────────────────────────────────────────────────────────────────────
void test_Y01_inline_flow_map() {
    TEST("Y01 inline flow map { k: v, k2: v2 }");
    std::string yaml =
        "cognitive_engine:\n"
        "  primary: { provider: deepseek, model: deepseek-v4-pro }\n";

    auto root = ManifestYaml::parse(yaml);
    auto* ce = ManifestYaml::find(root, "cognitive_engine");
    if (!ce) FAIL("cognitive_engine not found");
    auto* primary = ManifestYaml::find(*ce, "primary");
    if (!primary) FAIL("primary not found");

    std::string provider = ManifestYaml::get(*primary, "provider");
    std::string model = ManifestYaml::get(*primary, "model");

    EXPECT_EQ(provider, std::string("deepseek"), "provider should resolve from inline map");
    EXPECT_EQ(model, std::string("deepseek-v4-pro"), "model should resolve from inline map");
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Y02a — block scalar fold: `description: >`
//   Indented lines should be joined with spaces into description's value.
// ─────────────────────────────────────────────────────────────────────────────
void test_Y02a_block_scalar_fold() {
    TEST("Y02a block scalar >");
    std::string yaml =
        "description: >\n"
        "  this is a\n"
        "  folded paragraph\n"
        "name: x\n";

    auto root = ManifestYaml::parse(yaml);
    std::string desc = ManifestYaml::get(root, "description");
    std::string name = ManifestYaml::get(root, "name");

    EXPECT_NE_STR(desc, std::string(">"), "description must not be literal '>'");
    if (desc.find("folded paragraph") == std::string::npos)
        FAIL("description should contain 'folded paragraph'");
    EXPECT_EQ(name, std::string("x"), "sibling key 'name' must still parse");
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Y02b — block scalar literal: `text: |`
//   Indented lines preserved with newlines.
// ─────────────────────────────────────────────────────────────────────────────
void test_Y02b_block_scalar_literal() {
    TEST("Y02b block scalar |");
    std::string yaml =
        "text: |\n"
        "  line one\n"
        "  line two\n"
        "next: y\n";

    auto root = ManifestYaml::parse(yaml);
    std::string text = ManifestYaml::get(root, "text");
    std::string next = ManifestYaml::get(root, "next");

    EXPECT_NE_STR(text, std::string("|"), "text must not be literal '|'");
    if (text.find("line one") == std::string::npos)
        FAIL("text should contain 'line one'");
    EXPECT_EQ(next, std::string("y"), "sibling key 'next' must still parse");
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Y03 — get() with empty value should fall back to default
//   Current bug: returns "" because of `c.value.empty() ? "" : c.value`.
// ─────────────────────────────────────────────────────────────────────────────
void test_Y03_get_default_respected() {
    TEST("Y03 get() returns default when key has empty value");
    std::string yaml =
        "name:\n"
        "version: \"1.0\"\n";

    auto root = ManifestYaml::parse(yaml);
    // Note: `name:` is a parent of children, so really this exercises the
    // case where the key exists but its inline value is empty. The caller
    // expects the supplied default, not "".
    std::string result = ManifestYaml::get(root, "name", "DEFAULT_NAME");
    EXPECT_EQ(result, std::string("DEFAULT_NAME"), "expected default to be returned");
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Y05 — inline comment after value: `temperature: 0.7 # default`
//   Current bug: value becomes "0.7 # default" → stoi/stod throws on load.
// ─────────────────────────────────────────────────────────────────────────────
void test_Y05_inline_comment_stripped() {
    TEST("Y05 inline comment after value");
    std::string yaml =
        "temperature: 0.7 # default temperature\n"
        "max_tokens: 4096\n";

    auto root = ManifestYaml::parse(yaml);
    std::string temp = ManifestYaml::get(root, "temperature");
    std::string tokens = ManifestYaml::get(root, "max_tokens");

    EXPECT_EQ(temp, std::string("0.7"), "comment must be stripped from value");
    EXPECT_EQ(tokens, std::string("4096"), "comment-free value should be intact");
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Sanity — existing happy path should keep working
// ─────────────────────────────────────────────────────────────────────────────
void test_block_style_still_works() {
    TEST("baseline: block style cognitive_engine");
    std::string yaml =
        "cognitive_engine:\n"
        "  primary:\n"
        "    provider: deepseek\n"
        "    model: deepseek-chat\n"
        "    parameters:\n"
        "      temperature: 0.3\n";

    auto root = ManifestYaml::parse(yaml);
    auto* ce = ManifestYaml::find(root, "cognitive_engine");
    if (!ce) FAIL("cognitive_engine missing");
    auto* primary = ManifestYaml::find(*ce, "primary");
    if (!primary) FAIL("primary missing");
    EXPECT_EQ(ManifestYaml::get(*primary, "provider"), std::string("deepseek"), "provider");
    EXPECT_EQ(ManifestYaml::get(*primary, "model"), std::string("deepseek-chat"), "model");
    PASS();
}

void test_list_items_still_work() {
    TEST("baseline: import list resolves");
    std::string yaml =
        "import:\n"
        "  tools:\n"
        "    - exec\n"
        "    - list\n"
        "    - ./tools/scaffold/tool.yml\n";

    auto root = ManifestYaml::parse(yaml);
    auto* imp = ManifestYaml::find(root, "import");
    if (!imp) FAIL("import missing");
    auto tools = ManifestYaml::getList(*imp, "tools");
    if (tools.size() != 3) {
        std::cout << "    got " << tools.size() << " items, want 3\n";
        FAIL("expected 3 tools");
    }
    EXPECT_EQ(tools[0], std::string("exec"), "tools[0]");
    EXPECT_EQ(tools[1], std::string("list"), "tools[1]");
    EXPECT_EQ(tools[2], std::string("./tools/scaffold/tool.yml"), "tools[2]");
    PASS();
}

int main() {
    std::cout << "\n╔══════════════════════════════════════════╗\n";
    std::cout << "║   MK3 mini_yaml Unit Tests               ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n\n";

    test_block_style_still_works();
    test_list_items_still_work();
    test_Y01_inline_flow_map();
    test_Y02a_block_scalar_fold();
    test_Y02b_block_scalar_literal();
    test_Y03_get_default_respected();
    test_Y05_inline_comment_stripped();

    std::cout << "\n──────────────────────────────────────────\n";
    std::cout << "  " << passed << " passed, " << failed << " failed\n";
    std::cout << "──────────────────────────────────────────\n";
    return failed > 0 ? 1 : 0;
}
