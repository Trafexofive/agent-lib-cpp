#pragma once
// Pure UI/domain model for Cortex MK3 timeline blocks.
// No rendering, no terminal, no inkcell dependency.

#include <map>
#include <string>
#include <vector>

namespace cortex::mk3::ui::model {

enum class BlockKind {
    UserPrompt,
    Thought,
    Action,
    Result,
    Response,
    Final,
    Status,
    Error,
};

enum class BlockStatus {
    Idle,
    Pending,
    Ok,
    Error,
    Partial,
};

enum class ActionClass {
    Safe,
    Destructive,
    Unknown,
};

struct RelatedTarget {
    std::vector<std::string> agentPath;
    std::string entityId;
    std::string label;
    bool available = false;
};

struct TimelineBlock {
    std::string stableId;
    BlockKind kind = BlockKind::Status;
    BlockStatus status = BlockStatus::Idle;
    std::string title;
    std::string summary;
    std::vector<std::string> tags;

    // Protocol identity.
    std::string actionId;
    std::string actionType;
    std::string actionName;

    // Drill/detail semantics.
    bool drillable = false;
    bool hasDetail = false;
    RelatedTarget related;

    // Optional raw payload retained for details/copy/export.
    std::string rawBody;
    std::map<std::string, std::string> metadata;
};

struct AgentPath {
    std::vector<std::string> parts;

    bool root() const { return parts.empty(); }

    std::string label(const std::string& rootName = "root") const {
        std::string out = rootName.empty() ? "root" : rootName;
        for (const auto& part : parts) out += " / " + part;
        return out;
    }

    AgentPath child(const std::string& name) const {
        AgentPath next = *this;
        next.parts.push_back(name);
        return next;
    }
};

struct AgentRunView {
    std::string agentName = "root";
    AgentPath path;
    std::vector<TimelineBlock> blocks;
    int selectedIndex = 0;
    bool browseOnly = false;
};

inline const char* blockKindName(BlockKind kind) {
    switch (kind) {
        case BlockKind::UserPrompt: return "user";
        case BlockKind::Thought: return "thought";
        case BlockKind::Action: return "action";
        case BlockKind::Result: return "result";
        case BlockKind::Response: return "response";
        case BlockKind::Final: return "final";
        case BlockKind::Status: return "status";
        case BlockKind::Error: return "error";
    }
    return "status";
}

inline const char* blockStatusName(BlockStatus status) {
    switch (status) {
        case BlockStatus::Idle: return "idle";
        case BlockStatus::Pending: return "pending";
        case BlockStatus::Ok: return "ok";
        case BlockStatus::Error: return "error";
        case BlockStatus::Partial: return "partial";
    }
    return "idle";
}

}  // namespace cortex::mk3::ui::model
