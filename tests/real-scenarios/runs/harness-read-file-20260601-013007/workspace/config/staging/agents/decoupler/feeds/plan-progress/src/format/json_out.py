#!/usr/bin/env python3
"""Format plan state for display."""
import json
import sys

def format_state(state: dict) -> str:
    tasks = state.get('tasks', [])
    lines = []
    for t in tasks:
        status_icon = {'pending': '○', 'active': '◉', 'done': '✓', 'blocked': '✗'}.get(t.get('status', ''), '?')
        agent = t.get('agent', '?')
        desc = t.get('task', '?')
        lines.append(f"  [{t.get('id', '?')}] {status_icon} [{agent}] {desc}")
    return '\n'.join(lines)

if __name__ == '__main__':
    data = json.loads(sys.stdin.read()) if not sys.stdin.isatty() else {}
    print(format_state(data))
