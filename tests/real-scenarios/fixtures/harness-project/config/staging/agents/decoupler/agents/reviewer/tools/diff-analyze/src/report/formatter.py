"""Format diff results for agent consumption."""

def format_diff(diff: dict) -> str:
    """Return human-readable diff summary."""
    lines = [f"Diff: {diff.get('total_changes', 0)} changes"]

    for added in diff.get('added', []):
        lines.append(f"  + [{added['id']}] [{added.get('agent', '?')}] {added.get('task', '?')}")

    for removed in diff.get('removed', []):
        lines.append(f"  - [{removed['id']}] [{removed.get('agent', '?')}] {removed.get('task', '?')}")

    for mod in diff.get('modified', []):
        changes = mod.get('changes', {})
        detail = ', '.join(f"{k}: {v['from']} → {v['to']}" for k, v in changes.items())
        lines.append(f"  ~ [{mod['id']}] {detail}")

    if diff.get('reordered'):
        lines.append("  ⇅ Reordered")

    return '\n'.join(lines)
