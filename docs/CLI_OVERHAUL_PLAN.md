# CLI Overhaul Plan

Scope: `src/cli/` (2511 LOC across 6 headers). Goal: a CLI layer that is
single-purpose per file, declarative (flags defined once, help generated),
and easy to extend without drift. This is a *plan* — land it incrementally,
each step build-verified, never a big-bang rewrite.

## Current state

| File | LOC | Responsibility (actual) |
|---|---|---|
| `options.hpp` | 719 | `CliConfig` struct + 5 hardcoded help printers + config-file IO + getopt parsing |
| `run.hpp` | 776 | helpers + interactive picker + model validation + manifest resolution + `cmdRun` + serve/list/config dispatch |
| `commands.hpp` | 456 | subcommand helpers |
| `session.hpp` | 382 | session subcommands |
| `list_picker.hpp` | 155 | interactive list picker |
| `serve.hpp` | 23 | server bootstrap |

## Problems (with evidence)

1. **`options.hpp` violates SRP.** Four unrelated concerns in one file:
   config struct, help text, config-file load/save/apply, and `parseArgs`.
2. **Flag definitions live in 3 places** that drift independently:
   - the getopt `longOpts[]` table (`options.hpp:321`),
   - the hardcoded help strings (`printHelpGeneral/Run/Serve/List/Config`),
   - the `applyConfig` key→field mapping (`options.hpp:278`).
   Add a flag and you must edit all three (and forget one silently).
3. **Magic long-only option numbers** (`1002`…`1035`) scattered through the
   getopt table — opaque, collision-prone.
4. **`list` has a hand-rolled arg parser** (`options.hpp:~371`) that runs
   *before* getopt and re-implements `--providers/--models/--tools/--agents/
   --sessions` parsing. Two parsers, divergent semantics.
5. **Help drifts.** The `--provider` list in `printHelpGeneral` is hardcoded
   and already stale (omits `opencode-go`, `sambanova`, `cerebras`, `llm7`,
   etc. that `list --providers` shows).
6. **`run.hpp` is a grab-bag** — pure helpers (`endsWith`) next to UI
   (`interactivePicker`) next to dispatch (`cmdRun`), no separation.
7. **Config precedence is implicit** — `providerSet/modelSet/providerFromSession/
   modelFromSession` booleans + `applyConfig` + `applySessionMetadata`. The
   actual precedence (config-file < CLI < session < agent.yml) is only
   discoverable by tracing flags.

## Target architecture

```
src/cli/
├── cli_types.hpp      # CliConfig, grouped sub-structs (Provider, Session, Server, Render, Debug)
├── cli_flags.hpp      # ONE declarative flag registry (single source of truth)
├── cli_help.hpp       # generated help from the registry (per-command scopes)
├── cli_config.hpp     # config-file load/save/apply (defaults + precedence resolver)
├── cli_parse.hpp      # getopt_long → registry → CliConfig (drop the `list` special-case)
├── commands/
│   ├── run.hpp        # cmdRun
│   ├── serve.hpp      # cmdServe
│   ├── list.hpp       # cmdList (providers/models/tools/agents/sessions)
│   ├── config.hpp     # cmdConfig (show/set/init)
│   ├── completions.hpp# shell completions
│   └── sessions.hpp   # session subcommands
├── run.hpp            # thin dispatcher: command string → handler
└── list_picker.hpp    # unchanged (UI helper)
```

### Flag registry sketch (the core win)

```cpp
struct CliFlag {
    std::string longName;       // "provider"
    char shortOpt = 0;          // 'P' (0 = long-only)
    bool takesArg = false;      // optional vs required encoded separately
    bool optionalArg = false;
    std::string metavar;        // "<name>"
    std::string description;
    uint8_t scope;              // bitmask: Global | Run | Serve | List | Config
    std::function<void(CliConfig&, const char*)> apply;  // or a tagged switch
};
inline const std::vector<CliFlag>& cliFlags();
```

From `cliFlags()` derive:
- the getopt `longOpts[]` + short-opt string,
- the help text (filtered by `scope`),
- the `applyConfig` mapping (flags that also have config-file keys).

This removes drift class #2 entirely. `list`'s special-case parser is deleted —
its flags join the registry with `scope=List`, and the dispatcher routes
`list` args through the same `parseArgs`.

### Precedence resolver (removes problem #7)

One function, one documented order:

```
explicit CLI flag  >  session metadata  >  agent.yml  >  config file  >  built-in default
```

Replace the scattered `providerSet/modelSet/providerFromSession/...` booleans
with a single `FlagSource` enum per overridable field.

## Migration (incremental, build-verified each step)

- **P1 — extract `cli_types.hpp`.** Move `CliConfig` out of `options.hpp`;
  group fields into `ProviderOpts/SessionOpts/ServerOpts/RenderOpts/DebugOpts`.
  No behavior change. (Low risk, big readability win.)
- **P2 — fix help drift.** Regenerate the provider list in help from
  `providers::availableProviders()` instead of a hardcoded string. (Tiny, immediate value.)
- **P3 — introduce `cli_flags.hpp`** with the registry; migrate *one*
  command's flags (start with `list`) end-to-end (getopt + help + apply).
- **P4 — delete the `list` special-case** in `parseArgs` once `list` flags
  are registry-driven.
- **P5 — split `run.hpp`** into `commands/{run,serve,list,config,completions}.hpp`
  and a thin dispatcher.
- **P6 — precedence resolver** consolidation.
- **P7 — `cli_help.hpp`** generates all 5 help surfaces from the registry.

## Non-goals

- Not replacing getopt with a new dependency (argparse/cli11) — getopt is fine;
  the problem is drift + sprawl, not the parser primitive.
- Not changing flag *names* or *semantics* — this is a refactor, the public
  CLI surface stays stable.
- Not touching the TUI/agent runtime.

## Acceptance criteria

- `./cortex-mk3 --help`, `run --help`, `list --help` all generated from one registry.
- `list --providers` and `--help` agree on the provider set.
- No flag requires editing more than `cli_flags.hpp` (plus its handler).
- Build + `test-iteration-cap` + a manual `--help`/`list` smoke stay green.
