# agent-lib-MK3 — Build System
# Lean, modular C++17 agent runtime with native DeepSeek inference.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -g -fPIC -MMD -MP
LDFLAGS  ?= -lcurl -ljsoncpp -lpthread -lreadline

SRC_DIR   := src
BUILD_DIR := build

# inkcell sibling dependency (new TUI path). Override INKCELL_ROOT if installed elsewhere.
INKCELL_ROOT ?= ../inkcell
INKCELL_LIB  := $(INKCELL_ROOT)/build/libinkcell.a

# Include paths
INC_DIRS  := . $(SRC_DIR) $(shell find $(SRC_DIR) -type d) /usr/include/jsoncpp $(INKCELL_ROOT)/include
CXXFLAGS  += $(foreach dir,$(INC_DIRS),-I$(dir))

# Source files (exclude test files)
SRCS := $(shell find $(SRC_DIR) -name '*.cpp' ! -path '*/testing/*' ! -name 'call_tool.cpp')
OBJS := $(SRCS:%.cpp=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d) $(BUILD_DIR)/main.d $(BUILD_DIR)/server_main.d

# Targets
BIN_CLI    := cortex-mk3
BIN_SERVER := cortex-mk3-server
LIB_SHARED := libagent-mk3.so
LIB_STATIC := libagent-mk3.a

.PHONY: all lib clean run test install uninstall format lint watch dev all-tests smoke inkcell-lib

all: $(BIN_CLI) $(BIN_SERVER) lib

lib: $(LIB_SHARED) $(LIB_STATIC)

# ── CLI binary ──
inkcell-lib:
	$(MAKE) -C $(INKCELL_ROOT) lib

$(BUILD_DIR)/main.o: main.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BIN_CLI): $(OBJS) $(BUILD_DIR)/main.o | inkcell-lib
	$(CXX) $^ -o $@ $(INKCELL_LIB) $(LDFLAGS)

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

$(PARSER_TEST_OBJ): $(PARSER_TEST_SRC) src/protocol/parser.hpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(PARSER_TEST_BIN): $(OBJS) $(PARSER_TEST_OBJ)
	$(CXX) $(filter-out $(BUILD_DIR)/main.o,$(OBJS)) $(PARSER_TEST_OBJ) -o $@ $(LDFLAGS)

test-parser: $(PARSER_TEST_BIN)
	./$(PARSER_TEST_BIN)

# ── UI model tests (timeline/adapters; no TUI rendering) ──
UI_MODEL_TEST_SRC := src/testing/ui_model_test.cpp
UI_MODEL_TEST_BIN := ui-model-test

$(UI_MODEL_TEST_BIN): $(OBJS) $(UI_MODEL_TEST_SRC) src/ui/chat/ask_dialog_model.hpp src/ui/chat/chat_blocks.hpp src/ui/chat/chat_command_catalog.hpp src/ui/chat/chat_commands.hpp src/ui/chat/chat_io.hpp src/ui/chat/prompt_history.hpp src/ui/chat/transcript_cache.hpp src/ui/model/dashboard_controller.hpp src/ui/model/dashboard_model.hpp src/ui/model/timeline_model.hpp src/ui/model/app_context.hpp src/ui/model/command_model.hpp src/ui/model/inkcell_app_model.hpp src/ui/model/navigation_model.hpp src/ui/model/adapters/protocol_to_timeline.hpp src/ui/model/adapters/agent_tree.hpp
	$(CXX) $(CXXFLAGS) $(UI_MODEL_TEST_SRC) $(filter-out $(BUILD_DIR)/main.o,$(OBJS)) -o $@ $(LDFLAGS)

test-ui-model: $(UI_MODEL_TEST_BIN)
	./$(UI_MODEL_TEST_BIN)

# ── UI view tests (fixture-only Surface rendering) ──
UI_VIEW_TEST_SRC := src/testing/ui_view_test.cpp
UI_VIEW_TEST_BIN := ui-view-test

$(UI_VIEW_TEST_BIN): $(UI_VIEW_TEST_SRC) src/ui/chat/chat_view.hpp src/ui/views/timeline_view.hpp src/ui/model/timeline_model.hpp src/ui/layout/sbtui_layout.hpp src/ui/theme/cortex_theme.hpp
	$(CXX) $(CXXFLAGS) $(UI_VIEW_TEST_SRC) -o $@ $(LDFLAGS)

test-ui-view: $(UI_VIEW_TEST_BIN)
	./$(UI_VIEW_TEST_BIN)

# ── Chat scene integration tests (ask bridge, slash input, cancellation) ──
CHAT_SCENE_TEST_SRC := src/testing/chat_scene_test.cpp
CHAT_SCENE_TEST_BIN := chat-scene-test

