# Archived UI sources

Frozen during the full inkcell migration (path C). **Not linked** by production
entry points. Kept for archaeology and possible salvage — do not re-wire without
a second live consumer.

## scenes/

| File | Why archived |
|------|----------------|
| `dashboard_scene.hpp` | Superseded by `MainScene` hub |
| `help_scene.hpp` | Never registered in `mk3_tui_app.hpp` |
| `inspector_scene.hpp` | Never registered; inspector lives in hub/settings |
| `welcome_scene.hpp` | Alias shim of `MainScene` |

Live scenes: `src/ui/scenes/{main,agent,base}_scene.hpp`.
