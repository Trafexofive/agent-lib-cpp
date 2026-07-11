#pragma once
// Command inventory model: one source for palette/help/footer availability.

#include <string>
#include <vector>

#include "src/ui/model/app_context.hpp"
#include "src/ui/model/timeline_model.hpp"

namespace cortex::mk3::ui::model {

enum class CommandGroup {
    Run,
    Navigate,
    Inspect,
    CopyExport,
    View,
    Context,
    Danger,
    Help,
};

struct KeyHint {
    std::string key;
    std::string label;
};

struct CommandSpec {
    std::string id;
    std::string label;
    CommandGroup group = CommandGroup::Navigate;
    ActionClass actionClass = ActionClass::Safe;
    bool enabled = true;
    std::string disabledReason;
    bool previewRequired = false;
    std::vector<KeyHint> keys;
};

inline const char* commandGroupName(CommandGroup group) {
    switch (group) {
        case CommandGroup::Run: return "Run";
        case CommandGroup::Navigate: return "Navigate";
        case CommandGroup::Inspect: return "Inspect";
        case CommandGroup::CopyExport: return "Copy / Export";
        case CommandGroup::View: return "View";
        case CommandGroup::Context: return "Context";
        case CommandGroup::Danger: return "Danger";
        case CommandGroup::Help: return "Help";
    }
    return "Navigate";
}

inline CommandSpec command(std::string id, std::string label, CommandGroup group,
                           ActionClass actionClass = ActionClass::Safe,
                           std::vector<KeyHint> keys = {}) {
    CommandSpec c;
    c.id = std::move(id);
    c.label = std::move(label);
    c.group = group;
    c.actionClass = actionClass;
    c.keys = std::move(keys);
    return c;
}

inline void disable(CommandSpec& c, std::string reason) {
    c.enabled = false;
    c.disabledReason = std::move(reason);
}

inline std::vector<CommandSpec> commandsForContext(const UiContext& ctx) {
    std::vector<CommandSpec> out;

    out.push_back(command("help.open", "Open help", CommandGroup::Help, ActionClass::Safe, {{"?", "help"}}));
    out.push_back(command("palette.open", "Open command palette", CommandGroup::Help, ActionClass::Safe,
                          {{":", "actions"}, {"Ctrl-K", "actions"}}));

    if (ctx.run.cancelable || runActive(ctx.run.lifecycle)) {
        out.push_back(command("run.cancel", "Cancel current run", CommandGroup::Run, ActionClass::Safe,
                              {{"Ctrl-C", "cancel"}}));
    } else {
        out.push_back(command("app.quit", "Quit", CommandGroup::Navigate, ActionClass::Safe, {{"q", "quit"}}));
    }

    switch (ctx.view) {
        case AppView::Welcome: {
            out.push_back(command("nav.agent_history", "Open Agent / History", CommandGroup::Navigate,
                                  ActionClass::Safe, {{"1", "agent"}, {"Enter", "open"}}));
            out.push_back(command("nav.provider_picker", "Pick provider/model", CommandGroup::Context,
                                  ActionClass::Safe, {{"p", "provider"}}));
            out.push_back(command("nav.session_browser", "Browse sessions", CommandGroup::Context,
                                  ActionClass::Safe, {{"s", "sessions"}}));
            out.push_back(command("nav.manifest_surface", "Browse manifests/agents", CommandGroup::Context,
                                  ActionClass::Safe, {{"m", "manifests"}}));
            break;
        }
        case AppView::AgentHistory: {
            CommandSpec send = command("run.send_prompt", "Send prompt", CommandGroup::Run,
                                       ActionClass::Safe, {{"Enter", "send"}});
            if (ctx.focus != FocusPane::Composer) disable(send, "composer not focused");
            else if (runActive(ctx.run.lifecycle)) disable(send, "run already active");
            else if (!ctx.provider.configured) disable(send, "provider not configured");
            out.push_back(send);

            if (ctx.focus == FocusPane::Composer) {
                out.push_back(command("focus.history", "Focus history", CommandGroup::Navigate,
                                      ActionClass::Safe, {{"Esc", "history"}}));
            } else {
                out.push_back(command("focus.composer", "Focus composer", CommandGroup::Navigate,
                                      ActionClass::Safe, {{"i", "composer"}}));
                out.push_back(command("selection.prev", "Select previous block", CommandGroup::Navigate,
                                      ActionClass::Safe, {{"k", "up"}, {"↑", "up"}}));
                out.push_back(command("selection.next", "Select next block", CommandGroup::Navigate,
                                      ActionClass::Safe, {{"j", "down"}, {"↓", "down"}}));
            }

            CommandSpec detail = command("detail.open", "Open selected block detail", CommandGroup::Inspect,
                                         ActionClass::Safe, {{"Enter", "detail"}});
            if (!ctx.hasSelection) disable(detail, "no block selected");
            else if (!ctx.selectedHasDetail) disable(detail, "selected block has no detail");
            out.push_back(detail);

            CommandSpec drill = command("agent.drill", "Drill into sub-agent", CommandGroup::Navigate,
                                        ActionClass::Safe, {{"Enter", "drill"}});
            if (!ctx.selectedDrillable) disable(drill, "selected block has no sub-agent");
            out.push_back(drill);

            if (!ctx.atRootAgent) {
                out.push_back(command("agent.back", "Back to parent agent", CommandGroup::Navigate,
                                      ActionClass::Safe, {{"Backspace", "back"}, {"Esc", "back"}}));
            }

            out.push_back(command("view.toggle_thoughts", "Toggle thoughts", CommandGroup::View,
                                  ActionClass::Safe, {{"t", "thoughts"}}));
            out.push_back(command("view.toggle_raw", "Toggle raw", CommandGroup::View,
                                  ActionClass::Safe, {{"r", "raw"}}));

            CommandSpec copy = command("copy.selected", "Copy selected block", CommandGroup::CopyExport,
                                       ActionClass::Safe, {{"c", "copy"}});
            if (!ctx.hasSelection) disable(copy, "no block selected");
            out.push_back(copy);
            break;
        }
        case AppView::EventDetail: {
            out.push_back(command("nav.back", "Back to history", CommandGroup::Navigate,
                                  ActionClass::Safe, {{"Esc", "back"}, {"Backspace", "back"}}));
            out.push_back(command("copy.detail", "Copy detail", CommandGroup::CopyExport,
                                  ActionClass::Safe, {{"c", "copy"}}));
            if (ctx.selectedError) {
                CommandSpec retry = command("run.retry", "Retry failed action", CommandGroup::Run,
                                            ActionClass::Safe, {{"R", "retry"}});
                retry.previewRequired = true;
                out.push_back(retry);
            }
            break;
        }
        case AppView::SessionBrowser: {
            out.push_back(command("session.open", "Open selected session", CommandGroup::Navigate,
                                  ActionClass::Safe, {{"Enter", "open"}}));
            CommandSpec del = command("session.delete", "Delete selected session", CommandGroup::Danger,
                                      ActionClass::Destructive, {{"d", "delete"}});
            del.previewRequired = true;
            out.push_back(del);
            break;
        }
        case AppView::ProviderPicker:
        case AppView::ManifestSurface:
        case AppView::CommandPalette:
        case AppView::AskModal:
        case AppView::ConfirmModal:
        case AppView::HelpOverlay:
        case AppView::ResizeNotice:
            out.push_back(command("nav.back", "Back", CommandGroup::Navigate, ActionClass::Safe,
                                  {{"Esc", "back"}}));
            break;
    }

    return out;
}

inline std::vector<KeyHint> footerHintsForContext(const UiContext& ctx) {
    std::vector<KeyHint> hints;
    for (const auto& c : commandsForContext(ctx)) {
        if (!c.enabled || c.keys.empty()) continue;
        hints.push_back(c.keys.front());
    }
    return hints;
}

}  // namespace cortex::mk3::ui::model
