---
name: inkcell
description: >
  MASTERCLASS skill for the inkcell C++17 terminal UI framework — architecture,
  API surface, runtime loop, input/keymap/actions, Surface/Renderer, widgets,
  Shell/Region, canvas, shaders/sprites, snapshots, boilerplates, and the Cortex
  MK3 boundary. Activate for ANY inkcell work: build, debug, feature, review,
  app scaffold, visual regression, performance, key decoding, or when paths
  touch ~/repos/active/inkcell, include/inkcell, libinkcell, Surface/Scene/Engine,
  KeyDecoder, Renderer, or Cortex experimental TUI that depends on inkcell.
  Also trigger: "inkcell", "TUI framework", "cell buffer", "snapshot preview",
  "Ctrl-J vs Enter", "differential renderer", "inkcell widget".
  Does NOT activate for: pure Cortex agent protocol/harness without UI, ncurses,
  FTXUI/notcurses/TermOx, general C++ unrelated to inkcell, CMake-first TUI stacks.
---

# inkcell — Masterclass Agent Skill

You are operating with **full-stack fluency** on inkcell. Prefer this skill + live
headers over rediscovery. When uncertain, read the header named in the map —
not random examples.

| Fact | Value |
|------|-------|
| Repo | `~/repos/active/inkcell` (also symlink `second-brain-stack/libs/inkcell`) |
| Language | **C++17** |
| Build | **Makefile-first** — no CMake requirement, **no ncurses** |
| Namespace | `inkcell` |
| Umbrella | `#include "inkcell/inkcell.hpp"` |
| Lib | `build/libinkcell.a` |
| Install | `PREFIX` default `$HOME/.local` |
| License | MIT |
| Origin | Extracted from `sbtui`; treat `sbtui` as historical alias |
| Cortex consumer | `~/repos/active/agent-lib-cpp` via `INKCELL_ROOT ?= ../inkcell` |
| Canon docs | `LLM_DOCS.md` (agent), `README.md`, `docs/ARCHITECTURE.md`, `docs/COOKBOOK.md` |

---

## 0. Non-negotiable mental model

```text
terminal bytes
  → KeyDecoder          (src/key.cpp)
  → KeyMap / ScopedKeyMap
  → Action              (stable string id)
  → Engine / App
  → Scene::on_key?  → if consumed, skip keymap
  → Scene::update(Tick, Action)
  → Scene::draw(Surface&)
  → Renderer (full / differential)
  → Terminal backend
```

### Laws

1. **Draw into `Surface`, never the terminal.** Only `Terminal` + `Renderer` emit ANSI.
2. **Keys are not logic; Actions are protocol.** Map `"j"` → `"list.next"`; scenes switch on actions.
3. **No product domain in `include/inkcell`.** No agents, sessions, SaaS entities, protocols.
4. **Header-heavy library.** Almost everything is headers; `src/` is `key.cpp` + `terminal_posix.cpp`.
5. **Snapshot-first examples.** Defaults must not hang CI; `--live` is opt-in.
6. **Cortex chrome stays in Cortex.** Reusable TUI chrome for MK3 lives in
   `agent-lib-cpp/src/ui/{assets,components,layout,theme,gfx}` — do **not** upstream
   Cortex-specific hub/palette/field-raster into inkcell unless generalized twice.

---

## 1. Repo map (where truth lives)

```text
inkcell/
  include/inkcell/          # PUBLIC API
    inkcell.hpp             # umbrella
    engine.hpp app.hpp scene.hpp action.hpp
    key.hpp keymap.hpp
    surface.hpp style.hpp renderer.hpp ansi.hpp terminal.hpp
    geometry.hpp text.hpp draw.hpp layout.hpp
    theme.hpp focus.hpp command.hpp asset.hpp
    animation.hpp sprite.hpp shader.hpp viewport2d.hpp output.hpp
    widgets/                # leaf widgets (input, textarea, list, palette, …)
    ui/                     # Shell, Region, debug_overlay
    canvas/                 # infinite canvas document/controller/renderer
  src/key.cpp               # KeyDecoder implementation (Enter vs Ctrl-J HERE)
  src/terminal_posix.cpp
  tests/                    # test_core, test_input, test_render, test_runtime, assets
  examples/                 # public contracts (snapshot default)
  boilerplates/             # copy-out starters
  docs/                     # human docs + docs/previews/*.ansi
  LLM_DOCS.md               # dense agent pack — read for deep dives
  Makefile
```

