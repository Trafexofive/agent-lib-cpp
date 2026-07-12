# Cortex Streaming and Parser Performance

Measured on the project development machine with `-O2`, 2026-07-12. These are Cortex-side results; Inkcell engine internals were intentionally not changed.

## Before / after

| Workload | Before | After | Improvement |
|---|---:|---:|---:|
| Parser: 256 KiB `<response>`, 1-byte chunks | 322 ms | 7–9 ms | ~40× |
| Parser: 64 KiB `<action>`, 1-byte chunks | 1272 ms | 8 ms | ~159× |
| Parser: 128 KiB `<action>`, 1-byte chunks | 5073 ms | 12–13 ms | ~400× |
| UI reducer: 4000 response mutations, one drain | 319 ms | 40 ms | ~8× |
| Cached transcript: 3000 lines, 200 frames | not cached | 9 ms | versioned reuse |

Absolute timings are machine-specific. `make test-perf` uses deliberately wider budgets to detect algorithmic regressions without pretending to be a portable microbenchmark standard.

## Parser changes

- Response closing-tag scans start from the unread suffix, not the response beginning.
- Non-response closing-tag scanner persists depth/string/escape state across token feeds.
- Split opening/closing markers remain buffered and are reconsidered on the next token.
- Consumed parser buffer prefixes compact at 64 KiB.
- XML attribute parsing is linear and allocation-light; no per-tag regex construction.
- Final stray-tag stripping is a linear scan rather than `std::regex_replace`.
- CANON semantics, nested action protection, `<think>` compatibility, forged-result rejection, and split-boundary handling remain covered by parser tests.

## Streaming/UI changes

- Provider callbacks publish at most once per 16 ms frame interval, plus mandatory final flush.
- Token and protocol changes use one `publishMany` queue lock/eventfd wake per flush.
- Previous protocol state updates only changed indices; the whole vector is not recopied.
- A drained event batch triggers at most one transcript rebuild.
- Status-only, hidden raw-token, and progress-only events do not invalidate transcript data.
- AgentBridge snapshots no longer retain unbounded Token/Protocol payload history.
- AgentScene references transcript storage instead of copying it every frame.
- Wrapped transcript and semantic block metadata are version/width cached.

## Permanent regression gates

```bash
make test-perf
```

Current budgets:

| Gate | Budget |
|---|---:|
| 256 KiB response / 1-byte chunks | 250 ms |
| 128 KiB action / 1-byte chunks | 300 ms |
| 1000 protocol mutations / one drain | 100 ms |
| 3000-line cached transcript / 200 frames | 150 ms |

The reducer gate also asserts:

- indexed response integrity;
- one view rebuild per drained batch;
- no Token/Protocol retention in bridge snapshots.

## Deferred to Inkcell work

- engine polling/tick policy;
- terminal frame diff algorithm;
- Surface allocation strategy;
- key decoding/input completeness;
- engine-level dirty rectangles.

Those require Inkcell’s own benchmark suite rather than speculative Cortex-side patches.
