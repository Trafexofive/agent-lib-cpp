# agent-lib-MK3 — Build System
# Lean, modular C++17 agent runtime with native DeepSeek inference.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -g -fPIC -MMD -MP
LDFLAGS  ?= -lcurl -ljsoncpp -lpthread -lreadline

SRC_DIR   := src
BUILD_DIR := build

# Include paths
INC_DIRS  := . $(SRC_DIR) $(shell find $(SRC_DIR) -type d) /usr/include/jsoncpp
CXXFLAGS  += $(foreach dir,$(INC_DIRS),-I$(dir))

# Source files (exclude test files)
SRCS := $(shell find $(SRC_DIR) -name '*.cpp' ! -path '*/testing/*' ! -name 'call_tool.cpp')
OBJS := $(SRCS:%.cpp=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d) $(BUILD_DIR)/main.o.d $(BUILD_DIR)/server_main.o.d

# Targets
BIN_CLI    := cortex-mk3
BIN_SERVER := cortex-mk3-server
LIB_SHARED := libagent-mk3.so
LIB_STATIC := libagent-mk3.a

.PHONY: all lib clean run test install uninstall format lint watch dev all-tests smoke

all: $(BIN_CLI) $(BIN_SERVER) lib

lib: $(LIB_SHARED) $(LIB_STATIC)

# ── CLI binary ──
$(BUILD_DIR)/main.o: main.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BIN_CLI): $(OBJS) $(BUILD_DIR)/main.o
	$(CXX) $^ -o $@ $(LDFLAGS)

# ── Server binary ──
$(BUILD_DIR)/server_main.o: server_main.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BIN_SERVER): $(OBJS) $(BUILD_DIR)/server_main.o
	$(CXX) $^ -o $@ $(LDFLAGS)

# ── Library ──
$(LIB_SHARED): $(OBJS)
	$(CXX) -shared -o $@ $^ $(LDFLAGS)

$(LIB_STATIC): $(OBJS)
	ar rcs $@ $^

# ── Object compilation ──
$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ── Utility ──
clean:
	rm -rf $(BUILD_DIR) $(BIN_CLI) $(BIN_SERVER) $(LIB_SHARED) $(LIB_STATIC) parser-test

-include $(DEPS)

# ── Parser unit test ──
PARSER_TEST_SRC := src/testing/parser_test.cpp
PARSER_TEST_OBJ := $(BUILD_DIR)/testing/parser_test.o
PARSER_TEST_BIN := parser-test