### Where do I change X?

| Goal | Start |
|------|--------|
| Widget | `include/inkcell/widgets/`, example, optional asset |
| Theme role | `theme.hpp` + themed widgets + tests |
| Runtime loop | `engine.hpp` |
| App sugar | `app.hpp` |
| Shell chrome | `ui/shell.hpp`, `examples/app_shell_demo.cpp` |
| Commands metadata | `command.hpp` |
| Canvas | `canvas/*`, `examples/infinite_canvas_demo.cpp` |
| Key decode bugs | `src/key.cpp` + `tests/test_input.cpp` |
| Diff renderer / SGR | `renderer.hpp`, `ansi.hpp`, `style.hpp` |
| Snapshots | `scripts/export-previews.sh`, `make diff` / `approve` |
| Cortex integration | `agent-lib-cpp` only — link `libinkcell.a` |

---

## 2. Core types (API fluency)

### Geometry — `geometry.hpp`
`Size {w,h}` · `Point {x,y}` · `Vec2 {float}` · `Rect {x,y,w,h}` with `empty/right/bottom/inset/intersect`.

### Style — `style.hpp`
```cpp
Color::rgb(r,g,b)
Style::normal() | .with_fg() | .with_bg() | .strong() | .faint() | .emphasis() // italic
// fields: fg, bg, bold, dim, italic
```
Renderer emits SGR **differentially** mid-row. Italic = SGR `3` / `23` (`feb1fa6`).

### Surface — `surface.hpp`
Retained cell grid. `put`, `fill`, `text`, `box(BorderStyle)`, `hline`/`vline`, clip stack, `blit`.
**Never** write ANSI strings into cells as a substitute for Style.

### Scene — `scene.hpp`
```cpp
struct Tick { int delta_ms; };
class Scene {
  virtual void on_enter()/on_exit()/on_resize(Size);
  virtual void update(Tick, Action);
  virtual bool on_key(const KeyEvent&); // true = consumed, keymap skipped
  virtual void draw(Surface&) const = 0;
};
// SceneHost: add/switch_to/update/on_key/draw
```

### Action — `action.hpp`
String id wrapper: `.is("app.quit")`, `.any({...})`. Prefer `domain.verb` IDs.

### Key — `key.hpp` + `src/key.cpp`
```text
KeyCode: Character, Enter, Escape, Tab, BackTab, Backspace, Delete,
         Arrows, CtrlC, CtrlD, Function, Paste, FocusIn/Out, …
KeyEvent: code, ch, text (UTF-8), modifiers, raw
KeyDecoder::decode / feed / flush   // streaming CSI/UTF-8/paste
```

#### Critical decode law (do not regress)
| Byte | Meaning |
|------|---------|
| `\r` (CR) | **`KeyCode::Enter`** |
| `\n` (LF) | **Character `j` + `ModCtrl`** (Ctrl-J) |

Committed inkcell `5a0c246`. Cortex hub binds Ctrl-J/K for section cycle. If Ctrl-J “does nothing”, check decoder first — not the scene.

### KeyMap — `keymap.hpp`
Bind key chords → action ids. `ScopedKeyMap` for modal layers (palette, dialog).

### Engine / App
- **Engine**: terminal lifecycle, select loop, wake/eventfd, scene host, renderer, tick.
- **App**: thin fluent wrapper — theme, commands, assets next to Engine.

### CommandRegistry — `command.hpp`
Metadata only (title, category, description, default_key, tags). Not the executor.

### Draw / Layout
`draw.hpp`: lines, boxes, polylines, LineStyle.  
`layout.hpp` + `ui/region.hpp`: splits, docks — prefer over hand math.  
`ui/shell.hpp`: header/body/footer/overlay chrome.

