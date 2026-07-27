# Animation Budget

Policy for the QoL detour so motion stays product, not carnival.

## Levels

### Subtle (default / recommended)

| Element | Motion | Rate | When |
|---------|--------|------|------|
| Live spinner | existing braille | ~12.5 fps | `running` |
| Cursor blink | existing | ~530ms | composer focused |
| Footer accent | **new** soft pulse | ~1–1.5 Hz | `running` only |
| Toast notice | **new** fade/dim ladder | 200–300ms | notice set/clear |
| Field shaders | existing | slow | settings-enabled only |
| Pill page slide | existing | section change | hub only |
| Cmd palette | existing scale/fade | open/close | palette only |
| Workflow edges | existing pulse | run live | manifests canvas |

**No** continuous selection pulse, **no** full-frame sparkle, **no** per-token flash.

### Showy

Subtle **plus**:

| Element | Motion | Notes |
|---------|--------|-------|
| Selection | bg pulse while timeline focused | stop while `running` |
| Nested enter/back | 2–4 frame horizontal slide | one-shot |
| Toast | slightly longer fade + accent tick | still <500ms |

### Minimal

- Contrast/chrome only
- Keep existing spinner/cursor/field/pill/palette
- **No** new pulse/fade/slide

## Hard rules

1. **Motion is style-only** — never change layout geometry every frame (except one-shot slide offset).
2. **Reuse clocks** — `nowMs` / `gfx::nowSeconds()`; no extra threads.
3. **Respect reduce-motion later** — leave a single `bool reduceMotion` hook in theme/prefs (default false); wire if easy.
4. **Perf:** any new anim must not break `test-perf` cached transcript budget.
5. **Streaming:** do not animate per token; animate chrome at frame rate only.

## Implementation sketch

```text
pulse01(nowMs, periodMs) -> 0.5 + 0.5*sin(2π * nowMs/periodMs)
lerpStyle(idleAccent, liveAccent, pulse01)  // accent only
```

Toast:

```text
noticeBornMs, noticeText
age = now - born
if age < fadeIn: dim rising
elif age < hold: full
elif age < hold+fadeOut: dim falling
else: clear notice
```
