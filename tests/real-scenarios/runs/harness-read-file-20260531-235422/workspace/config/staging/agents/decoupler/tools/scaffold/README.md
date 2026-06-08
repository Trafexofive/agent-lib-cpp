# Scaffold Tool

**Runtime:** python3  
**Purpose:** Decomposes user tasks into structured sub-task plans

## Contract
Input: `{"task": "...", "mode": "decompose"}`  
Output: `{"plan": [{"id": 1, "agent": "builder", "task": "...", "depends_on": [0]}, ...], "valid": true}`

## Files
| File | Concern |
|------|---------|
| analysis/extractor.py | Extract distinct concerns from task text |
| analysis/classifier.py | Classify each sub-task by needed agent type |
| planning/dag.py | Build dependency DAG from sub-tasks |
| planning/scheduler.py | Order sub-tasks respecting dependencies |
| validation/integrity.py | Check for cycles, orphans, duplicates |
| main.py | Entrypoint — routes to above |