$(PARSER_TEST_OBJ): $(PARSER_TEST_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(PARSER_TEST_BIN): $(OBJS) $(PARSER_TEST_OBJ)
	$(CXX) $(filter-out $(BUILD_DIR)/main.o,$(OBJS)) $(PARSER_TEST_OBJ) -o $@ $(LDFLAGS)

test-parser: $(PARSER_TEST_BIN)
	./$(PARSER_TEST_BIN)

# ── Protocol test runner ──
PROTOCOL_TEST_SRC := src/testing/protocol_test.cpp
PROTOCOL_TEST_OBJ := $(BUILD_DIR)/testing/protocol_test.o
PROTOCOL_TEST_BIN := protocol-test

$(PROTOCOL_TEST_OBJ): $(PROTOCOL_TEST_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(PROTOCOL_TEST_BIN): $(OBJS) $(PROTOCOL_TEST_OBJ)
	$(CXX) $(filter-out $(BUILD_DIR)/main.o,$(OBJS)) $(PROTOCOL_TEST_OBJ) -o $@ $(LDFLAGS)

test-protocol: $(PROTOCOL_TEST_BIN)
	./$(PROTOCOL_TEST_BIN)

test-protocol-list: $(PROTOCOL_TEST_BIN)
	./$(PROTOCOL_TEST_BIN) --list

# Feed manifest tests
FEED_MANIFEST_TEST_SRC = src/testing/feed_manifest_runner.cpp
FEED_MANIFEST_TEST_BIN = feed-manifest-test

$(FEED_MANIFEST_TEST_BIN): $(OBJS) $(FEED_MANIFEST_TEST_SRC)
	$(CXX) $(CXXFLAGS) $(FEED_MANIFEST_TEST_SRC) $(OBJS) -o $@ $(LDFLAGS)

test-feeds: $(FEED_MANIFEST_TEST_BIN)
	@./$(FEED_MANIFEST_TEST_BIN)

# Docker relic dispatcher tests
DOCKER_RELIC_TEST_SRC = src/testing/docker_relic_runner.cpp
DOCKER_RELIC_TEST_BIN = docker-relic-test

$(DOCKER_RELIC_TEST_BIN): $(DOCKER_RELIC_TEST_SRC)
	$(CXX) $(CXXFLAGS) $(DOCKER_RELIC_TEST_SRC) -o $@ $(LDFLAGS)

test-relics: $(DOCKER_RELIC_TEST_BIN)
	@./$(DOCKER_RELIC_TEST_BIN)

# call-tool helper for feed scripts
CALL_TOOL_BIN = call-tool
$(CALL_TOOL_BIN): src/tools/call_tool.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) src/tools/call_tool.cpp $(OBJS) -o $@ $(LDFLAGS)

# ask_tool built-in test
ASK_CARDS_TEST_SRC = src/testing/ask_tool_test.cpp
ASK_CARDS_TEST_BIN = ask-tool-test
$(ASK_CARDS_TEST_BIN): $(ASK_CARDS_TEST_SRC) $(OBJS)
	$(CXX) $(CXXFLAGS) $(ASK_CARDS_TEST_SRC) $(OBJS) -o $@ $(LDFLAGS)
test-ask-cards: $(ASK_CARDS_TEST_BIN)
	@./$(ASK_CARDS_TEST_BIN)

run: $(BIN_CLI)
	./$(BIN_CLI)

run-server: $(BIN_SERVER)
	./$(BIN_SERVER)

test: $(BIN_CLI)
	@echo "=== Smoke test: provider listing ==="
	./$(BIN_CLI) --list-providers
	@echo "=== Smoke test: help ==="
	./$(BIN_CLI) --help
	@echo "=== Smoke test: version ==="
	./$(BIN_CLI) --version

# ── Prompt tuning ──
SUITE ?= basic-protocol
tune-prompt: $(BIN_CLI)
	@tests/prompt-tests/tune tests/prompt-tests/suites/$(SUITE).yml $(ARGS)

tune-diff:
	@tests/prompt-tests/tune tests/prompt-tests/suites/$(SUITE).yml --diff

tune-cat:
	@tests/prompt-tests/tune tests/prompt-tests/suites/$(SUITE).yml --cat

# ── Sandbox policy tests ──
POLICY_TEST_SRC = tests/policy_test.cpp
POLICY_TEST_BIN = policy-test
$(POLICY_TEST_BIN): $(POLICY_TEST_SRC)
	$(CXX) $(CXXFLAGS) -Isrc -Isrc/sandbox $(POLICY_TEST_SRC) -o $@
test-policy: $(POLICY_TEST_BIN)
	@./$(POLICY_TEST_BIN)

# ── TUI tests ──
TUI_TERMINAL_TEST_SRC = tests/tui/terminal_test.cpp
TUI_TERMINAL_TEST_BIN = tests/tui/terminal_test
$(TUI_TERMINAL_TEST_BIN): $(TUI_TERMINAL_TEST_SRC)
	$(CXX) $(CXXFLAGS) -Isrc $(TUI_TERMINAL_TEST_SRC) -o $@
test-tui-terminal: $(TUI_TERMINAL_TEST_BIN)
	@./$(TUI_TERMINAL_TEST_BIN)

# ── Install / Uninstall ──
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

install: $(BIN_CLI)
	@mkdir -p $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN_CLI) $(DESTDIR)$(BINDIR)/$(BIN_CLI)
	@echo "✓ installed $(BIN_CLI) → $(DESTDIR)$(BINDIR)/$(BIN_CLI)"
	@if [ -f $(BIN_SERVER) ]; then install -m 755 $(BIN_SERVER) $(DESTDIR)$(BINDIR)/$(BIN_SERVER); echo "✓ installed $(BIN_SERVER) → $(DESTDIR)$(BINDIR)/$(BIN_SERVER)"; fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN_CLI)
	rm -f $(DESTDIR)$(BINDIR)/$(BIN_SERVER)
	@echo "✓ uninstalled from $(DESTDIR)$(BINDIR)"

# ── Code quality ──
format:
	@which clang-format >/dev/null 2>&1 || { echo "clang-format not found — install with: sudo pacman -S clang"; exit 1; }
	find src main.cpp server_main.cpp -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i -style=file 2>/dev/null
	@echo "✓ formatted"

lint:
	@which clang-tidy >/dev/null 2>&1 || { echo "clang-tidy not found — install with: sudo pacman -S clang"; exit 1; }
	find src -name '*.cpp' ! -path '*/testing/*' | xargs clang-tidy -p build --quiet 2>/dev/null || true
	@echo "✓ lint complete"

# ── Development shortcuts ──
watch:
	@which entr >/dev/null 2>&1 || { echo "entr not found — install with: sudo pacman -S entr"; exit 1; }
	find src main.cpp -name '*.cpp' -o -name '*.hpp' | entr -c make cortex-mk3

smoke: $(BIN_CLI)
	@echo "=== smoke: version ===" && ./$(BIN_CLI) version 2>/dev/null
	@echo "=== smoke: built ===" && ls -lh $(BIN_CLI)

dev: clean all
	@$(MAKE) -s smoke
	@$(MAKE) -s test-parser
	@$(MAKE) -s test-protocol 2>/dev/null | tail -3
	@echo "✓ dev build complete"

all-tests: test-parser test-protocol test-feeds test-policy
	@echo "✓ all tests passed"
