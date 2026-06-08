# Decomposition Patterns

Common patterns for task decomposition. The decoupler agent loads these for unknown domains.

## Pattern: Feature Implementation
```
1. [architect] Design feature architecture
2. [builder] Implement core logic
3. [builder] Implement tests
4. [reviewer] Review implementation
```

## Pattern: Refactoring
```
1. [architect] Analyze current codebase structure
2. [builder] Extract interfaces
3. [builder] Migrate implementation
4. [reviewer] Verify behavior preservation
5. [tester] Run regression suite
```

## Pattern: Bug Fix
```
1. [researcher] Reproduce and isolate bug
2. [architect] Identify root cause
3. [builder] Implement fix
4. [tester] Verify fix, run regression
```

## Pattern: API Integration
```
1. [researcher] Study third-party API docs
2. [architect] Design integration layer
3. [builder] Implement client + error handling
4. [builder] Add tests with mocks
```

## Pattern: Performance Optimization
```
1. [researcher] Profile and identify bottlenecks
2. [architect] Design optimization strategy
3. [builder] Implement changes
4. [tester] Benchmark before/after
```

## Agent Types Reference
| Agent | Responsibility |
|-------|---------------|
| architect | Design, structure, scaffolding |
| builder | Implementation, coding |
| reviewer | Code review, validation |
| researcher | Investigation, data gathering |
| tester | Test writing, verification |
| artificer | Specialized tool creation |
| orchestrator | Multi-agent coordination |
