# Cortex MK3 inkcell app-specific sbtui fill-in

**App name:** Cortex MK3 AgentShell  
**Class:** hybrid (API-client LLM provider + local filesystem/session state)  
**One-line value prop:** sovereign terminal control plane for protocol-native agent work.

## IA table

| Entity | Relationships | Safe actions | Destructive / gated actions | Canonical views |
|---|---|---|---|---|
| Session | owns turns, transcript, metadata | resume, fork, export, rename | delete/archive session | list, detail |
| Turn | belongs to session, contains protocol events | inspect, copy, replay view | none | timeline item, detail |
| ProtocolEvent | belongs to turn; may reference tool/action/result | jump, expand/collapse, copy | none | timeline row, detail |
| Action | references tool/agent/feed/relic/workflow | inspect schema, copy args | execute mutation through tool preview | action card, detail |
| Result | result of action | inspect output, copy output | none | result card, detail |
| Manifest | owns imported tools/feeds/relics/subagents/workflows | inspect, dry-run, select | none | catalog list, ownership detail |
| Provider/Model | backs agent run | inspect, select for next run | none | picker list, status detail |
| Tool capability | imported by manifest | inspect schema | mutation preview before tool runs where possible | capability list, schema detail |

## Views / transition diagram

```text
AgentTimeline <1> ─┬─ <2> Dashboard
                  ├─ <3> Inspector
                  ├─ <:> CommandPalette (planned)
                  ├─ <?> HelpOverlay
                  └─ ManifestManager / SessionBrowser / ProviderPicker (planned scenes)

Any scene ─ q/Ctrl-C → quit
Any scene ─ route key → target scene with route-change transition in live mode only
```

## Density tiers

| Tier | Width | Cortex behavior |
|---|---|---|
| Wide | ≥160 | nav + timeline + detail/inspector all visible |
| Standard | 100–159 | nav + timeline + compact detail |
| Narrow | 80–99 | single primary pane; nav/detail collapse into route pages |
| Below 80 | centered resize notice |

## State taxonomy

| View | Loading | Populated | Empty | Error | Stale/partial |
|---|---|---|---|---|---|
| AgentTimeline | skeleton rows matching timeline | transcript rows + counts | explicit no-turn message | inline provider/tool error | partial protocol events render normally with failed marker |
| Dashboard | metric skeleton | metrics + recent events | zero counters + no events | error banner | degraded chip if provider retry/backoff |
| Inspector | bridge/event skeleton | raw tail + event log | explicit raw empty | error line with cause | partial event log visible |
| Help | static | key groups | n/a | n/a | n/a |

## Motion rows

| Trigger | Motion | Duration | Snapshot behavior |
|---|---|---|---|
| route/page change | content slide-in offset | 10 ticks | resolved end-state |
| live stream token/protocol event | data-driven line insertion only | tied to event | final state only |
| provider running | status glyph is true state, not idle decoration | while operation active | final state |

No idle motion. No decorative scanlines/pulses.

## Tokens

Uses local Cortex tokens mapped to sbtui semantic set:

- `base_bg`: #05070c
- `panel_bg`: #080b12
- `panel_2`: #0d1320
- `panel_3`: #122034
- `dim`: #748098
- `text`: #d7deea
- `bright`: #eff6ff
- `cyan`: #5adcff
- `green`: #65e39a
- `amber`: #f5b950
- `red`: #ff6b7a

## Deviations

None intended. Any visual effect must map to the motion table above.