### Widgets (leaf)
`input`, `textarea`, `scroll_view`, `list`, `menu`, `dialog`, `command_palette`,
`status_bar`, `gauge`, `slider`, `spinner`, `tree`, `timeline`, `metric_card`,
`kanban`, `calendar`, `activity_feed`, `key_hints`, `panel`, `pet`, `infinite_canvas`, …

### Sprite / Animation / Shader
- **Sprite**: row strings + transparent char → blit to Surface.
- **TweenFloat / animation**: frame-driven interpolation helpers.
- **CellShader**: `apply_shader(Surface, Rect, fn, frame, time)` — per-cell transform.
  This is inkcell’s shader. Cortex MK3 also has a separate **field-raster** half-block
  system under `agent-lib-cpp/src/ui/gfx/` — do not confuse the two.

### Canvas package
Document/controller/renderer for pan-zoom infinite surfaces (`examples/infinite_canvas_demo`).

---

## 3. Build / verify / install

```bash
cd ~/repos/active/inkcell

make                  # lib + tests + examples + boilerplates
make lib              # build/libinkcell.a
make test             # core
make test-input       # key decoder (Enter vs Ctrl-J)
make test-render
make test-runtime
make test-examples    # snapshot smoke — must not hang
make test-boilerplates
make warning-budget   # header/core changes
make snapshots && make diff   # visual delta
make approve          # ONLY after intentional visual review
make lsp              # compile_commands / clangd
make install | uninstall | reinstall
make clean
```

### Cortex rebuild after inkcell header changes
```bash
cd ~/repos/active/inkcell && make lib
cd ~/repos/active/agent-lib-cpp
# Makefile header deps are weak — force:
rm -f cortex-mk3 build/main.o build/testing/*.o
make cortex-mk3 test-chat-scene
```

---

## 4. Canonical app skeleton

```cpp
#include "inkcell/inkcell.hpp"

struct MainScene : inkcell::Scene {
    void update(inkcell::Tick t, inkcell::Action a) override {
        if (a.is("app.quit")) { /* signal quit via engine/app */ }
    }
    bool on_key(const inkcell::KeyEvent& e) override {
        // return true only when focus-owned (e.g. typing in TextArea)
        return false;
    }
    void draw(inkcell::Surface& s) const override {
        s.clear(inkcell::Style::panel());
        s.text({2, 1}, "hello", inkcell::Style::accent());
    }
};

int main() {
    inkcell::App app;
    // register scene, keymap bindings → actions, run
}
```

Patterns:
- Snapshot path: render once to stdout / file when not a TTY or `SNAPSHOT` env.
- Live path: `Engine::run()` alt-screen, bracketed paste, focus events.
- Keep `main.cpp` tiny; logic in scenes + pure models.

Copy starters: `boilerplates/app-framework/`, `boilerplates/vendor-app/`.

---

## 5. Testing & visual discipline

| Gate | Meaning |
|------|---------|
| `make test` | logic/API |
| `make test-input` | decoder regressions |
| `make test-examples` | examples don’t hang; snapshot OK |
| `make diff` | ANSI golden delta |
| `make approve` | update goldens **intentionally** |
| `make warning-budget` | no warning creep |

**Never** claim green from `cmd \| grep` pipelines alone — pipelines mask failures.

---

## 6. Coding conventions (agent)

### Do
- C++17, modular, no stubs/TODOs on shipped paths
- Fluent builders where the API already is fluent
- Stable action IDs (`list.next`, `palette.open`, `app.quit`)
- Semantic theme roles in shared widgets
- Stage **explicit** git paths only
- Explain WHY in comments, not WHAT

### Don’t
- Raw ANSI from widgets/apps
- Product domain types in `include/inkcell`
- Live-only demos in default smoke set
- Casual public action ID renames
- CMake/ncurses “just because”
- Upstream Cortex hub chrome into inkcell on first use
- `git add -A` / force-push unless ordered

### Promotion rule
Shared abstraction needs **two real call sites** + tests before entering core.

---

## 7. Playbooks

### A. New widget
1. `include/inkcell/widgets/foo.hpp`
2. Example usage (snapshot default)
3. Tests if non-trivial
4. Optional `AssetKind::Widget` entry
5. `make test test-examples warning-budget`

