# Full QoL Day

**Goal:** Ship Feel + Composer + Nested in one focused day without scope creep.  
**Effort:** ~6–8 hours including verify.  
**Order matters.**

## Sequence

```
1. Feel pack          (contrast + chrome + pulse + toast)     ~2–3h
2. Composer pack      (keys + submit + history draft)         ~1–2h
3. Nested drill pack  (truthful chrome + micro-slide)         ~2–3h
4. Offline verify + short live pass                          ~1h
```

## Why this order

- **Feel first:** every later change is judged on a readable surface.
- **Composer second:** pure input path; independent of nested.
- **Nested last:** depends on header/status layout from Feel.

## Explicitly deferred (not “full day”)

| Item | Why later |
|------|-----------|
| Multi-line composer | Contract change (Enter vs Ctrl-Enter) |
| Bracketed paste confirm | Needs paste FSM + tests |
| Ask dialog type colors / scroll | Separate medium pack |
| Collapsible huge tool dumps | Touches projection policy |
| Async LLM spinner unlock | Architecture, not QoL detour |
| Help overlay full rewrite | Do after bindings stabilize |

## Definition of done

- [ ] All three pack acceptance checklists checked
- [ ] `make cortex-mk3 test-chat-scene test-ui-model test-ui-view test-perf -j$(nproc)`
- [ ] Manual: graphite + neon; idle; running stream; nested enter/back; Ctrl-U/K/W; toast
- [ ] Optional commit: `feat(ui): tui qol feel+composer+nested` with explicit paths only

## Rollback

Each pack is draw/key localized — revert by pack commit if split, or file list:

- Feel: theme + chat_view + chat_blocks + notice timing
- Composer: agent_scene keys + submit trim
- Nested: agent_scene vm + optional ShellModel slide fields
