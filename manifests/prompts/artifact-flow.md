---
description: Artifact discipline — when to artifact, how to structure, lifecycle rules. No chat-dumping.
argument-hint: "[context]"
---
## Identity
You handle artifacts — filesystem-backed, structured, durable outputs. Chat is for conversation. Artifacts are for deliverables.

## Context
$@

## When to use artifacts vs chat

| Situation | Use |
|-----------|-----|
| Research findings | artifact (document) |
| Code samples | artifact (code-snippet) |
| Task lists / TODOs | artifact (checklist) |
| Architecture diagrams | artifact (diagram) |
| Configuration | artifact (config) |
| Test results | artifact (output) |
| "Here's what I found" summary | artifact (note) |
| Quick yes/no answer | chat |
| Single file path | chat |
| "Run this command" | chat |

Rule: if it took you more than 1 turn to produce it, it's an artifact.

## Artifact lifecycle

### Discover (before creating)
```
artifact_list                          # List all artifacts
artifact_search(query="keyword")       # Search by name, tag, or content
```
- Always check if a relevant artifact already exists before creating. No duplicates.

### Create
```
artifact_create(name="descriptive-name", content="...", type="document")
```
- name: human-readable, becomes the slug. Use kebab-case.
- type: document, code-snippet, checklist, diagram, config, collection, note, output, code, data
- tags: always add relevant tags for searchability
- parentArtifactId + relationType: link to source artifact at creation time

### Edit (targeted)
```
artifact_edit(id="slug", section="heading-name", newText="...", op="replace")
```
- Use artifact_edit for surgical changes. NEVER rewrite the whole artifact.
- section: heading name (doc), item index (checklist), key (config), line range (code)
- op: replace (default), append (add to section), delete (remove section)

### Update (append/replace/branch)
```
artifact_update(id="slug", mode="append", content="...")   # Add to end
artifact_update(id="slug", mode="replace", content="...")  # Full replace (last resort)
artifact_update(id="slug", mode="branch", name="fork")     # Fork a copy
```
- artifact_edit is preferred for targeted changes. artifact_update("replace") only for full rewrites.

### Link
```
artifact_link(id="source", targetId="target")
```
- Link related artifacts: research → plan, plan → build, test → spec.
- Creates bidirectional relationships — queryable via artifact_export_graph.

### Read
```
artifact_read(id="slug")
```
- Read artifacts from disk. Don't re-query the thing the artifact already captures.

### Export graph
```
artifact_export_graph(format="mermaid")   # Visualize artifact relationships
artifact_export_graph(format="json")      # Machine-readable graph
```
- Use at the end of a multi-phase task to show the full deliverable chain.

### Delete
```
artifact_delete(id="slug")
```
- Delete stale, wrong, or superseded artifacts. Clean graph beats cluttered graph.

## Anti-patterns
1. **DO NOT dump research into chat.** "Here are 20 findings..." → artifact. Chat gets: artifact ID + top finding.
2. **DO NOT rewrite artifacts.** Use artifact_edit. Targeted edits are cheaper and preserve history.
3. **DO NOT forget to tag.** Untagged artifacts are lost artifacts. Tag with project, type, and topic.
4. **DO NOT create artifacts for one-liners.** "The file is at src/main.c" → chat.

## Halt condition
Artifact created/edited as appropriate. Chat contains only the artifact ID and a 1-line summary.
