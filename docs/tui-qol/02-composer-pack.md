# Composer Pack

**Goal:** Line-editing and submit behavior that match muscle memory (readline-ish).  
**Effort:** ~1–2 hours.  
**Risk:** Low–medium (key routing; don’t break slash Tab completion).  
**Audit refs:** #5–6, #8–9, #11.

## Problem

- Submit path trims **leading** whitespace → kills intentional code/indent pastes.
- No `Ctrl-U` / `Ctrl-K` / `Ctrl-W` (kill line / to-end / word).
- History Up/Down can clobber a draft (draft field exists; restore discipline incomplete).
- Long input has left-ellipsis already; polish cursor/window edge cases.

## In scope

| # | Change | Primary files |
|---|--------|----------------|
| C1 | Submit: trim **trailing only** | `inkcell_app_model.hpp` `submitComposer` / agent_scene submit path |
| C2 | `Ctrl-U` clear line; `Ctrl-K` kill to EOL; `Ctrl-W` / `Alt-Backspace` kill word back | `agent_scene.hpp` on_key (composer focus) |
| C3 | History: always stash draft before replace; restore when leaving history bounds or Esc in history mode | `agent_scene.hpp`, `promptHistoryDraft` |
| C4 | Tab completion **only** when value starts with `/` (or existing stem rules) — never steal bare Tab for empty line | `agent_scene.hpp` |
| C5 | Prompt glyph polish: focused `›`, unfocused `  `; cursor already blinks — keep | `chat_view.hpp` `drawPromptLine` |
| C6 | Optional: show `hist N/M` dim chip when browsing history | status or prompt right |

## Out of scope

- True multi-line composer (`Ctrl-Enter` send) — larger UX contract
- Bracketed paste confirm dialog
- Clipboard integration beyond existing `/cp-*`

## Acceptance

- [ ] Leading spaces/tabs in submit survive
- [ ] Ctrl-U/K/W work when composer focused and not in ask overlay
- [ ] Up into history then Down past end restores draft
- [ ] Slash completion still works on `/he` + Tab
- [ ] `test-chat-scene` green (extend with Ctrl-U / leading-ws cases)

## Tests to add

- Leading whitespace preserved on submit
- Ctrl-U empties composer
- History draft restore
