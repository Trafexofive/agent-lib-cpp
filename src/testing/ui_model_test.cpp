#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "src/ui/chat/chat_commands.hpp"
#include "src/ui/chat/prompt_history.hpp"
#include "src/ui/model/adapters/agent_tree.hpp"
#include "src/ui/model/adapters/protocol_to_timeline.hpp"
#include "src/ui/model/command_model.hpp"
#include "src/ui/model/dashboard_controller.hpp"
#include "src/ui/model/inkcell_app_model.hpp"
#include "src/ui/model/navigation_model.hpp"
#include "src/ui/model/workflow_run_model.hpp"
#include "src/ui/model/workflow_runner.hpp"
#include "src/workflows/workflow.hpp"

using namespace cortex::mk3;
using namespace cortex::mk3::ui;
using namespace cortex::mk3::ui::model;

// Minimal provider stub for nesting tests — returns a fixed response so a
// child Agent's prompt() populates its protocolEvents with real content.
namespace {
class NestNoopProvider : public ILlmProvider {
   public:
    explicit NestNoopProvider(std::string resp = "<response final=\"true\">ok</response>")
        : resp_(std::move(resp)) {}
    std::string generate(const ChatMessages&) override { return resp_; }
    void generateStream(const ChatMessages&, StreamCallback cb) override { cb(resp_, true); }
    void setModel(const std::string&) override {}
    void setTemperature(double) override {}
    void setMaxTokens(int) override {}
    void setTopP(double) override {}
    std::string getModel() const override { return "noop"; }
    double getTemperature() const override { return 0.7; }
    int getMaxTokens() const override { return 65536; }
    std::vector<ModelInfo> listModels() override { return {}; }
    std::string providerName() const override { return "noop"; }
    // Report anyContent=true so the agent loop accepts the stream and
    // doesn't retry (the default lastStreamStats() returns anyContent=false,
    // which made runLoop clear protocolEvents_ and bail with 'stopped
    // without emitting').
    StreamStats lastStreamStats() const override {
        StreamStats s;
        s.anyContent = true;
        return s;
    }
   private:
    std::string resp_;
};
}  // namespace

