#!/usr/bin/env bash
# Plan progress feed — reports sub-task completion stats

STATE_FILE="${PLAN_STATE_FILE:-/tmp/decoupler-plan-state.json}"

if [ -f "$STATE_FILE" ]; then
    python3 -c "
import json
try:
    with open('$STATE_FILE') as f:
        state = json.load(f)
    tasks = state.get('tasks', [])
    pending = sum(1 for t in tasks if t.get('status') == 'pending')
    active = sum(1 for t in tasks if t.get('status') == 'active')
    done = sum(1 for t in tasks if t.get('status') == 'done')
    blocked = sum(1 for t in tasks if t.get('status') == 'blocked')
    total = len(tasks)
    pct = int((done / total) * 100) if total > 0 else 0
    print(json.dumps({
        'pending': pending,
        'active': active,
        'done': done,
        'blocked': blocked,
        'total': total,
        'progress_pct': pct
    }))
except Exception as e:
    print(json.dumps({'pending': 0, 'active': 0, 'done': 0, 'blocked': 0, 'total': 0, 'progress_pct': 0}))
"
else
    echo '{"pending": 0, "active": 0, "done": 0, "blocked": 0, "total": 0, "progress_pct": 0}'
fi
