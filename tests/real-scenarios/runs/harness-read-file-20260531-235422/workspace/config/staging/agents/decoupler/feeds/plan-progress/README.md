# Plan Progress Feed

**Runtime:** bash  
**Purpose:** Tracks decomposition plan state — reports pending, active, done, blocked counts

## Output
```json
{"pending": 3, "active": 0, "done": 1, "blocked": 0, "total": 4, "progress_pct": 25}
```

## How It Works
Reads a `.plan-state.json` file written by the scaffold tool after plan creation and updated as sub-agents complete.
