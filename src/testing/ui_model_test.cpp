#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "src/ui/model/adapters/agent_tree.hpp"
#include "src/ui/model/adapters/protocol_to_timeline.hpp"

using namespace cortex::mk3;
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
}  // namespace

int main() {
    std::cout << "UI model tests\n";
    test_tool_action_result();
    test_agent_drillability();
    test_missing_child_not_drillable();
    test_thought_toggle_and_path_prefix();
    test_generic_path_exists();
    std::cout << "\n" << (failures == 0 ? "all passed" : "failures: " + std::to_string(failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
