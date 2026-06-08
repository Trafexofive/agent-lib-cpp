"""Validate a decomposition plan for structural correctness."""

def validate(plan: list[dict]) -> tuple[bool, list[str]]:
    """Check plan for cycles, orphans, missing agents."""
    warnings = []
    ids = {item['id'] for item in plan}

    if not plan:
        return False, ["Empty plan"]

    # Check all dependency references exist
    for item in plan:
        for dep in item.get('depends_on', []):
            if dep not in ids and dep != 0:
                warnings.append(f"Task {item['id']} depends on non-existent task {dep}")

    # Check for self-dependency
    for item in plan:
        if item['id'] in item.get('depends_on', []):
            warnings.append(f"Task {item['id']} depends on itself")

    # Check for cycles (DFS)
    visited = set()
    rec_stack = set()
    has_cycle = False

    def dfs(node: int) -> bool:
        nonlocal has_cycle
        visited.add(node)
        rec_stack.add(node)
        for item in plan:
            if item['id'] == node:
                for dep in item.get('depends_on', []):
                    if dep not in visited:
                        if dfs(dep):
                            return True
                    elif dep in rec_stack:
                        has_cycle = True
                        return True
        rec_stack.discard(node)
        return False

    for item in plan:
        if item['id'] not in visited:
            dfs(item['id'])

    if has_cycle:
        warnings.append("Plan contains a dependency cycle")

    # Check all have agent types
    for item in plan:
        if 'agent' not in item:
            warnings.append(f"Task {item['id']} has no agent type assigned")

    valid = len(warnings) == 0
    return valid, warnings
