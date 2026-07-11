#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "src/ui/model/adapters/agent_tree.hpp"
#include "src/ui/model/adapters/protocol_to_timeline.hpp"
#include "src/ui/model/command_model.hpp"
#include "src/ui/model/inkcell_app_model.hpp"
#include "src/ui/model/navigation_model.hpp"

using namespace cortex::mk3;
using namespace cortex::mk3::ui;
using namespace cortex::mk3::ui::model;

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

    UiEvent done;
    done.kind = UiEventKind::TurnDone;
    done.text = "I pinged reader.";
    model.apply(done);
    check(countRows(model, TimelineKind::Final) == 0,
          "turn done does not duplicate an existing response");
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
    test_chat_protocol_reducer_updates_in_place();
    std::cout << "\n" << (failures == 0 ? "all passed" : "failures: " + std::to_string(failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
