# Cortex UI layout (inkcell product)

Convention target: `libs/inkcell/examples/inkcell-tetris/src/` and aart.

```
src/ui/
  app/          assembly, runtime, launcher, turn, REPL
  scenes/       inkcell Scene subclasses (hub, agent, workflow, tool, relic)
  views/        draw-only surfaces (shell, timeline) — no key policy
  components/   reusable chrome (pills, palette, chips, workflow widgets)
  chat/         transcript / composer / ask / footer (chat-domain, not generic widgets)
  model/        state, reducers, timeline, dashboard — no Surface draws
  theme/        Cortex theme pack (product-local; not inkcell catalog)
  gfx/          field-raster, shaders, blit cache
  layout/       density + page geometry
  bridge/       Agent ↔ UI events (ask, protocol)
  text/         sanitize
  assets/       glyphs
  _archive/     frozen; not linked
```

## Rules

1. **Scenes own keys.** Views draw. Models mutate. Do not put `handle_key` in a view.
2. **No product domain in `include/inkcell`.** Hub/palette/theme stay here until two consumers.
3. **Clock.dirty / skip_idle.** Mark on keys, wake, resize, real motion — not every frame.
4. **Ctrl-U / Ctrl-K / Ctrl-W** while composer focused = TextArea line-edit. Fine-scroll only when unfocused / timeline focus.
5. **Do not mass-rename** to match tetris in one commit. Move one leaf at a time with a green build.

Live scenes today: `main_scene`, `agent_scene`, `base_scene`, `workflow_scene`, `tool_scene`, `relic_scene`.
