# Item Catalog (pick-list)

Mapped to audit numbers + files. Use for custom packs.

## Chrome / feel

| ID | Item | Audit | Files | Pack |
|----|------|-------|-------|------|
| F1 | Header agent identity | #1 | `chat_view.hpp`, `agent_scene.hpp` | Feel |
| F2 | Header grouping agent · provider/model · session | #2 | `chat_view.hpp` | Feel |
| F3 | Idle last-turn summary in status | #3–4 | `chat_view.hpp` | Feel |
| F4 | Prompt glyph / focus affordance | #5 | `chat_view.hpp` | Feel / Composer |
| F5 | Long input left ellipsis polish | #6 | `chat_view.hpp` | Composer |
| F6 | Mode label rename / consistency | #7 | `agent_scene.hpp` | Feel |
| F7 | Graphite dim contrast | #40 | `cortex_theme.hpp` | Feel |
| F8 | Thought/Raw/Notice contrast | #17, #41 | `chat_blocks.hpp` | Feel |
| F9 | Selection highlight strength | #37, #42 | `chat_blocks.hpp` | Feel |
| F10 | Running accent pulse | — | `chat_view.hpp` | Feel / Anim |
| F11 | Toast notice fade | #47-ish | notice draw paths | Feel / Anim |

## Composer

| ID | Item | Audit | Files | Pack |
|----|------|-------|-------|------|
| C1 | Trailing-only trim on submit | #8 | `inkcell_app_model.hpp` | Composer |
| C2 | Ctrl-U / Ctrl-K / Ctrl-W | #9 | `agent_scene.hpp` | Composer |
| C3 | History draft restore | #11 | `agent_scene.hpp` | Composer |
| C4 | Tab only for `/` completion | #9 | `agent_scene.hpp` | Composer |
| C5 | Multi-line composer | #10 | inkcell textarea + scene | **Deferred** |
| C6 | Bracketed paste confirm | #12 | key decode + scene | **Deferred** |

## Nested drill

| ID | Item | Audit | Files | Pack |
|----|------|-------|-------|------|
| N1 | Breadcrumb / back affordance in chrome | #19, #23 | `chat_view.hpp`, `agent_scene.hpp` | Nested |
| N2 | Nested agent identity in header | #20 | `agent_scene.hpp` | Nested |
| N3 | Nested metrics truth | #21–22 | model + scene | Nested |
| N4 | Nested hints | #48 | `agent_scene.hpp` | Nested |
| N5 | Drillable row affordance | #24 | projection labels | Nested |
| N6 | Enter/back micro-slide | #23 | `agent_scene.hpp` + model state | Nested / Anim |

## Transcript / long output

| ID | Item | Audit | Notes |
|----|------|-------|-------|
| T1 | Empty state message | #15 | Likely already present — verify |
| T2 | Scrollbar vs text width | #16 | Small fix |
| T3 | Code fence wrap polish | #13–14 | Medium |
| T4 | Collapsible huge results | #43 | Later |
| T5 | Viewport virtualization | #44 | **Shipped** (span path) |
| T6 | Binary sanitize | live bug | **Shipped** |

## Ask / help

| ID | Item | Audit | Pack |
|----|------|-------|------|
| H1 | Help overlay refresh | #25–27, #50 | Optional add-on |
| A1 | Ask dialog scroll / type colors | #28–32 | Separate medium pack |

## Already good (reference)

- Spinner, cursor blink, field shaders, pill slide, palette anim, workflow pulse
- Pi Esc / Ctrl-C
- Incremental projection + viewport materialization
- Session hub fork/title/delete