namespace {
int failures = 0;

void check(bool cond, const std::string& name) {
    if (cond) {
        std::cout << "  " << name << "... PASS\n";
    } else {
        std::cout << "  " << name << "... FAIL\n";
        ++failures;
    }
}

ProtocolEvent actionEvent(std::string type, std::string name, std::string id, std::string body = "") {
    ProtocolEvent ev;
    ev.kind = ProtocolEventKind::ACTION;
    ev.action.type = std::move(type);
    ev.action.name = std::move(name);
    ev.action.id = std::move(id);
    ev.action.body = std::move(body);
    return ev;
}

ProtocolEvent resultEvent(std::string id, bool ok, std::string summary, std::string toolName = "") {
    ProtocolEvent ev;
    ev.kind = ProtocolEventKind::RESULT;
    ev.result.id = std::move(id);
    ev.result.ok = ok;
    ev.result.summary = std::move(summary);
    ev.result.toolName = std::move(toolName);
    ev.result.elapsedMs = 12;
    ev.result.outputBytes = ev.result.summary.size();
    return ev;
}

ProtocolEvent responseEvent(std::string text) {
    ProtocolEvent ev;
    ev.kind = ProtocolEventKind::RESPONSE;
    ev.text = std::move(text);
    return ev;
}

ProtocolEvent thoughtEvent(std::string text) {
    ProtocolEvent ev;
    ev.kind = ProtocolEventKind::THOUGHT;
    ev.text = std::move(text);
    return ev;
}

void test_tool_action_result() {
    std::vector<ProtocolEvent> events = {
        actionEvent("tool", "fs_read", "a1", "{path:'x'}"),
        resultEvent("a1", true, "read 42 bytes", "fs_read"),
        responseEvent("done"),
    };
    auto blocks = protocolEventsToTimeline(events);
    check(blocks.size() == 3, "tool action/result/response count");
    check(blocks[0].kind == BlockKind::Action && blocks[0].status == BlockStatus::Pending,
          "tool action becomes pending action block");
    check(!blocks[0].drillable, "tool action is not drillable");
    check(blocks[1].kind == BlockKind::Result && blocks[1].status == BlockStatus::Ok,
          "ok result becomes ok result block");
    check(blocks[1].metadata["elapsed_ms"] == "12", "result metadata includes elapsed ms");
}

void test_agent_drillability() {
    ProtocolTimelineOptions opts;
    opts.childAgentNames = {"reader"};
    std::vector<ProtocolEvent> events = {
        actionEvent("agent", "reader", "r1", "Inspect files"),
        resultEvent("r1", true, "reader summary", "reader"),
    };
    auto blocks = protocolEventsToTimeline(events, opts);
    check(blocks.size() == 2, "agent action/result count");
    check(blocks[0].drillable && blocks[0].related.available, "agent action drills when child exists");
    check(blocks[0].related.agentPath.size() == 1 && blocks[0].related.agentPath[0] == "reader",
          "agent action related path points to child");
    check(blocks[1].drillable && blocks[1].actionName == "reader",
          "agent result inherits drill target from matching action");
}

void test_missing_child_not_drillable() {
    ProtocolTimelineOptions opts;
    opts.childAgentNames = {"tester"};
    std::vector<ProtocolEvent> events = {actionEvent("agent", "reader", "r1", "Inspect files")};
    auto blocks = protocolEventsToTimeline(events, opts);
    check(blocks.size() == 1, "missing child action count");
    check(!blocks[0].drillable && !blocks[0].related.available,
          "agent action is not drillable when child missing");
}

void test_thought_toggle_and_path_prefix() {
    ProtocolTimelineOptions opts;
    opts.path.parts = {"reader", "grep"};
    opts.showThoughts = false;
    std::vector<ProtocolEvent> events = {thoughtEvent("hidden"), responseEvent("visible")};
    auto hidden = protocolEventsToTimeline(events, opts);
    check(hidden.size() == 1 && hidden[0].kind == BlockKind::Response,
          "thoughts hidden by default option");
    opts.showThoughts = true;
    auto shown = protocolEventsToTimeline(events, opts);
    check(shown.size() == 2 && shown[0].kind == BlockKind::Thought,
          "thoughts visible when enabled");
    check(shown[0].stableId.find("root/reader/grep:thought:") == 0,
          "nested stable id includes agent path");
}

void test_generic_path_exists() {
    auto exists = [](const AgentPath& parent, const std::string& child) {
        if (parent.parts.empty()) return child == "reader" || child == "tester";
        if (parent.parts.size() == 1 && parent.parts[0] == "reader") return child == "grep";
        return false;
    };
    AgentPath ok;
    ok.parts = {"reader", "grep"};
    AgentPath bad;
    bad.parts = {"reader", "missing"};
    check(pathExists(ok, exists), "generic nested path exists");
    check(!pathExists(bad, exists), "generic missing nested path fails");
}

const CommandSpec* findCommand(const std::vector<CommandSpec>& commands, const std::string& id) {
    for (const auto& c : commands)
        if (c.id == id) return &c;
    return nullptr;
}

void test_command_inventory_agent_history() {
    UiContext ctx;
    ctx.view = AppView::AgentHistory;
    ctx.focus = FocusPane::Composer;
    ctx.provider.configured = true;
    ctx.hasSelection = true;
    ctx.selectedHasDetail = true;
    ctx.selectedDrillable = true;
    auto commands = commandsForContext(ctx);
    auto* send = findCommand(commands, "run.send_prompt");
    auto* drill = findCommand(commands, "agent.drill");
    auto* copy = findCommand(commands, "copy.selected");
    check(send && send->enabled, "send prompt enabled when composer focused and provider configured");
    check(drill && drill->enabled, "drill command enabled for drillable selection");
    check(copy && copy->enabled, "copy command enabled for selected block");
}

void test_command_inventory_disabled_reasons() {
    UiContext ctx;
    ctx.view = AppView::AgentHistory;
    ctx.focus = FocusPane::Composer;
    ctx.provider.configured = false;
    auto commands = commandsForContext(ctx);
    auto* send = findCommand(commands, "run.send_prompt");
    auto* detail = findCommand(commands, "detail.open");
    check(send && !send->enabled && send->disabledReason == "provider not configured",
          "send prompt disabled with provider reason");
    check(detail && !detail->enabled && detail->disabledReason == "no block selected",
          "detail disabled with selection reason");
}

void test_context_status_line() {
    UiContext ctx;
    ctx.provider.provider = "openai-codex";
    ctx.provider.model = "gpt-5.5";
    ctx.run.lifecycle = RunLifecycle::RunningTools;
    ctx.run.pendingOps = 2;
    ctx.session.id = "abcdef123456";
    ctx.session.staleReplay = true;
    std::string status = globalStatusLine(ctx);
    check(status.find("openai-codex/gpt-5.5") != std::string::npos,
          "status includes provider/model");
    check(status.find("running tools") != std::string::npos,
          "status includes lifecycle");
    check(status.find("pending 2") != std::string::npos,
          "status includes pending ops");
    check(status.find("stale replay") != std::string::npos,
          "status includes stale replay marker");
}

void test_navigation_stack_agent_drill() {
    NavigationState nav;
    replaceRoot(nav, makeEntry(AppView::AgentHistory, FocusPane::Composer));
    check(atRootAgent(nav), "navigation root starts at root agent");
    check(pushAgentChild(nav, "reader", "r1"), "push child agent succeeds");
    check(!atRootAgent(nav), "navigation child is not root");
    check(currentBreadcrumb(nav, "root") == "root / reader", "breadcrumb includes child agent");
    check(pushAgentChild(nav, "grep"), "push sub-child agent succeeds");
    check(currentBreadcrumb(nav, "root") == "root / reader / grep", "breadcrumb includes sub-child");
    check(popView(nav), "pop sub-child succeeds");
    check(currentBreadcrumb(nav, "root") == "root / reader", "pop restores parent breadcrumb");
    check(popView(nav), "pop child succeeds");
    check(atRootAgent(nav), "pop restores root agent");
    check(!popView(nav), "cannot pop root view");
}

int countRows(const ShellModel& model, TimelineKind kind) {
    int count = 0;
    for (const auto& row : model.rootRows)
        if (row.kind == kind) ++count;
    return count;
}

void test_workflow_run_hub_transitions() {
    using namespace model;
    WorkflowRunHub hub;

    workflows::WorkflowManifest mf;
    mf.name = "smoke-test";
    mf.version = "2.0";
    mf.summary = "unit";
    workflows::WorkflowStep a;
    a.id = "announce";
    a.type = "emit";
    a.emitEvent = "smoke.started";
    workflows::WorkflowStep b;
    b.id = "done";
    b.type = "return";
    mf.steps = {a, b};

    hub.prepare(mf, "manifests/workflows/smoke-test.yml");
    auto s0 = hub.snapshot();
    check(s0.status == RunStatus::Starting && s0.steps.size() == 2 && s0.live,
          "workflow hub prepare seeds steps and starting status");

    workflows::StepProgress enter;
    enter.id = "announce";
    enter.type = "emit";
    enter.phase = workflows::StepProgress::Phase::Enter;
    hub.onProgress(enter);
    hub.markRunning();
    auto s1 = hub.snapshot();
    check(s1.status == RunStatus::Running && s1.currentIdx == 0 &&
              s1.steps[0].status == StepStatus::Running,
          "workflow hub enter marks running step");

    workflows::StepProgress ok;
    ok.id = "announce";
    ok.type = "emit";
    ok.phase = workflows::StepProgress::Phase::Ok;
    ok.elapsedMs = 1.5;
    hub.onProgress(ok);

    workflows::WorkflowResult result;
    result.success = true;
    result.workflowName = "smoke-test";
    result.elapsedMs = 12.0;
    result.stepMetrics.push_back({"announce", "emit", 1.5, true});
    result.stepMetrics.push_back({"done", "return", 0.2, true});
    hub.finish(result);
    auto s2 = hub.snapshot();
    check(s2.status == RunStatus::Succeeded && !s2.live && s2.steps[0].status == StepStatus::Ok,
          "workflow hub finish succeeds and clears live");

    Json::Value schema;
    schema["type"] = "object";
    schema["properties"]["target"]["type"] = "string";
    schema["properties"]["environment"]["type"] = "string";
    schema["properties"]["environment"]["default"] = "staging";
    auto input = defaultInputFromSchema(schema);
    check(input.isMember("target") && input["environment"].asString() == "staging",
          "defaultInputFromSchema fills required defaults");

    check(workflowRunnablePath("manifests/workflows/smoke-test.yml", "smoke-test"),
          "smoke-test is runnable");
    check(!workflowRunnablePath("manifests/workflows/workflow_spec.yml", "workflow_spec"),
          "workflow_spec is not runnable");
}

void test_dashboard_model() {
    model::DashboardState dashboard;
    // 7 pills: Home · Sessions · Manifests · Tools · Relics · Workflows · Settings
    const int last = model::DashboardState::sectionCount - 1;
    for (int i = 0; i < last; ++i) dashboard.moveNavigation(1);
    check(dashboard.section == model::DashboardSection::Settings && dashboard.navigationIndex == last,
          "dashboard navigation reaches Settings");
    dashboard.moveNavigation(1);
    check(dashboard.section == model::DashboardSection::Home && dashboard.navigationIndex == 0,
          "dashboard navigation wraps Home");
    dashboard.moveNavigation(-1);
    check(dashboard.section == model::DashboardSection::Settings && dashboard.navigationIndex == last,
          "dashboard navigation wraps backward to Settings");

    session::SessionManager::SessionInfo first;
    first.id = "first";
    session::SessionManager::SessionInfo second;
    second.id = "second";
    dashboard.sessions = {first, second};
    dashboard.moveSession(1);
    check(dashboard.selectedSession() && dashboard.selectedSession()->id == "second",
          "dashboard session selection moves to next record");
    dashboard.moveSession(20);
    check(dashboard.sessionIndex == 1, "dashboard session selection clamps safely");
}

void test_dashboard_session_controller() {
    std::string base = "/tmp/mk3-dashboard-sessions-" + std::to_string(::getpid());
    session::SessionManager sessions(base);
    Session existing = sessions.create("resume-me", "coder", "gpt-5.5", "openai-codex");
    existing.records.push_back({SessionRecord::USER, "hello", "", ""});
    existing.records.push_back({SessionRecord::AGENT, "hi", "", ""});
    sessions.save(existing);

    model::DashboardState dashboard;
    // Explicit temp store — global scope so foreign ids are visible
    // (per-project filtering is the default-store view policy).
    dashboard.refreshSessions(sessions, true);
    bool loadedAgent = false;
    auto resumed = model::resumeDashboardSession(
        dashboard, sessions, [&](const std::string& id) { loadedAgent = id == "resume-me"; });
    check(resumed.ok && resumed.sessionId == "resume-me" && resumed.records.size() == 2 && loadedAgent,
          "dashboard controller resumes selected structured session");

    bool clearedAgent = false;
    auto created = model::createDashboardSession(
        dashboard, sessions, "coder", "gpt-5.5", "openai-codex",
        [&] { clearedAgent = true; }, "new-session");
    // Vet-fix: vet-fix: end-of-life tests must follow the new semantics —
    // createDashboardSession now ARMS the session id (sets agent.sessionId)
    // but does NOT write the file. The file is persisted by Agent::saveSession
    // only when the run produced real content. So the file should NOT exist
    // yet, unless something else has gated-persisted.
    check(created.ok && created.sessionId == "new-session" && clearedAgent &&
              !sessions.exists("new-session"),
          "dashboard controller arms session id without writing empty file");
    // If we now persist through the manager, the file appears.
    sessions.save(sessions.load("new-session"));
    check(sessions.exists("new-session"),
          "dashboard controller lets the manager persist on demand");
    std::filesystem::remove_all(base);
}

void test_chat_persistence() {
    std::string path = "/tmp/mk3-chat-history-test-" + std::to_string(::getpid());
    std::vector<std::string> saved = {"first", "second", "second", "third"};
    check(chat::savePromptHistory(saved, path, 3), "prompt history saves atomically");
    auto loaded = chat::loadPromptHistory(path);
    check(loaded.size() == 2 && loaded[0] == "second" && loaded[1] == "third",
          "prompt history loads bounded deduplicated entries");
    std::filesystem::remove(path);

    std::vector<SessionRecord> records;
    records.push_back({SessionRecord::USER, "hello", "", ""});
    records.push_back({SessionRecord::AGENT, "hi", "", ""});
    records.push_back({SessionRecord::TOOL_CALL, "read x", "", ""});
    records.push_back({SessionRecord::TOOL_RESULT, "x contents", "", ""});
    ShellModel model;
    model.loadSessionRecords(records);
    check(model.rootRows.size() == 4, "session replay loads every structured record");
    check(model.rootRows[0].kind == TimelineKind::User &&
              model.rootRows[1].kind == TimelineKind::Response &&
              model.rootRows[2].kind == TimelineKind::Action &&
              model.rootRows[3].kind == TimelineKind::Result,
          "session replay maps record roles to chat rows");
}


void test_slash_tab_completion_lcp_and_stop_command() {
    auto matches = chat::completeChatCommand("/st");
    bool hasStop = false;
    for (const auto& m : matches)
        if (m == "/stop") hasStop = true;
    check(hasStop, "tab candidates include /stop");
    auto lcp = chat::commonPrefixOf(matches);
    check(lcp.rfind("/st", 0) == 0, "common prefix keeps /st stem");

    auto se = chat::completeChatCommand("/se");
    bool hasSessions = false;
    for (const auto& m : se)
        if (m == "/sessions") hasSessions = true;
    check(hasSessions, "tab candidates include /sessions for /se prefix");

    auto only = chat::completeChatCommand("/truncate");
    check(only.size() == 1 && only[0] == "/truncate", "/truncate is completable");

    auto stop = chat::executeChatCommand("/stop");
    check(stop.handled && stop.stopLoop, "/stop sets stopLoop");
    auto cancel = chat::executeChatCommand("/cancel");
    check(cancel.handled && cancel.stopLoop, "/cancel aliases stopLoop");
}

// ── Composer command batching ──────────────────────────────────────────
// Hermetic: build synthetic skills/prompts, then verify parse/compose.
static chat::DynamicChatCommand makeDynamic(const std::string& name,
                                            const std::string& kind,
                                            const std::string& body) {
    chat::DynamicChatCommand c;
    c.name = "/" + name + ":" + kind;
    c.kind = kind;
    c.body = body;
    return c;
}

void test_composer_batch_parse_and_compose() {
    using chat::DynamicChatCommand;
    std::vector<DynamicChatCommand> cmds;
    cmds.push_back(makeDynamic("manifest", "skill",
                               "MANIFEST-SKILL-BODY\n$ARGUMENTS"));
    cmds.push_back(makeDynamic("review", "prompt",
                               "REVIEW-PROMPT-BODY\nargs={{args}}"));

    // User writes /kind:name (e.g. /skill:manifest); canonical is /name:kind.
    auto b1 = chat::parseComposerBatch("/skill:manifest /prompt:review audit this repo", cmds);
    check(b1.anyCommand && b1.commands.size() == 2, "batch sees 2 tags");
    check(b1.allResolved, "both tags resolved across /kind:name orientation");
    check(b1.commands[0].kind == "skill" && b1.commands[1].kind == "prompt",
          "tag kinds resolved from canonical");
    check(b1.commands[0].name == "/manifest:skill" &&
          b1.commands[1].name == "/review:prompt",
          "tags canonicalized to /name:kind");
    // Trailing text after the last tag (multi-tag) becomes the plain ask.
    check(b1.plainText == "audit this repo", "trailing text threads as plain ask");
    check(b1.commands[0].expanded.find("MANIFEST-SKILL-BODY") != std::string::npos,
          "skill body expanded");
    check(b1.commands[1].expanded.find("REVIEW-PROMPT-BODY") != std::string::npos,
          "prompt body expanded");

    // A single tag keeps trailing text as its $ARGUMENTS (backward compatible).
    auto b2 = chat::parseComposerBatch("/skill:manifest  \n focus the engine", cmds);
    check(b2.commands.size() == 1, "single skill tag");
    check(b2.plainText.empty(), "single tag: no plain ask split");
    check(b2.commands[0].expanded.find("focus the engine") != std::string::npos,
          "single-tag trailing text is its args");

    // Unknown tag flagged unresolved but doesn't crash.
    auto b3 = chat::parseComposerBatch("/skill:nope hi", cmds);
    check(b3.anyCommand && b3.commands.size() == 1, "unknown tag still a command");
    check(!b3.allResolved, "unknown tag marks allResolved false");
    check(!b3.commands[0].resolved, "the unknown command is unresolved");

    // composeBatchPrompt joins bodies in order, then the plain ask.
    std::string prompt = chat::composeBatchPrompt(b1);
    check(prompt.find("MANIFEST-SKILL-BODY") < prompt.find("REVIEW-PROMPT-BODY"),
          "compose keeps command order");
    check(prompt.find("audit this repo") != std::string::npos,
          "compose includes the plain ask");
    check(prompt.find("REVIEW-PROMPT-BODY") < prompt.find("audit this repo"),
          "plain ask composed after command bodies");

    // Empty input → empty batch.
    auto b0 = chat::parseComposerBatch("   ", cmds);
    check(b0.empty(), "whitespace-only input yields empty batch");

    // Canonical /name:kind also parses.
    auto b4 = chat::parseComposerBatch("/manifest:skill /review:prompt go", cmds);
    check(b4.commands.size() == 2 && b4.allResolved,
          "canonical /name:kind parses too");
    check(b4.plainText == "go", "canonical form threads plain ask");
}

void test_chat_ask_dialog_channel() {
    Json::Value params;
    params["chainTitle"] = "Choose target";
    Json::Value card;
    card["id"] = "target";
    card["type"] = "choice";
    card["title"] = "Target";
    card["options"].append("reader");
    card["options"].append("tester");
    params["cards"].append(card);

    AgentBridge bridge;
    auto future = std::async(std::launch::async, [&] { return bridge.requestAsk(params); });
    for (int i = 0; i < 100 && !bridge.askPending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto events = bridge.drain();
    check(events.size() == 1 && events[0].kind == UiEventKind::AskDialog,
          "ask channel publishes dialog event");

    ShellModel model;
    model.apply(events[0]);
    check(model.askActive && model.askDialog.current() &&
              model.askDialog.current()->type == "choice",
          "ask dialog event activates parsed card state");
    chat::advanceDialog(model.askDialog, "reader");
    bridge.completeAsk(model.askDialog.results);
    Json::Value result = future.get();
    check(result["success"].asBool() && result["results"]["target"].asString() == "reader",
          "ask channel returns card result to worker");

    Json::Value numberParams;
    Json::Value numberCard;
    numberCard["id"] = "count";
    numberCard["type"] = "number";
    numberCard["numberMin"] = 2;
    numberCard["numberMax"] = 4;
    numberParams["cards"].append(numberCard);
    auto numberState = chat::parseDialogState(numberParams);
    check(!chat::handleDialogLine(numberState, "1") && !numberState.error.empty(),
          "ask number card enforces lower bound");
    check(chat::handleDialogLine(numberState, "3") && numberState.results["count"].asInt() == 3,
          "ask number card accepts valid value");

    Json::Value confirmParams;
    Json::Value confirmCard;
    confirmCard["id"] = "danger";
    confirmCard["type"] = "type_confirm";
    confirmCard["confirmWord"] = "DELETE";
    confirmParams["cards"].append(confirmCard);
    auto confirmState = chat::parseDialogState(confirmParams);
    check(!chat::handleDialogLine(confirmState, "delete"), "type-confirm rejects wrong case/value");
    check(chat::handleDialogLine(confirmState, "DELETE"), "type-confirm accepts exact word");

    // Notes-only dialog: drain/settleAsk must complete the bridge without keys.
    Json::Value notesParams;
    Json::Value noteCard;
    noteCard["id"] = "n1";
    noteCard["type"] = "note";
    noteCard["title"] = "FYI";
    notesParams["cards"].append(noteCard);
    AgentBridge notesBridge;
    auto notesFuture =
        std::async(std::launch::async, [&] { return notesBridge.requestAsk(notesParams); });
    for (int i = 0; i < 100 && !notesBridge.askPending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ShellModel notesModel;
    notesModel.drain(notesBridge);
    Json::Value notesResult = notesFuture.get();
    check(notesResult["success"].asBool() && !notesBridge.askPending(),
          "model drain settleAsk completes notes-only ask_tool");
}

void test_chat_commands() {
    auto help = chat::executeChatCommand("/help");
    check(help.handled && help.title == "commands" && help.lines.size() >= 6,
          "chat help command renders command catalog");

    chat::ChatCommandContext ctx;
    ctx.manifestPath = "manifests/agents/coder/agent.yml";
    ctx.toolCount = 3;
    ctx.subAgentCount = 2;
    auto manifests = chat::executeChatCommand("/manifests", ctx);
    check(manifests.handled && manifests.title == "active surface",
          "chat manifests command is handled locally");
    bool hasTools = false;
    for (const auto& line : manifests.lines)
        if (line == "tools     3") hasTools = true;
    check(hasTools, "chat manifests command reports active counts");

    check(chat::executeChatCommand("/clear").clearTranscript, "chat clear command classified");
    check(chat::executeChatCommand("/thoughts").toggleThoughts, "chat thoughts command classified");
    check(chat::executeChatCommand("/raw").toggleRaw, "chat raw command classified");
    check(chat::executeChatCommand("/theme").toggleTheme, "chat theme command classified");
    auto neonTheme = chat::executeChatCommand("/theme neon");
    check(neonTheme.toggleTheme && neonTheme.themeName == "neon", "chat theme accepts explicit neon selection");
    auto badTheme = chat::executeChatCommand("/theme radioactive");
    check(!badTheme.toggleTheme && !badTheme.lines.empty(), "chat theme rejects unknown palette");
    chat::ChatCommandContext dev;
    dev.devMode = true;
    check(chat::executeChatCommand("/prompts", dev).showPrompts, "chat prompts command classified");
    check(chat::executeChatCommand("/dump-prompt", dev).dumpPrompts, "chat dump-prompt command classified");
    check(chat::executeChatCommand("/cp-all").copyAll, "chat copy-all command classified");
    check(chat::executeChatCommand("/cp-raw", dev).copyRaw, "chat copy-raw command classified");
    check(chat::executeChatCommand("/quit").quit, "chat quit command classified");
    auto dynamic = chat::discoverDynamicChatCommands();
    check(!dynamic.empty(), "dynamic prompt/skill catalog is discovered");
    auto debugger = chat::completeChatCommand("/debug");
    bool hasDebugger = false;
    for (const auto& name : debugger)
        if (name.rfind("/debugger", 0) == 0) hasDebugger = true;
    check(hasDebugger, "dynamic command participates in completion");
    auto expanded = chat::executeChatCommand("/debugger parser crash");
    check(expanded.handled && !expanded.composerReplacement.empty() &&
              expanded.composerReplacement.find("parser crash") != std::string::npos,
          "dynamic command expands arguments into composer text");

    auto unknown = chat::executeChatCommand("/nope");
    check(unknown.handled && unknown.title == "unknown command",
          "unknown slash command is not sent to model");
}

void test_chat_prompt_history() {
    ShellModel model;
    model.composer.value = "first prompt";
    model.composer.cursor = static_cast<int>(model.composer.value.size());
    check(model.submitComposer(), "first prompt submits");
    model.composer.value = "second prompt";
    model.composer.cursor = static_cast<int>(model.composer.value.size());
    check(model.submitComposer(), "second prompt submits");

    model.composer.value = "draft";
    model.composer.cursor = 5;
    check(model.historyPrevious() && model.composer.value == "second prompt",
          "history up recalls latest prompt");
    check(model.historyPrevious() && model.composer.value == "first prompt",
          "history up recalls older prompt");
    check(model.historyNext() && model.composer.value == "second prompt",
          "history down moves toward latest prompt");
    check(model.historyNext() && model.composer.value == "draft",
          "history down restores draft");
}

void test_chat_protocol_reducer_updates_in_place() {
    ShellModel model;
    model.apply(UiEvent::status("agent running"));
    check(model.rootRows.empty(), "status stays out of transcript");

    model.apply(UiEvent::token("raw bytes"));
    check(countRows(model, TimelineKind::Stream) == 0, "FULL mode stream stays out of transcript");

    ProtocolEvent action = actionEvent("agent", "reader", "ping_reader", "ping");
    model.apply(UiEvent::protocolEvent(action, 0));
    model.apply(UiEvent::protocolEvent(action, 0));
    check(countRows(model, TimelineKind::Action) == 1 && model.actionCount == 1,
          "duplicate action update does not append");
    check(model.pendingOps == 1, "action remains pending");

    ProtocolEvent progress = resultEvent("ping_reader", true, "reader is running…", "reader");
    progress.result.elapsedMs = 0;
    model.apply(UiEvent::protocolEvent(progress, 1));
    check(countRows(model, TimelineKind::Result) == 0 && model.resultCount == 0,
          "progress placeholder is not a completed result row");

    ProtocolEvent result = resultEvent("ping_reader", true, "reader available", "reader");
    model.apply(UiEvent::protocolEvent(result, 1));
    check(countRows(model, TimelineKind::Result) == 1 && model.resultCount == 1,
          "final result replaces placeholder slot");
    check(model.pendingOps == 0, "final result clears pending action");

    model.apply(UiEvent::protocolEvent(responseEvent("I"), 2));
    model.apply(UiEvent::protocolEvent(responseEvent("I pinged reader."), 2));
    check(countRows(model, TimelineKind::Response) == 1,
          "partial response updates one transcript row");
    check(model.rootRows.back().body == "I pinged reader.", "response row contains latest text");
    model.rebuildViews();
    bool hasAgentLabel = false;
    bool hasCortexLabel = false;
    for (const auto& line : model.transcriptView.lines) {
        if (line.find("AGENT  reader") != std::string::npos) hasAgentLabel = true;
        if (line.find("CORTEX") != std::string::npos) hasCortexLabel = true;
    }
    check(hasAgentLabel && hasCortexLabel, "transcript uses semantic agent and assistant labels");

    UiEvent done;
    done.kind = UiEventKind::TurnDone;
    done.text = "I pinged reader.";
    model.apply(done);
    check(countRows(model, TimelineKind::Final) == 0,
          "turn done does not duplicate an existing response");

    model.apply(UiEvent::status("agent running"));
    check(model.actionCount == 0 && model.resultCount == 0 && model.pendingOps == 0,
          "new turn resets per-turn action metrics");
    check(model.tokenBytes == 0 && model.raw.empty(), "new turn resets stream metrics");

    UiEvent cancelled;
    cancelled.kind = UiEventKind::TurnDone;
    cancelled.text = "[cancelled]";
    model.apply(cancelled);
    check(model.status == "cancelled" && !model.running, "cancelled turn has truthful terminal state");
}

void test_chat_turn_start_timestamp_lifecycle() {
    // turnStartMs is captured on the false->true running transition (status
    // event) and cleared on turn end (TurnDone, Error) so the dashboard live
    // metrics line shows a fresh elapsed each turn and "—" when idle.
    ShellModel model;
    check(model.turnStartMs == 0, "turn start timestamp is zero when idle");

    model.apply(UiEvent::status("agent running"));
    check(model.running && model.turnStartMs > 0,
          "turn start timestamp is captured when a turn starts");

    int64_t firstStart = model.turnStartMs;
    UiEvent done;
    done.kind = UiEventKind::TurnDone;
    done.text = "ok";
    model.apply(done);
    check(!model.running && model.turnStartMs == 0,
          "turn start timestamp is cleared when the turn completes");

    model.apply(UiEvent::status("agent running"));
    check(model.turnStartMs > 0 && model.turnStartMs >= firstStart,
          "subsequent turn captures a fresh (non-decreasing) timestamp");
}

void test_chat_last_turn_summary_lifecycle() {
    // lastTurnElapsedMs is captured at turn end (TurnDone/Error) and persists
    // so the dashboard "last" line can show the previous turn's outcome and
    // duration after running flips back to false. The stored timestamps use
    // millisecond resolution, so back-to-back apply() calls within the same
    // millisecond legitimately produce a 0-ms difference; the sleeps below
    // guarantee a measurable elapsed so the assertions are meaningful.
    ShellModel model;
    check(model.lastTurnElapsedMs == 0, "last turn elapsed is zero before any turn");

    model.apply(UiEvent::status("agent running"));
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    UiEvent done;
    done.kind = UiEventKind::TurnDone;
    done.text = "ok";
    model.apply(done);
    check(model.lastTurnElapsedMs >= 3,
          "last turn elapsed captures real elapsed time on TurnDone");
    check(!model.running && model.turnStartMs == 0,
          "turn end clears running and turnStartMs after capturing last");
    int64_t captured = model.lastTurnElapsedMs;

    model.apply(UiEvent::status("agent running"));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    done.text = "second";
    model.apply(done);
    check(model.lastTurnElapsedMs >= 5 && model.lastTurnElapsedMs >= captured,
          "subsequent turn updates last turn elapsed with fresh value");

    ShellModel err;
    err.apply(UiEvent::status("agent running"));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    UiEvent errEvt;
    errEvt.kind = UiEventKind::Error;
    errEvt.text = "boom";
    err.apply(errEvt);
    check(err.lastTurnElapsedMs >= 2 && !err.running,
          "error path captures last turn elapsed and clears running");
}

void test_chat_last_response_body() {
    // The dashboard preview line reads the most recent Response row's body.
    // The helper returns empty when no Response row exists, and the latest
    // body when one does. Action/result rows do not count.
    ShellModel model;
    check(model.lastResponseBody().empty(),
          "last response body is empty with no timeline rows");

    ProtocolEvent action = actionEvent("agent", "reader", "ping_reader", "ping");
    model.apply(UiEvent::protocolEvent(action, 0));
    ProtocolEvent result = resultEvent("ping_reader", true, "ok", "reader");
    model.apply(UiEvent::protocolEvent(result, 1));
    check(model.lastResponseBody().empty(),
          "last response body is empty when only action/result rows exist");

    model.apply(UiEvent::protocolEvent(responseEvent("streaming partial"), 2));
    check(model.lastResponseBody() == "streaming partial",
          "last response body returns the Response row body");

    // A later response event (updated text) replaces the body; the helper
    // returns the latest.
    model.apply(UiEvent::protocolEvent(responseEvent("streaming final answer"), 2));
    check(model.lastResponseBody() == "streaming final answer",
          "last response body returns the most recent response text");
}

void test_chat_thought_rows_visible_by_default() {
    // Thinking tokens (real test-time-compute and harness-time <thought>/
    // <think>/<thinking> aliases) must stream visibly into the transcript
    // by default. The operator can hide them with the 't' toggle later.
    // This pins: (a) showThoughts default = true, (b) a THOUGHT protocol
    // event produces a Thought timeline row (not skipped), (c) the row's
    // body carries the thought text so it renders in the chat.
    ShellModel model;
    check(model.showThoughts == true,
          "showThoughts defaults to true (thoughts stream by default)");

    // Apply a THOUGHT protocol event and assert the timeline keeps it.
    model.apply(UiEvent::protocolEvent(thoughtEvent("reasoning across the thinking tag"), 0));
    bool found = false;
    for (const auto& r : model.rootRows) {
        if (r.kind == TimelineKind::Thought &&
            r.body.find("reasoning across the thinking tag") != std::string::npos) {
            found = true;
            break;
        }
    }
    check(found,
          "thought protocol event produces a Thought timeline row visible by default");
}




void test_subagent_history_continues_across_prompts() {
    // Non-ephemeral sub-agent: two parent prompts must share one history.
    // Drilldown (rowsFromAgent) must show BOTH parent missions + responses.
    AgentConfig rootCfg;
    rootCfg.name = "coder";
    rootCfg.provider = "noop";
    rootCfg.model = "noop";
    auto root = std::make_unique<Agent>(rootCfg, std::make_shared<NestNoopProvider>());

    AgentConfig childCfg;
    childCfg.name = "reader";
    childCfg.provider = "noop";
    childCfg.model = "noop";
    // Second prompt returns different text so we can see both in history.
    auto child = std::make_unique<Agent>(
        childCfg, std::make_shared<NestNoopProvider>(
                      "<response final=\"true\">ack-one</response>"));
    Agent* childRaw = child.get();
    root->addSubAgent(std::move(child));

    setenv("HOME", "/tmp", 1);
    childRaw->prompt("ping one", [](const std::string&, bool) {}, "", false,
                     PromptSource::ParentAgent, "coder");
    // Swap provider response for second call by prompting again — NestNoopProvider
    // is fixed-string; history still records both Parent lines + Agent outputs.
    childRaw->prompt("list tools", [](const std::string&, bool) {}, "", false,
                     PromptSource::ParentAgent, "coder");

    check(childRaw->history().size() >= 2,
          "child history has both parent turns (and agent outputs when present)");
    bool sawPing = false, sawTools = false;
    for (const auto& h : childRaw->history()) {
        if (h.find("ping one") != std::string::npos) sawPing = true;
        if (h.find("list tools") != std::string::npos) sawTools = true;
    }
    check(sawPing, "history retains first parent mission");
    check(sawTools, "history retains second parent mission");

    auto rows = rowsFromAgent(childRaw);
    bool rowPing = false, rowTools = false;
    for (const auto& r : rows) {
        if (r.body.find("ping one") != std::string::npos) rowPing = true;
        if (r.body.find("list tools") != std::string::npos) rowTools = true;
    }
    check(rowPing, "rowsFromAgent shows first parent mission");
    check(rowTools, "rowsFromAgent shows second parent mission");
}

void test_chat_multi_turn_does_not_clobber() {
    // Second turn must APPEND protocol rows, never overwrite the first turn.
    // Regression for the "overrides from the top" contiguity bug: REPL may
    // pre-set running=true before status("agent running"), so the mapping
    // epoch must still reset on every running status.
    ShellModel model;
    model.agentName = "coder";

    // Turn 1 user + response
    model.pushRow({TimelineKind::User, "you", "first question", true});
    model.running = true;  // simulate REPL pre-set before status
    {
        UiEvent st;
        st.kind = UiEventKind::Status;
        st.text = "agent running";
        model.apply(st);
    }
    {
        UiEvent pe;
        pe.kind = UiEventKind::Protocol;
        pe.protocolIndex = 0;
        pe.protocol.kind = ProtocolEventKind::THOUGHT;
        pe.protocol.text = "thinking about first";
        model.apply(pe);
    }
    {
        UiEvent pe;
        pe.kind = UiEventKind::Protocol;
        pe.protocolIndex = 1;
        pe.protocol.kind = ProtocolEventKind::RESPONSE;
        pe.protocol.text = "answer one";
        model.apply(pe);
    }
    {
        UiEvent td;
        td.kind = UiEventKind::TurnDone;
        td.text = "answer one";
        model.apply(td);
    }

    // Turn 2 — running already true again (REPL pattern)
    model.pushRow({TimelineKind::User, "you", "second question", true});
    model.running = true;
    {
        UiEvent st;
        st.kind = UiEventKind::Status;
        st.text = "agent running";
        model.apply(st);
    }
    {
        UiEvent pe;
        pe.kind = UiEventKind::Protocol;
        pe.protocolIndex = 0;  // agent cleared protocolEvents — index restarts
        pe.protocol.kind = ProtocolEventKind::THOUGHT;
        pe.protocol.text = "thinking about second";
        model.apply(pe);
    }
    {
        UiEvent pe;
        pe.kind = UiEventKind::Protocol;
        pe.protocolIndex = 1;
        pe.protocol.kind = ProtocolEventKind::RESPONSE;
        pe.protocol.text = "answer two";
        model.apply(pe);
    }

    bool hasFirst = false, hasSecond = false, hasAns1 = false, hasAns2 = false;
    bool clobbered = false;
    for (const auto& l : model.transcriptView.lines) {
        if (l.find("first question") != std::string::npos) hasFirst = true;
        if (l.find("second question") != std::string::npos) hasSecond = true;
        if (l.find("answer one") != std::string::npos) hasAns1 = true;
        if (l.find("answer two") != std::string::npos) hasAns2 = true;
        if (l.find("thinking about first") != std::string::npos &&
            l.find("thinking about second") != std::string::npos)
            clobbered = true;  // same line somehow
    }
    // Count thought bodies
    int thoughtFirst = 0, thoughtSecond = 0;
    for (const auto& l : model.transcriptView.lines) {
        if (l.find("thinking about first") != std::string::npos) ++thoughtFirst;
        if (l.find("thinking about second") != std::string::npos) ++thoughtSecond;
    }
    check(hasFirst, "multi-turn keeps first user message");
    check(hasSecond, "multi-turn keeps second user message");
    check(hasAns1, "multi-turn keeps first answer");
    check(hasAns2, "multi-turn keeps second answer");
    check(thoughtFirst >= 1, "first turn thought still present");
    check(thoughtSecond >= 1, "second turn thought present");
    check(!clobbered, "thoughts not merged onto one line");
}

void test_chat_empty_thoughts_not_rendered() {
    ShellModel model;
    model.showThoughts = true;
    TimelineRow empty;
    empty.kind = TimelineKind::Thought;
    empty.title = "thought";
    empty.body = "   \n\t";
    TimelineRow real;
    real.kind = TimelineKind::Thought;
    real.title = "thought";
    real.body = "actual reasoning";
    model.rootRows.push_back(std::move(empty));
    model.rootRows.push_back(std::move(real));
    model.rebuildViews();
    int thoughtHeaders = 0;
    bool hasReal = false;
    for (const auto& l : model.transcriptView.lines) {
        if (l.find("THOUGHT") != std::string::npos) ++thoughtHeaders;
        if (l.find("actual reasoning") != std::string::npos) hasReal = true;
    }
    check(thoughtHeaders == 1, "empty/whitespace thought rows are not rendered");
    check(hasReal, "non-empty thought body still renders");
}

void test_chat_subagent_result_shows_final_no_auto_enter() {
    // Sub-agent RESULT in the parent transcript shows the child's final
    // response text as the body. No nested child blocks. No auto-enter —
    // ↳ Enter is manual only.
    AgentConfig rootCfg;
    rootCfg.name = "coder";
    rootCfg.provider = "noop";
    rootCfg.model = "noop";
    auto rootAgent = std::make_unique<Agent>(rootCfg, std::make_shared<NestNoopProvider>());

    AgentConfig childCfg;
    childCfg.name = "reader";
    childCfg.provider = "noop";
    childCfg.model = "noop";
    auto childAgent = std::make_unique<Agent>(childCfg,
        std::make_shared<NestNoopProvider>("<response final=\"true\">reader-ok</response>"));
    Agent* childRaw = childAgent.get();
    rootAgent->addSubAgent(std::move(childAgent));

    setenv("HOME", "/tmp", 1);
    childRaw->prompt("ping", [](const std::string&, bool) {}, "", true);
    check(childRaw->responseOutput().find("reader-ok") != std::string::npos,
          "child responseOutput holds final text");

    ShellModel model;
    model.agentName = "coder";
    model.setRootAgent(rootAgent.get());
    check(model.atRoot(), "starts at root — no auto-enter");

    // Simulate RESULT protocol event with empty/placeholder summary — the
    // model should fill body from the child's responseOutput.
    UiEvent ev;
    ev.kind = UiEventKind::Protocol;
    ev.protocolIndex = 0;
    ev.protocol.kind = ProtocolEventKind::RESULT;
    ev.protocol.result.id = "ping_reader";
    ev.protocol.result.ok = true;
    ev.protocol.result.summary = "reader";  // placeholder = action name
    ev.protocol.result.toolName = "reader";
    ev.protocol.result.elapsedMs = 12.0;
    model.apply(ev);

    check(model.atRoot(), "RESULT does not auto-enter child chat");

    bool headerFound = false;
    bool finalShown = false;
    for (const auto& l : model.transcriptView.lines) {
        if (l.find("✓ RESULT") != std::string::npos && l.find("reader") != std::string::npos)
            headerFound = true;
        if (l.find("┌") != std::string::npos && l.find("reader") != std::string::npos)
            finalShown = true;
        if (l.find("reader-ok") != std::string::npos)
            finalShown = true;  // done well paints final reply
    }
    check(headerFound, "RESULT header present in parent");
    check(finalShown, "RESULT well shows final reply (not empty act0)");

    // Manual drill still works.
    check(model.enterSubAgent("reader"), "manual enterSubAgent works");
    check(!model.atRoot(), "manual drill leaves root");
    check(model.goBack(), "goBack returns to parent");
    check(model.atRoot(), "back at root after goBack");
}

void test_sanitize_blocks_binary_and_cleans_response() {
    // Binary blob (ELF-ish NUL + high non-UTF8 density) must collapse.
    std::string bin;
    bin.push_back('\x7f');
    bin.append("ELF");
    for (int i = 0; i < 200; ++i) bin.push_back(static_cast<char>(i % 32));
    for (int i = 0; i < 200; ++i) bin.push_back(static_cast<char>(0x80 + (i % 40)));
    std::string cleaned = sanitizeForDisplay(bin);
    check(cleaned.find("binary/non-text") != std::string::npos,
          "sanitize collapses binary/non-text payload");
    check(cleaned.find("ELF") == std::string::npos || cleaned.find("binary") != std::string::npos,
          "sanitize does not paint raw ELF bytes as body");

    // Invalid UTF-8 sequence replaced, not passed through as garbage cells.
    std::string badUtf8 = "ok ";
    badUtf8.push_back(static_cast<char>(0xFF));
    badUtf8.push_back(static_cast<char>(0xFE));
    badUtf8.append(" end");
    std::string u = sanitizeForDisplay(badUtf8);
    check(u.find("ok") != std::string::npos && u.find("end") != std::string::npos,
          "sanitize keeps surrounding text around bad UTF-8");
    // Must not contain the raw 0xFF byte.
    bool hasRaw = false;
    for (unsigned char c : u)
        if (c == 0xFF || c == 0xFE) hasRaw = true;
    check(!hasRaw, "sanitize strips invalid high bytes");

    // Streaming upsert path sanitizes Response bodies (the live bug path).
    ShellModel model;
    ProtocolEvent pe;
    pe.kind = ProtocolEventKind::RESPONSE;
    pe.text = bin;
    AgentBridge bridge;
    bridge.publish(UiEvent::protocolEvent(std::move(pe), 0));
    model.drain(bridge);
    bool blocked = false;
    for (const auto& r : model.rootRows) {
        if (r.kind == TimelineKind::Response || r.kind == TimelineKind::Final) {
            blocked = r.body.find("binary/non-text") != std::string::npos ||
                      r.body.find("not shown") != std::string::npos;
            // Also accept if body is short marker-like and has no NULs.
            if (!blocked) {
                bool nul = false;
                for (unsigned char c : r.body)
                    if (c == 0) nul = true;
                blocked = !nul && r.body.size() < bin.size();
            }
        }
    }
    check(blocked, "upsertProtocolRow sanitizes binary Response body");
}
}  // namespace

int main() {
    std::cout << "UI model tests\n";
    test_tool_action_result();
    test_agent_drillability();
    test_missing_child_not_drillable();
    test_thought_toggle_and_path_prefix();
    test_generic_path_exists();
    test_command_inventory_agent_history();
    test_command_inventory_disabled_reasons();
    test_context_status_line();
    test_navigation_stack_agent_drill();
    test_workflow_run_hub_transitions();
    test_dashboard_model();
    test_dashboard_session_controller();
    test_chat_persistence();
    test_slash_tab_completion_lcp_and_stop_command();
    test_composer_batch_parse_and_compose();
    test_chat_ask_dialog_channel();
    test_chat_commands();
    test_chat_prompt_history();
    test_chat_protocol_reducer_updates_in_place();
    test_chat_turn_start_timestamp_lifecycle();
    test_chat_last_turn_summary_lifecycle();
    test_chat_last_response_body();
    test_chat_thought_rows_visible_by_default();
    test_subagent_history_continues_across_prompts();
    test_chat_multi_turn_does_not_clobber();
    test_chat_empty_thoughts_not_rendered();
    test_chat_subagent_result_shows_final_no_auto_enter();
    test_sanitize_blocks_binary_and_cleans_response();
    std::cout << "\n" << (failures == 0 ? "all passed" : "failures: " + std::to_string(failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