$(CHAT_SCENE_TEST_BIN): $(OBJS) $(CHAT_SCENE_TEST_SRC) src/ui/scenes/agent_scene.hpp src/ui/scenes/main_scene.hpp src/ui/model/dashboard_controller.hpp src/ui/model/dashboard_model.hpp src/ui/chat/ask_dialog_model.hpp src/ui/chat/chat_commands.hpp
	$(CXX) $(CXXFLAGS) $(CHAT_SCENE_TEST_SRC) $(filter-out $(BUILD_DIR)/main.o,$(OBJS)) -o $@ $(INKCELL_LIB) $(LDFLAGS)

test-chat-scene: $(CHAT_SCENE_TEST_BIN) | inkcell-lib
	./$(CHAT_SCENE_TEST_BIN)

# ── Parser + streaming reducer performance regression gates ──
PERF_TEST_SRC := src/testing/perf_test.cpp
PERF_TEST_BIN := perf-test

$(PERF_TEST_BIN): $(OBJS) $(PERF_TEST_SRC) src/protocol/parser.hpp src/ui/bridge/agent_bridge.hpp src/ui/model/inkcell_app_model.hpp
	$(CXX) $(CXXFLAGS) $(PERF_TEST_SRC) $(filter-out $(BUILD_DIR)/main.o,$(OBJS)) -o $@ $(INKCELL_LIB) $(LDFLAGS)

test-perf: $(PERF_TEST_BIN) | inkcell-lib
	./$(PERF_TEST_BIN)

# ── mini_yaml unit tests ──
YAML_TEST_SRC := src/testing/yaml_test.cpp
YAML_TEST_BIN := yaml-test

$(YAML_TEST_BIN): $(YAML_TEST_SRC) src/core/mini_yaml.hpp
	$(CXX) $(CXXFLAGS) $(YAML_TEST_SRC) -o $@

test-yaml: $(YAML_TEST_BIN)
	./$(YAML_TEST_BIN)

# ── Manifest classifier tests (ML01) ──
MCLASS_TEST_SRC := src/testing/manifest_classifier_test.cpp
MCLASS_TEST_BIN := manifest-classifier-test

$(MCLASS_TEST_BIN): $(MCLASS_TEST_SRC) src/core/manifest_loader.hpp src/core/mini_yaml.hpp
	$(CXX) $(CXXFLAGS) $(MCLASS_TEST_SRC) -o $@ $(LDFLAGS)

test-manifest-classifier: $(MCLASS_TEST_BIN)
	./$(MCLASS_TEST_BIN)

# ── Manifest semantics tests (agent.yml source-of-truth) ──
MSEM_TEST_SRC := src/testing/manifest_semantics_test.cpp
MSEM_TEST_OBJ := $(BUILD_DIR)/testing/manifest_semantics_test.o
MSEM_TEST_BIN := manifest-semantics-test

$(MSEM_TEST_OBJ): $(MSEM_TEST_SRC) src/core/manifest_loader.hpp src/core/agent.hpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(MSEM_TEST_BIN): $(OBJS) $(MSEM_TEST_OBJ)
	$(CXX) $(filter-out $(BUILD_DIR)/main.o,$(OBJS)) $(MSEM_TEST_OBJ) -o $@ $(LDFLAGS)

test-manifest-semantics: $(MSEM_TEST_BIN)
	./$(MSEM_TEST_BIN)

# ── Recursive autoload tests (MA01) ──
AUTOLOAD_TEST_SRC := src/testing/autoload_test.cpp
AUTOLOAD_TEST_OBJ := $(BUILD_DIR)/testing/autoload_test.o
AUTOLOAD_TEST_BIN := autoload-test