### B. Key / input bug
1. Reproduce with `tests/test_input.cpp` unit first
2. Fix `src/key.cpp` decoder state machine
3. `make test-input`
4. Re-check Cortex hub bindings if chord-related

### C. Visual / renderer change
1. Touch `style.hpp` / `ansi.hpp` / `renderer.hpp` carefully (ABI/layout)
2. `make test-render test-examples`
3. `make snapshots && make diff` → inspect → `approve` if intentional
4. Force-rebuild Cortex

### D. Performance
- Differential SGR already mid-row (`renderer`)
- Engine keeps persistent front/back surfaces (no per-frame realloc)
- Profile before cleverness; prefer less put/fill churn

### E. Cortex feature that needs a primitive
1. Implement Cortex-local first (`agent-lib-cpp/src/ui/...`)
2. If truly generic (second consumer), extract to inkcell with tests + example
3. Bump inkcell, rebuild Cortex against new lib

### F. Scaffold product app
1. Copy boilerplate
2. Point `INKCELL_DIR` / link `libinkcell.a`
3. Rename namespace; commands in one registry
4. Data models free of render includes

---

## 8. Cortex MK3 boundary (load-bearing)

| Layer | Owns |
|-------|------|
| **inkcell** | cells, input, scenes, widgets, render, terminal |
| **Cortex `src/ui`** | hub, chat transcript, manifests registry UX, field-raster, cmd palette chrome, prefs |
| **Cortex `src/core`** | agents, protocol, tools, workflows engine, sessions |

Recent Cortex UI (do not “fix” by moving into inkcell):
- Manifests hub + textured pill + Settings AAA
- Cell field-raster shaders (`src/ui/gfx/`) — separate from `inkcell::shader`
- Animated cmd palette, card swipe
- Prefs: `~/.config/cortex-mk3/ui.json`

Inkcell commits Cortex depends on recently:
- `5a0c246` Enter(CR) ≠ Ctrl-J(LF)
- `feb1fa6` italic SGR on Style + differential renderer
- perf: differential SGR, persistent surfaces

---

## 9. Decision tree

| Question | Answer |
|----------|--------|
| Render UI? | `Surface` + widgets/draw/Shell — never raw ANSI |
| Input behavior? | KeyMap → Action → `Scene::update`; typing via `on_key` |
| New app? | boilerplate copy |
| Demo a feature? | example, snapshot default |
| Belongs in core? | only if domain-free and second use exists |
| Visual change? | snapshots/diff/approve |
| Ctrl-J broken? | `src/key.cpp` first |
| Cortex-only chrome? | stay in `agent-lib-cpp/src/ui` |

---

## 10. Doc index

| Doc | Use |
|-----|-----|
| `LLM_DOCS.md` | full agent instant pack |
| `README.md` | product face |
| `docs/ARCHITECTURE.md` | layers |
| `docs/COOKBOOK.md` | recipes |
| `docs/SNAPSHOTS.md` | golden workflow |
| `docs/API.md` | header map |
| `docs/PERFORMANCE.md` | perf notes |
| `docs/TROUBLESHOOTING.md` | failure modes |
| `docs/DESIGN_PRINCIPLES.md` | invariants |
| This skill | operating contract for agents |

---

## 11. Anti-patterns (instant reject)

- Emitting `\033[` from a widget or Cortex scene “just this once”
- Mapping `\n` back to Enter
- Putting `Agent` / protocol types under `include/inkcell`
- Claiming tests passed via grepped pipelines
- Approving snapshot diffs without reading them
- Flattening Cortex textured pill “for simplicity”
- Duplicating inkcell widgets inside Cortex when composition works
- Adding CMake as the primary build

---

## 12. 30-second boot

```bash
cd ~/repos/active/inkcell
make test && make test-input
make lsp
# optional live:
# make examples && ./build/examples/app_shell_demo --live
```

Read order on a fresh task:
1. This skill  
2. Named headers from the map  
3. One relevant example  
4. `LLM_DOCS.md` § for depth  
5. Narrowest `make` proof  

**GODSPEED. Draw cells. Map keys to actions. Keep domain out of the framework.**
