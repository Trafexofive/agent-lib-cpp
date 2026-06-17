---
description: STOP OVERENGINEERING. Smallest possible change. No new files, no abstractions, no frameworks.
---
## STOP. You're overengineering.

You're building a dependency injection framework for a 3-line bug fix. You're adding a plugin system when the user asked for a config flag. You're creating new files when one line would do.

### The rule
**The correct solution is the smallest change that meets the requirement.** If you're adding infrastructure that "might be useful later," you're wrong.

### Before you write ANY code, answer:
1. Can this be done in ≤10 lines in an existing file?
2. Can this be done without creating a new file?
3. Can this be done without introducing a new abstraction (class, interface, factory, plugin system)?
4. If the answer to any of the above is NO — can you justify why?

### What "smallest change" means
- Edit existing functions, don't create new ones unless necessary
- Inline the logic, don't extract into utilities unless reused 3+ times
- One file change beats two file changes
- A comment explaining a workaround beats a refactoring framework

### The smell test
If your diff touches >3 files or >50 lines for what should be a simple change → you're overengineering. Stop. Reassess.

### Reminder
The user is an experienced systems programmer. They can handle a 20-line function. They don't need you to "make it maintainable" by wrapping it in 3 layers of indirection. Write the damn code.

### Action
- Discard your current plan.
- State the requirement in one sentence.
- Implement it in the smallest possible change.