$(AUTOLOAD_TEST_OBJ): $(AUTOLOAD_TEST_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(AUTOLOAD_TEST_BIN): $(OBJS) $(AUTOLOAD_TEST_OBJ)
	$(CXX) $(filter-out $(BUILD_DIR)/main.o,$(OBJS)) $(AUTOLOAD_TEST_OBJ) -o $@ $(LDFLAGS)

test-autoload: $(AUTOLOAD_TEST_BIN)
	./$(AUTOLOAD_TEST_BIN)

# ── Context manager tests (pin / peek / unpin) ──
CTX_TEST_SRC := src/testing/context_test.cpp
CTX_TEST_OBJ := $(BUILD_DIR)/testing/context_test.o
CTX_TEST_BIN := context-test

$(CTX_TEST_OBJ): $(CTX_TEST_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(CTX_TEST_BIN): $(OBJS) $(CTX_TEST_OBJ)
	$(CXX) $(filter-out $(BUILD_DIR)/main.o,$(OBJS)) $(CTX_TEST_OBJ) -o $@ $(LDFLAGS)

test-context: $(CTX_TEST_BIN)
	./$(CTX_TEST_BIN)

# ── Session round-trip tests (AC14/04/17/18) ──
SESS_TEST_SRC := src/testing/session_test.cpp
SESS_TEST_OBJ := $(BUILD_DIR)/testing/session_test.o
SESS_TEST_BIN := session-test

$(SESS_TEST_OBJ): $(SESS_TEST_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(SESS_TEST_BIN): $(OBJS) $(SESS_TEST_OBJ)
	$(CXX) $(filter-out $(BUILD_DIR)/main.o,$(OBJS)) $(SESS_TEST_OBJ) -o $@ $(LDFLAGS)

test-session: $(SESS_TEST_BIN)
	./$(SESS_TEST_BIN)

# ── Sandbox + context_* integration tests (SB02/SB07/BT04) ──
SBOX_TEST_SRC := src/testing/sandbox_context_test.cpp
SBOX_TEST_OBJ := $(BUILD_DIR)/testing/sandbox_context_test.o
SBOX_TEST_BIN := sandbox-context-test

$(SBOX_TEST_OBJ): $(SBOX_TEST_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(SBOX_TEST_BIN): $(OBJS) $(SBOX_TEST_OBJ)
	$(CXX) $(filter-out $(BUILD_DIR)/main.o,$(OBJS)) $(SBOX_TEST_OBJ) -o $@ $(LDFLAGS)

test-sandbox-context: $(SBOX_TEST_BIN)
	./$(SBOX_TEST_BIN)

# ── Protocol test runner ──
PROTOCOL_TEST_SRC := src/testing/protocol_test.cpp
PROTOCOL_TEST_OBJ := $(BUILD_DIR)/testing/protocol_test.o
PROTOCOL_TEST_BIN := protocol-test

$(PROTOCOL_TEST_OBJ): $(PROTOCOL_TEST_SRC) src/protocol/parser.hpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(PROTOCOL_TEST_BIN): $(OBJS) $(PROTOCOL_TEST_OBJ)
	$(CXX) $(filter-out $(BUILD_DIR)/main.o,$(OBJS)) $(PROTOCOL_TEST_OBJ) -o $@ $(LDFLAGS)

test-protocol: $(PROTOCOL_TEST_BIN)
	./$(PROTOCOL_TEST_BIN)

test-protocol-list: $(PROTOCOL_TEST_BIN)
	./$(PROTOCOL_TEST_BIN) --list

# ── Workflow engine tests ──
WORKFLOW_ENGINE_TEST_SRC := src/testing/workflow_engine_test.cpp
WORKFLOW_ENGINE_TEST_BIN := workflow-engine-test

$(WORKFLOW_ENGINE_TEST_BIN): $(WORKFLOW_ENGINE_TEST_SRC)
	$(CXX) $(CXXFLAGS) $(WORKFLOW_ENGINE_TEST_SRC) -o $@ $(LDFLAGS)

test-workflows: $(WORKFLOW_ENGINE_TEST_BIN)
	./$(WORKFLOW_ENGINE_TEST_BIN)

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

$(DOCKER_RELIC_TEST_BIN): $(DOCKER_RELIC_TEST_SRC) build/src/utils/process.o
	$(CXX) $(CXXFLAGS) $(DOCKER_RELIC_TEST_SRC) build/src/utils/process.o -o $@ $(LDFLAGS)

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

# Provider model metadata tests
PROVIDER_MODEL_INFO_TEST_SRC = src/testing/provider_model_info_test.cpp
PROVIDER_MODEL_INFO_TEST_BIN = provider-model-info-test
$(PROVIDER_MODEL_INFO_TEST_BIN): $(PROVIDER_MODEL_INFO_TEST_SRC) $(BUILD_DIR)/src/providers/generic_openai.o
	$(CXX) $(CXXFLAGS) $(PROVIDER_MODEL_INFO_TEST_SRC) $(BUILD_DIR)/src/providers/generic_openai.o -o $@ $(LDFLAGS)

test-provider-model-info: $(PROVIDER_MODEL_INFO_TEST_BIN)
	@./$(PROVIDER_MODEL_INFO_TEST_BIN)

# ── ScriptedProvider — test-only queue-driven ILlmProvider fake ──────────────
# Enabler for the sub-agent test phase: lets a test script the exact protocol
# responses the parent and child agents emit, so we can drive real
# <action type="agent"> delegation, tool calls, multi-turn flows, and edge
# cases without touching the network. Header-only — no .o to link.
SCRIPTED_PROVIDER_TEST_SRC := src/testing/scripted_provider_test.cpp
SCRIPTED_PROVIDER_TEST_BIN := scripted-provider-test

$(SCRIPTED_PROVIDER_TEST_BIN): $(SCRIPTED_PROVIDER_TEST_SRC) src/testing/scripted_provider.hpp
	$(CXX) $(CXXFLAGS) $(SCRIPTED_PROVIDER_TEST_SRC) -o $@ $(LDFLAGS)

test-scripted-provider: $(SCRIPTED_PROVIDER_TEST_BIN)
	@./$(SCRIPTED_PROVIDER_TEST_BIN)

# ── Live smoke: real-model mid-low live test against opencode-go ───────────
# Runs cortex-mk3 against a real LLM on the opencode-go router and asserts
# the rendered chat surface contains the expected response + header. Catches
# what offline tests cannot: real provider HTTP, real streaming, real
# protocol shape. Graceful skip when OPENCODE_API_KEY is unset (live tests
# are opt-in; never breaks keyless/CI runs).
LIVE_SMOKE_BIN ?= $(BIN_CLI)
LIVE_SMOKE_MODEL ?= deepseek-v4-flash
LIVE_SMOKE_TIMEOUT ?= 90

live-smoke: $(LIVE_SMOKE_BIN)
	@bash tests/tui/live_smoke.sh $(LIVE_SMOKE_BIN) $(LIVE_SMOKE_MODEL) $(LIVE_SMOKE_TIMEOUT)

run: $(BIN_CLI)
	./$(BIN_CLI)

run-server: $(BIN_SERVER)
	./$(BIN_SERVER)

test: $(BIN_CLI)
	@echo "=== Smoke test: provider listing ==="
	./$(BIN_CLI) list --providers
	@echo "=== Smoke test: help ==="
	./$(BIN_CLI) --help
	@echo "=== Smoke test: version ==="
	./$(BIN_CLI) version

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

TUI_RENDER_TEST_SRC = tests/tui/render_test.cpp
TUI_RENDER_TEST_BIN = tests/tui/render_test
$(TUI_RENDER_TEST_BIN): $(TUI_RENDER_TEST_SRC)
	$(CXX) $(CXXFLAGS) -Isrc $(TUI_RENDER_TEST_SRC) -o $@ $(LDFLAGS)
test-tui-render: $(TUI_RENDER_TEST_BIN)
	@./$(TUI_RENDER_TEST_BIN)

# ── DeepSearchStack staging module smoke ──
test-dss-module: $(BIN_CLI)
	@tests/deepsearch_stack_manifest_smoke.sh

# ── Install / Uninstall ──
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

install: $(BIN_CLI)
	@mkdir -p $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN_CLI) $(DESTDIR)$(BINDIR)/$(BIN_CLI)
	@echo "✓ installed $(BIN_CLI) → $(DESTDIR)$(BINDIR)/$(BIN_CLI)"
	@if [ -f $(BIN_SERVER) ]; then install -m 755 $(BIN_SERVER) $(DESTDIR)$(BINDIR)/$(BIN_SERVER); echo "✓ installed $(BIN_SERVER) → $(DESTDIR)$(BINDIR)/$(BIN_SERVER)"; fi

reinstall: install
	@echo "✓ reinstalled"

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

all-tests: test-yaml test-manifest-classifier test-autoload test-context test-session test-parser test-protocol test-workflows test-feeds test-policy test-subagent
	@echo "✓ all tests passed"

# ── Sub-agent delegation test ──
SUBAGENT_TEST_SRC := src/testing/subagent_delegation_test.cpp
SUBAGENT_TEST_BIN := subagent-delegation-test

$(SUBAGENT_TEST_BIN): $(SUBAGENT_TEST_SRC) src/testing/scripted_provider.hpp
	$(CXX) $(CXXFLAGS) $(SUBAGENT_TEST_SRC) $(filter-out $(BUILD_DIR)/main.o,$(OBJS)) -o $@ $(LDFLAGS)

test-subagent: $(SUBAGENT_TEST_BIN)
	./$(SUBAGENT_TEST_BIN)


# ── Multi-turn compliance + latency benchmark ──
BENCH_ARGS ?=
bench:
	python3 tests/bench.py $(BENCH_ARGS)

bench-smoke:
	python3 tests/bench.py --scenario smoke $(BENCH_ARGS)

bench-baseline:
	python3 tests/bench.py --baseline $(BENCH_ARGS)

bench-list:
	python3 tests/bench.py --list

# ── Self-evolving agent loop ──
EVOLVE_ARGS ?=
evolve: $(BIN_CLI)
	python3 tests/self-evolve/runner.py $(EVOLVE_ARGS)

evolve-task: $(BIN_CLI)
	python3 tests/self-evolve/runner.py --task-only $(EVOLVE_ARGS)

evolve-cheap: $(BIN_CLI)
	python3 tests/self-evolve/runner.py --model deepseek-chat --provider deepseek $(EVOLVE_ARGS)
