#include <iostream>
#include <string>
#include <vector>

#include "inkcell/surface.hpp"
#include "src/ui/chat/chat_view.hpp"
#include "src/ui/views/timeline_view.hpp"

using namespace cortex::mk3::ui;
using namespace cortex::mk3::ui::model;
using namespace cortex::mk3::ui::views;

namespace {
int failures = 0;

void check(bool cond, const std::string& name) {
    if (cond) std::cout << "  " << name << "... PASS\n";
    else {
        std::cout << "  " << name << "... FAIL\n";
        ++failures;
    }
}

std::string rowText(const inkcell::Surface& s, int y) {
    std::string out;
    for (int x = 0; x < s.width(); ++x) out += s.at({x, y}).glyph;
    return out;
}

bool containsRow(const inkcell::Surface& s, const std::string& needle) {
    for (int y = 0; y < s.height(); ++y)
        if (rowText(s, y).find(needle) != std::string::npos) return true;
    return false;
}

TimelineBlock block(BlockKind kind, BlockStatus status, std::string title, std::string summary) {
    TimelineBlock b;
    b.kind = kind;
    b.status = status;
    b.title = std::move(title);
    b.summary = std::move(summary);
    b.hasDetail = true;
    return b;
}

void test_empty_state() {
    inkcell::Surface s({100, 24});
    s.clear(theme::base_bg());
    TimelineViewModel model;
    drawTimeline(s, {2, 2, 96, 10}, model);
    check(containsRow(s, "No turns yet"), "empty state message rendered");
    check(containsRow(s, "Enter sends"), "empty state key hint rendered");
    check(rowText(s, 2).substr(0, 2) == "  ", "empty state preserves left edge inset");
}

void test_selected_block_cues() {
    inkcell::Surface s({100, 24});
    s.clear(theme::base_bg());
    TimelineViewModel model;
    model.focused = true;
    model.selectedIndex = 1;
    model.blocks.push_back(block(BlockKind::Action, BlockStatus::Pending, "tool:fs_read #a1", "reading"));
    model.blocks.back().tags = {"tool", "sync"};
    model.blocks.push_back(block(BlockKind::Result, BlockStatus::Ok, "result #a1 fs_read", "read 42 bytes"));
    model.blocks.back().tags = {"ok", "fs_read"};
    drawTimeline(s, {2, 2, 96, 12}, model);
    check(containsRow(s, "◐ tool:fs_read #a1  [pending]"), "pending action glyph/status rendered");
    check(containsRow(s, "> ✓ result #a1 fs_read  [ok]"), "selected result has marker and glyph");
    check(containsRow(s, "│ read 42 bytes"), "selected body continuation marker rendered");
    check(containsRow(s, "#ok #fs_read"), "tags rendered");
}

void test_drillable_tag() {
    inkcell::Surface s({100, 24});
    s.clear(theme::base_bg());
    TimelineBlock b = block(BlockKind::Action, BlockStatus::Pending, "agent:reader #r1", "inspect");
    b.drillable = true;
    b.related.label = "reader";
    b.tags = {"agent"};
    TimelineViewModel model;
    model.blocks.push_back(b);
    drawTimeline(s, {2, 2, 96, 8}, model);
    check(containsRow(s, "↳ reader"), "drillable child target rendered");
}

void test_chat_block_primitives() {
    check(chat::classifyChatBlock("  YOU") == chat::ChatBlockKind::User,
          "chat block classifies user primitive");
    check(chat::classifyChatBlock("  CORTEX") == chat::ChatBlockKind::Assistant,
          "chat block classifies assistant primitive");
    check(chat::classifyChatBlock("  AGENT  reader  #r1") == chat::ChatBlockKind::Agent,
          "chat block classifies agent primitive");
    check(chat::classifyChatBlock("  TOOL  exec  #e1") == chat::ChatBlockKind::ToolExec,
          "chat block classifies exec builtin");
    check(chat::classifyChatBlock("  TOOL  read  #r1") == chat::ChatBlockKind::ToolRead,
          "chat block classifies read builtin");
    check(chat::classifyChatBlock("  TOOL  fs_write  #w1") == chat::ChatBlockKind::ToolWrite,
          "chat block classifies write builtin");

    // Agent-name aware classification: the assistant label is now the real agent
    // name (e.g. "coder") + model/provider meta, not the generic "CORTEX" sentinel.
    // The classifier matches the agent name first, with "CORTEX" kept as a
    // fallback for standalone tests that never wire agentName.
    const std::string agent = "coder";
    check(chat::classifyChatBlock("  coder  openai-codex/gpt-5.5", agent) ==
              chat::ChatBlockKind::Assistant,
          "chat block classifies real agent-name assistant label");
    check(chat::classifyChatBlock("  AGENT  reader  #ping  ↳", agent) ==
              chat::ChatBlockKind::Agent,
          "chat block classifies subagent action (name + id + drillable)");
    check(chat::classifyChatBlock("  CORTEX", agent) == chat::ChatBlockKind::Assistant,
          "chat block falls back to CORTEX sentinel when no agent name wired");
    check(chat::classifyChatBlock("  coder", "") != chat::ChatBlockKind::Assistant,
          "chat block does not mis-classify a bare agent name as Assistant when no agent name is wired");

    inkcell::Surface surface({60, 14});
    chat::ChatSurfaceModel model;
    model.transcript = {"  YOU", "    hello", "", "  TOOL  exec  #e1", "    pwd"};
    chat::drawTranscript(surface, {2, 2, 56, 8}, model);
    // Top-anchored: short transcripts start at the top of the body (body.y = 2),
    // not bottom-anchored with a void above.
    int firstY = 2;
    auto userBg = chat::blockBackground(chat::ChatBlockKind::User);
    auto execBg = chat::blockBackground(chat::ChatBlockKind::ToolExec);
    auto baseBg = theme::base_bg().bg;
    check(inkcell::same_color(surface.at({2, firstY}).style.bg, userBg) &&
              inkcell::same_color(surface.at({2, firstY + 1}).style.bg, userBg),
          "chat user background spans header and body");
    // Empty separator line inherits the prior block's background (contiguous:
    // no base-bg gutter between the YOU block and the TOOL block).
    check(inkcell::same_color(surface.at({2, firstY + 2}).style.bg, userBg) &&
              !inkcell::same_color(surface.at({2, firstY + 2}).style.bg, baseBg),
          "chat empty separator inherits the prior block background (contiguous)");
    check(inkcell::same_color(surface.at({2, firstY + 3}).style.bg, execBg) &&
              inkcell::same_color(surface.at({2, firstY + 4}).style.bg, execBg),
          "chat builtin background spans header and body");
}

void test_chat_transcript_wraps_long_lines() {
    std::vector<std::string> source = {
        "  This response is deliberately longer than the available transcript width and must wrap.",
    };
    auto lines = chat::wrapTranscript(source, 32);
    check(lines.size() >= 3, "chat transcript wraps long response");
    bool preservedIndent = true;
    for (const auto& line : lines) preservedIndent = preservedIndent && line.rfind("  ", 0) == 0;
    check(preservedIndent, "chat transcript preserves indentation while wrapping");

    std::string token(90, 'x');
    auto tokenLines = chat::wrapTranscript({"    " + token}, 24);
    std::string reconstructed;
    for (const auto& line : tokenLines) reconstructed += line.substr(std::min<size_t>(4, line.size()));
    check(reconstructed == token, "chat wrapping never truncates long tokens");

    auto semantic = chat::wrapTranscript({"  AGENT  reader  #ping  ↳"}, 40);
    check(semantic.size() == 1 && semantic[0] == "  AGENT  reader  #ping  ↳",
          "chat wrapping preserves semantic header spacing");
    auto selectedSemantic = chat::wrapTranscript({"› AGENT  reader  #ping  ↳"}, 40);
    check(selectedSemantic.size() == 1 && selectedSemantic[0] == "› AGENT  reader  #ping  ↳",
          "selected semantic header preserves spacing");

    auto code = chat::wrapTranscript({"    ```cpp", "    int  x = 1;", "    ```"}, 40);
    check(code.size() == 3 && code[0].find("┌─ cpp") != std::string::npos &&
              code[1].find("│ int  x = 1;") != std::string::npos &&
              code[2].find("└─") != std::string::npos,
          "chat code fences preserve code whitespace");
}

void test_chat_wrap_cache() {
    inkcell::Surface surface({60, 14});
    std::vector<std::string> source = {"  CORTEX", "    cached response"};
    chat::TranscriptWrapCache cache;
    chat::ChatSurfaceModel model;
    model.transcriptSource = &source;
    model.transcriptCache = &cache;
    model.transcriptVersion = 7;
    chat::drawTranscript(surface, {2, 2, 56, 8}, model);
    check(cache.sourceVersion == 7 && cache.width == 55 && !cache.lines.empty(),
          "chat wrap cache records source version and width");
    const auto* storage = cache.lines.data();
    chat::drawTranscript(surface, {2, 2, 56, 8}, model);
    check(cache.lines.data() == storage, "chat wrap cache reuses wrapped storage without mutation");
    source.push_back("    updated");
    model.transcriptVersion = 8;
    chat::drawTranscript(surface, {2, 2, 56, 8}, model);
    check(cache.sourceVersion == 8 && cache.lines.size() == 3,
          "chat wrap cache invalidates on transcript version change");
}

void test_chat_wrap_cache_incremental_tail() {
    // Incremental wrap: on a transcriptVersion bump at the same width, only the
    // dirty tail re-wraps. The stable prefix is reused as-is, and code-fence state
    // carries across the boundary so appended code lines render inside the fence.
    inkcell::Surface surface({60, 20});
    std::vector<std::string> source = {
        "  CORTEX",
        "    first response line",
        "    ```cpp",
        "    int x = 1;",
    };
    chat::TranscriptWrapCache cache;
    chat::ChatSurfaceModel model;
    model.transcriptSource = &source;
    model.transcriptCache = &cache;
    model.transcriptVersion = 1;
    chat::drawTranscript(surface, {2, 2, 56, 16}, model);
    size_t initialLines = cache.lines.size();
    check(initialLines == 4, "incremental wrap baseline produces 4 display lines");
    check(cache.sourceSnapshot.size() == source.size() &&
              cache.sourceLineSpans.size() == source.size(),
          "incremental wrap records source snapshot and per-line spans");
    check(cache.lines[2].find("┌─ cpp") != std::string::npos,
          "incremental wrap baseline renders open code fence");
    std::vector<std::string> prefixCopy(cache.lines.begin(), cache.lines.end());

    // Append a code-body line; fence state must carry (renders with │ prefix).
    source.push_back("    int y = 2;");
    model.transcriptVersion = 2;
    chat::drawTranscript(surface, {2, 2, 56, 16}, model);
    check(cache.lines.size() == initialLines + 1,
          "incremental append adds exactly one tail line");
    check(cache.lines.back().find("│ int y = 2;") != std::string::npos,
          "incremental wrap carries code-fence state across boundary");
    bool prefixIntact = true;
    for (size_t i = 0; i < initialLines; ++i)
        if (cache.lines[i] != prefixCopy[i]) { prefixIntact = false; break; }
    check(prefixIntact, "incremental wrap preserves stable prefix on append");

    // Mutate the last line; dirty start = last source line, tail re-wrapped.
    source[4] = "    int z = 3;";
    model.transcriptVersion = 3;
    chat::drawTranscript(surface, {2, 2, 56, 16}, model);
    check(cache.lines.back().find("│ int z = 3;") != std::string::npos,
          "incremental wrap re-wraps mutated tail line");
    check(cache.lines.size() == initialLines + 1,
          "incremental wrap preserves line count on tail mutate");
    prefixIntact = true;
    for (size_t i = 0; i < initialLines; ++i)
        if (cache.lines[i] != prefixCopy[i]) { prefixIntact = false; break; }
    check(prefixIntact, "incremental wrap preserves stable prefix on tail mutate");

    // Close the fence; carried open state makes ``` render as └─.
    source.push_back("    ```");
    model.transcriptVersion = 4;
    chat::drawTranscript(surface, {2, 2, 56, 16}, model);
    check(cache.lines.back().find("└─") != std::string::npos,
          "incremental wrap renders fence close after carried open state");
    check(cache.lines.size() == initialLines + 2,
          "incremental wrap appends fence-close line");
    prefixIntact = true;
    for (size_t i = 0; i < initialLines; ++i)
        if (cache.lines[i] != prefixCopy[i]) { prefixIntact = false; break; }
    check(prefixIntact, "incremental wrap preserves stable prefix across fence close");
}

void test_chat_selection_stays_visible_after_wrap() {
    inkcell::Surface s({60, 16});
    chat::ChatSurfaceModel model;
    model.historyFocused = true;
    model.followBottom = false;
    for (int i = 0; i < 18; ++i) model.transcript.push_back("    history line " + std::to_string(i));
    model.transcript.push_back("› CORTEX");
    model.transcript.push_back("    selected response");
    chat::drawTranscript(s, {2, 2, 56, 10}, model);
    check(containsRow(s, "› CORTEX"), "selected transcript block remains visible");
}

void test_chat_nested_sub_block_rendering() {
    // Sub-agent nesting, the clean way: the child's final response is
    // appended as extra body lines of the parent's Result block (6-space
    // indent, 2 deeper than the parent's own 4-space body). No sub-rect,
    // no new color — the lines inherit the parent block's kind via
    // buildBlockMetadata, so the render paints them with the parent's
    // own bg. 'Pad with the main parent bg/block' — one cohesive block.
    inkcell::Surface s({60, 14});
    chat::ChatSurfaceModel model;
    // 2 lines: a Result header + an indented sub-content body line
    // (the rebuildViews sub-content prefix is "      " = 6 spaces).
    model.transcript = {"  \xe2\x9c\x93 RESULT  reader  #r1  \xe2\x86\xb3",
                       "      child final answer here"};
    chat::drawTranscript(s, {2, 2, 56, 8}, model);
    // Both the header row and the sub-content row share the parent
    // block's bg (ResultOk) — no separate sub-rect, no frame, no
    // distinct sub-color. The sub-content is a padded, indented part
    // of the parent block.
    auto headerCell = s.at({3, 2});   // header row, x=3
    auto subCell    = s.at({3, 3});   // sub-content row, x=3
    check(inkcell::same_color(subCell.style.bg, headerCell.style.bg),
          "nested sub-content shares the parent block's bg (no frame, no sub-color)");
    // The sub-content text starts at x=body.x+6 (2 deeper than the
    // parent's body at body.x+4), marking the visual nesting depth
    // while staying inside the parent's padded bg.
    auto subText = s.at({2 + 6, 3});
    check(!subText.glyph.empty(),
          "nested sub-content text is drawn at x=body.x+6 (indented within parent bg)");
}

void test_chat_transcript_empty_state() {
    // First-run UX: an empty transcript body must show a centered dim
    // headline + tip instead of a blank void between the header and the
    // status line. The empty state is hidden the moment any content exists.
    inkcell::Surface s({60, 14});
    chat::ChatSurfaceModel model;
    model.transcript = {};   // empty
    chat::drawTranscript(s, {2, 2, 56, 8}, model);
    // The empty state renders the headline + tip centered in the body.
    // The body starts at y=2 and is 8 rows tall, so the headline sits at
    // y = 2 + (8/2 - 1) = 5 and the tip at y = 6.
    std::string headline = rowText(s, 5);
    std::string tip = rowText(s, 6);
    check(headline.find("No conversation yet") != std::string::npos,
          "empty transcript shows the centered headline");
    check(tip.find("Type a prompt below") != std::string::npos,
          "empty transcript shows the centered tip line");
    // Non-empty transcript: the empty state must NOT show. Use a fresh
    // surface so leftover cells from the empty-state draw above don't
    // false-positive the suppression check.
    inkcell::Surface s2({60, 14});
    chat::ChatSurfaceModel populated;
    populated.transcript = {"  YOU", "    hello"};
    chat::drawTranscript(s2, {2, 2, 56, 8}, populated);
    std::string anyRow2;
    for (int y = 2; y < 10; ++y) anyRow2 += rowText(s2, y);
    check(anyRow2.find("No conversation yet") == std::string::npos,
          "populated transcript suppresses the empty state headline");
}

void test_chat_prompt_empty_no_placeholder() {
    // Design: elevated prompt bar, no instructional placeholder copy.
    // Focused empty = glyph + blinking cursor only. Unfocused = quiet bar.
    inkcell::Surface s({60, 8});
    chat::ChatSurfaceModel model;
    model.input = "";
    model.inputCursor = 0;
    model.inputFocused = true;
    model.nowMs = 0;  // cursor visible phase
    chat::drawChatSurface(s, {0, 0, 60, 8}, model);
    std::string prompt = rowText(s, 7);  // bottom row
    check(prompt.find("Ask anything") == std::string::npos,
          "empty focused composer has no placeholder copy");
    check(prompt.find("\xe2\x96\x88") != std::string::npos || prompt.find("›") != std::string::npos,
          "empty focused composer shows prompt glyph/cursor");
    chat::ChatSurfaceModel unfocused;
    unfocused.input = "";
    unfocused.inputCursor = 0;
    unfocused.inputFocused = false;
    chat::drawChatSurface(s, {0, 0, 60, 8}, unfocused);
    std::string unfocusedPrompt = rowText(s, 7);
    check(unfocusedPrompt.find("Ask anything") == std::string::npos,
          "unfocused composer has no placeholder copy");
    chat::ChatSurfaceModel typing;
    typing.input = "hello";
    typing.inputCursor = 5;
    typing.inputFocused = true;
    typing.nowMs = 0;
    chat::drawChatSurface(s, {0, 0, 60, 8}, typing);
    std::string typingPrompt = rowText(s, 7);
    check(typingPrompt.find("hello") != std::string::npos,
          "typing renders the input in the composer");
}

void test_chat_prompt_cursor_position() {
    inkcell::Surface s({40, 8});
    chat::ChatSurfaceModel model;
    model.input = "abcd";
    model.inputCursor = 2;
    model.inputFocused = true;
    model.nowMs = 0;  // cursor-on phase
    chat::drawChatSurface(s, {0, 0, 40, 8}, model);
    check(containsRow(s, "› ab█cd"), "chat prompt renders cursor at model position");
}

void test_chat_prompt_keeps_input_while_running() {
    // Regression: the composer must not be replaced by a running placeholder.
    // The status line (● agent running) already communicates the running state.
    inkcell::Surface s({40, 8});
    chat::ChatSurfaceModel model;
    model.input = "hello";
    model.inputCursor = 5;
    model.inputFocused = true;
    model.running = true;
    model.status = "agent running";
    model.pendingOps = 2;
    model.actionCount = 3;
    model.nowMs = 0;
    chat::drawChatSurface(s, {0, 0, 40, 8}, model);
    std::string promptRow = rowText(s, 7);
    check(promptRow.find("hello") != std::string::npos,
          "composer keeps input text while running");
    check(promptRow.find("agent running") == std::string::npos,
          "composer is not replaced by running placeholder");
    // Elevated status row carries live chips (pend/act/…) + spinner — not prose
    // stuffed into the prompt.
    std::string statusRow = rowText(s, 6);
    check(statusRow.find("pend") != std::string::npos || statusRow.find("act") != std::string::npos,
          "status line still reports running state via metrics chips");
}

void test_chat_subagent_scope_chrome() {
    // Subagent drilldown UX: the header breadcrumb highlights the current scope
    // (amber, matching AGENT blocks) and the status line shows a ◀ <name>
    // indicator so the operator knows WHERE they are in the agent tree.
    inkcell::Surface s({120, 24});
    chat::ChatSurfaceModel model;
    model.title = "CORTEX MK3";
    model.path = "root / reader";
    model.provider = "openai-codex";
    model.model = "gpt-5.5";
    model.scopeName = "reader";
    chat::drawHeader(s, {0, 0, 120, 1}, model);
    std::string header = rowText(s, 0);
    check(header.find("CORTEX MK3") != std::string::npos,
          "header renders the title");
    check(header.find("root") != std::string::npos && header.find("reader") != std::string::npos,
          "header renders the full breadcrumb path");
    // The drilled-in scope segment should be styled amber; find any amber cell
    // in the header row to confirm the highlight.
    int amberCol = -1;
    for (int x = 0; x < s.width(); ++x) {
        if (inkcell::same_color(s.at({x, 0}).style.fg, theme::amber().fg)) { amberCol = x; break; }
    }
    check(amberCol >= 0, "header highlights the drilled-in scope in amber");

    // Status line: when scoped, it carries the ◀ reader indicator.
    chat::drawStatusLine(s, {0, 4, 120, 1}, model);
    std::string status = rowText(s, 4);
    check(status.find("\xe2\x97\x80") != std::string::npos && status.find("reader") != std::string::npos,
          "status line shows the drilled-in scope indicator when scoped");
    // At root: no scope indicator.
    chat::ChatSurfaceModel rootModel;
    rootModel.title = "CORTEX MK3";
    rootModel.path = "root";
    rootModel.provider = "openai-codex";
    rootModel.model = "gpt-5.5";
    chat::drawStatusLine(s, {0, 5, 120, 1}, rootModel);
    std::string rootStatus = rowText(s, 5);
    check(rootStatus.find("\xe2\x97\x80") == std::string::npos,
          "status line omits the scope indicator at root");
}

void test_chat_help_and_theme() {
    inkcell::Surface s({100, 30});
    theme::set(theme::Variant::Graphite);
    check(std::string(theme::name()) == "graphite", "graphite is the default chat theme");
    chat::drawHelpOverlay(s, {2, 2, 96, 26});
    check(containsRow(s, "CHAT HELP"), "chat help overlay renders on demand");
    check(containsRow(s, "T           switch graphite / neon"),
          "chat help documents theme switching");
    theme::toggle();
    check(std::string(theme::name()) == "neon", "chat theme toggles to neon");
    theme::set(theme::Variant::Graphite);
}

void test_ask_dialog_overlay() {
    inkcell::Surface s({100, 30});
    Json::Value params;
    params["chainTitle"] = "Select worker";
    Json::Value card;
    card["id"] = "worker";
    card["type"] = "choice";
    card["title"] = "Worker";
    card["message"] = "Choose one worker.";
    card["options"].append("reader");
    card["options"].append("tester");
    params["cards"].append(card);
    auto state = chat::parseDialogState(params);
    chat::drawAskDialog(s, {2, 2, 96, 26}, state, "", {});
    check(containsRow(s, "Select worker"), "ask overlay renders chain title");
    check(containsRow(s, "> reader"), "ask overlay renders selected choice");
    check(containsRow(s, "Enter choose"), "ask overlay renders interaction hint");
}
}  // namespace

int main() {
    std::cout << "UI view tests\n";
    test_empty_state();
    test_selected_block_cues();
    test_drillable_tag();
    test_chat_block_primitives();
    test_chat_transcript_wraps_long_lines();
    test_chat_wrap_cache();
    test_chat_wrap_cache_incremental_tail();
    test_chat_selection_stays_visible_after_wrap();
    test_chat_prompt_cursor_position();
    test_chat_prompt_keeps_input_while_running();
    test_chat_prompt_empty_no_placeholder();
    test_chat_transcript_empty_state();
    test_chat_nested_sub_block_rendering();
    test_chat_subagent_scope_chrome();
    test_chat_help_and_theme();
    test_ask_dialog_overlay();
    std::cout << "\n" << (failures == 0 ? "all passed" : "failures: " + std::to_string(failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
