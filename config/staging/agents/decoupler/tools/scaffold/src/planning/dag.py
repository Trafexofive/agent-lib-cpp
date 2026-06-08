"""Build a dependency DAG from sub-tasks.

Same-agent sequential tasks are independent (can run in parallel).
Different-agent tasks create pipelines (builder → reviewer → builder).
"""
def build_dag(sub_tasks: list[dict]) -> list[dict]:
    plan = []
    prev_build_id = None  # Most recent builder/architect task ID

    for i, st in enumerate(sub_tasks):
        item = {
            'id': i + 1,
            'agent': st.get('agent', 'builder'),
            'task': st['task'],
            'depends_on': [],
        }
        agent = st.get('agent', 'builder')

        # Only add cross-agent dependencies (pipeline handoffs)
        if agent == 'reviewer' and prev_build_id is not None:
            item['depends_on'].append(prev_build_id)

        plan.append(item)

        if agent in ('builder', 'architect'):
            prev_build_id = i + 1

    return plan
