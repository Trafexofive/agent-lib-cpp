"""Classify sub-tasks to appropriate agent types based on domain keywords."""

AGENT_KEYWORDS = {
    'architect': ['design', 'architecture', 'structure', 'pattern', 'interface', 'api design', 'schema'],
    'builder': ['implement', 'build', 'code', 'develop', 'fix', 'refactor', 'migrate', 'deploy'],
    'reviewer': ['review', 'audit', 'inspect', 'quality', 'standards', 'compliance'],
    'researcher': ['research', 'investigate', 'explore', 'analyze', 'benchmark', 'profile', 'evaluate'],
    'tester': ['test', 'validate', 'verify', 'coverage', 'spec', 'acceptance'],
    'orchestrator': ['coordinate', 'pipeline', 'workflow', 'orchestrate', 'manage'],
}

def classify_subtask(sub_task: dict) -> dict:
    """Refine agent type based on task content keywords.
    Only overrides if extractor confidence is very low (< 0.4).
    """
    task_lower = sub_task['task'].lower()
    scores = {}

    for agent, keywords in AGENT_KEYWORDS.items():
        score = sum(1 for kw in keywords if kw in task_lower)
        if score > 0:
            scores[agent] = score

    if scores and sub_task.get('confidence', 0) < 0.4:
        best = max(scores, key=scores.get)
        sub_task['agent'] = best
        sub_task['confidence'] = min(0.7, scores[best] / 4.0)

    return sub_task
