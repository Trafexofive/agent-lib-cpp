"""Extract distinct concerns from a task description."""
import re

# Patterns: (regex, verb_group, agent_type)
PATTERNS = [
    # Multi-module: "Split X into Y: a, b, c" or "Break X into modules: a, b, c"
    (r'(?:split|break|decouple|separate|refactor).*?(?:into|:)\s*(.+)', "decompose_list"),
    # Single action patterns
    (r'(?:create|build|implement|develop|write|code)\s+(?:a\s+)?(\w[\w\s]+)', 'builder'),
    (r'(?:design|architect|structure|plan|scaffold)\s+(?:a\s+)?(\w[\w\s]+)', 'architect'),
    (r'(?:review|audit|inspect|check|verify)\s+(?:a\s+)?(\w[\w\s]+)', 'reviewer'),
    (r'(?:research|investigate|explore|find|study)\s+(?:a\s+)?(\w[\w\s]+)', 'researcher'),
    (r'(?:test|validate)\s+(?:a\s+)?(\w[\w\s]+)', 'tester'),
    (r'(?:fix|resolve|debug|patch)\s+(?:a\s+)?(\w[\w\s]+)', 'builder'),
    (r'(?:deploy|release|ship|publish)\s+(?:a\s+)?(\w[\w\s]+)', 'builder'),
    (r'(?:configure|setup|install|provision)\s+(?:a\s+)?(\w[\w\s]+)', 'builder'),
    (r'(?:document|write docs|explain)\s+(?:a\s+)?(\w[\w\s]+)', 'architect'),
    (r'(?:migrate|convert|transform)\s+(?:a\s+)?(\w[\w\s]+)', 'builder'),
]

MODULE_AGENT_MAP = {
    'config': 'builder', 'csv_parser': 'builder', 'api_client': 'builder',
    'reporter': 'builder', 'archiver': 'builder', 'email': 'builder',
    'logger': 'builder', 'validator': 'tester', 'models': 'architect',
    'utils': 'builder', 'cli': 'builder', 'pipeline': 'architect',
    'auth': 'builder', 'db': 'builder', 'cache': 'builder',
    'parser': 'builder', 'serializer': 'builder', 'handler': 'builder',
    'service': 'builder', 'controller': 'builder', 'middleware': 'builder',
    'notification': 'builder', 'scheduler': 'builder', 'queue': 'builder',
}


def extract_concerns(task: str) -> list[dict]:
    """Break a task string into sub-tasks with detected agent types."""
    task_lower = task.lower()

    # Check for decompose_list pattern first (multi-module tasks)
    m = re.search(r'(?:split|break|decouple|separate|refactor).*?(?:into|:)\s*(.+)', task_lower)
    if m:
        rest = m.group(1).strip()
        rest = re.sub(r'^(?:separate\s+)?modules?\s*:\s*', '', rest)
        modules = re.split(r'\s*,\s*', rest)
        sub_tasks = []
        for mod in modules:
            mod = mod.strip().rstrip('.')
            if not mod:
                continue
            agent = MODULE_AGENT_MAP.get(mod.replace('-', '_').lower(), 'builder')
            sub_tasks.append({'task': f"Extract {mod} module", 'agent': agent, 'confidence': 0.8})
        if sub_tasks:
            return sub_tasks

    # Find ALL action patterns in the task (not just first match)
    sub_tasks = []
    seen_positions = set()
    for pattern, agent_type in PATTERNS:
        if agent_type == "decompose_list":
            continue  # already handled above
        for match_obj in re.finditer(pattern, task_lower):
            start = match_obj.start()
            end = match_obj.end()
            # Avoid overlapping matches
            if any(start <= p < end or p <= start < p_end for p, p_end in seen_positions):
                continue
            seen_positions.add((start, end))
            sub_tasks.append({
                'task': match_obj.group(1).strip().capitalize(),
                'agent': agent_type,
                'confidence': 0.6,
            })

    if not sub_tasks:
        sub_tasks.append({'task': task.strip(), 'agent': 'builder', 'confidence': 0.3})

    return sub_tasks
