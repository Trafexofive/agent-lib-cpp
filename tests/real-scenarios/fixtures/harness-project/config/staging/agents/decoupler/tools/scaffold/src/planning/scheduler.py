"""Topological sort respecting dependencies."""

def schedule(plan: list[dict]) -> list[dict]:
    """Order plan items by dependencies (topological sort)."""
    if not plan:
        return plan

    # Build adjacency and in-degree
    in_degree = {item['id']: 0 for item in plan}
    adj = {item['id']: [] for item in plan}

    for item in plan:
        for dep in item.get('depends_on', []):
            if dep in adj:
                adj[dep].append(item['id'])
                in_degree[item['id']] += 1

    # Kahn's algorithm
    queue = [item['id'] for item in plan if in_degree[item['id']] == 0]
    order = []

    while queue:
        node = queue.pop(0)
        order.append(node)
        for neighbor in adj.get(node, []):
            in_degree[neighbor] -= 1
            if in_degree[neighbor] == 0:
                queue.append(neighbor)

    # Map back to original items in order
    item_map = {item['id']: item for item in plan}
    return [item_map[oid] for oid in order if oid in item_map]
