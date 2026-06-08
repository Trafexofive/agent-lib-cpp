"""Normalize plan JSON for diff comparison."""

def normalize(plan: list[dict]) -> list[dict]:
    """Ensure all required fields exist with defaults."""
    result = []
    for item in plan:
        normalized = {
            'id': item.get('id', 0),
            'agent': item.get('agent', 'builder'),
            'task': item.get('task', ''),
            'depends_on': item.get('depends_on', [])
        }
        result.append(normalized)
    return result
