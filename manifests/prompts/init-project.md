---
description: Project onboarding pipeline. Map structure, squeezer key modules, produce architecture doc and onboarding guide.
argument-hint: "[repo-path]"
---
## What this is
Given a repo, produce two artifacts: an architecture map and an onboarding guide. Automate the "WTF does this codebase do?" experience.

## Target
$@ (default: current directory)

## Protocol

### Phase 1: Structural scan
```
autonomous_discover(query="entry points main modules", maxFiles=10) →
bash(find . -type f -name "*.py" -o -name "*.c" -o -name "*.rs" -o -name "*.ts" | head -30) →
bash(tree -L 2 -I 'node_modules|.git|build|dist|target|__pycache__')
```
Map: directory structure, entry points, config files, build system, test structure.

### Phase 2: Interface map (parallel squeezer)
```
# Spawn sub-agents to squeezer key directories/modules:
spawn_agent(name="map-src", prompt="squeezer src/ — produce interface map: exports, imports, key types")
spawn_agent(name="map-tests", prompt="squeezer tests/ — produce test structure: frameworks, patterns, coverage areas")
spawn_agent(name="map-config", prompt="squeezer config files — extract settings, env vars, defaults")
→ harvest_completed
```

### Phase 3: Dependency and tooling scan
```
bash: look for Makefile, CMakeLists.txt, Cargo.toml, go.mod, package.json, pyproject.toml
bash: grep for external dependencies
bash: check git log --oneline -20 for recent activity and commit style
```

### Phase 4: Produce architecture map
```
artifact_create(name="architecture-$PROJECT", type="document", content={
  project: name,
  language: [...],
  build_system: ...,
  entry_points: [...],
  directory_structure: tree_output,
  key_modules: [{ path, purpose, exports, dependencies }],
  data_flow: "how data moves through the system",
  external_dependencies: [...],
  test_structure: { framework, location, coverage },
  config: { files, key_settings, env_vars }
})
```

### Phase 5: Produce onboarding guide
```
artifact_create(name="onboarding-$PROJECT", type="document", content={
  what_is_this: "one-paragraph summary",
  prerequisites: [...tools needed to build/run],
  quick_start: "commands to clone, build, run",
  development_workflow: "edit → lint → test → commit workflow",
  key_concepts: [...concepts a new dev needs to understand],
  footguns: [...common mistakes],
  where_to_start_reading: [ordered list of files to read first]
})
```

### Phase 6: Link and report
```
artifact_link(architecture → onboarding)  # bidirectional
agent_status_log: "Onboarded $PROJECT. Architecture: $ID1. Guide: $ID2."
```

## Anti-patterns
1. **DO NOT read every file.** Squeezer gives you the interface map without the implementation.
2. **DO NOT guess the purpose.** Read the README, check git log, look at the entry points.
3. **DO NOT skip the onboarding guide.** Architecture map is for reference. Onboarding guide is for humans.
4. **DO NOT produce one artifact.** These serve different audiences — keep them separate and link them.
