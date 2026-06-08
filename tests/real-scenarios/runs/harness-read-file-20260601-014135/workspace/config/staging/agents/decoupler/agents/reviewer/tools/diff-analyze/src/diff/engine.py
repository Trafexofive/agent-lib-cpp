"""Compute structural diffs between two plan versions."""

def diff_plans(plan_a: list[dict], plan_b: list[dict]) -> dict:
    """Compare plan_b against plan_a baseline."""
    ids_a = {t['id']: t for t in plan_a}
    ids_b = {t['id']: t for t in plan_b}

    added = [t for t in plan_b if t['id'] not in ids_a]
    removed = [t for t in plan_a if t['id'] not in ids_b]

    modified = []
    for t in plan_b:
        if t['id'] in ids_a:
            orig = ids_a[t['id']]
            if (orig.get('task') != t.get('task') or
                orig.get('agent') != t.get('agent') or
                orig.get('depends_on') != t.get('depends_on')):
                modified.append({
                    'id': t['id'],
                    'changes': {
                        k: {'from': orig.get(k), 'to': t.get(k)}
                        for k in ['task', 'agent', 'depends_on']
                        if orig.get(k) != t.get(k)
                    }
                })

    # Check reordering
    order_a = [t['id'] for t in plan_a]
    order_b = [t['id'] for t in plan_b]
    reordered = order_a != order_b

    return {
        'added': added,
        'removed': removed,
        'modified': modified,
        'reordered': reordered,
        'total_changes': len(added) + len(removed) + len(modified) + (1 if reordered else 0)
    }
