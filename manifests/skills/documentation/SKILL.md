---
name: documentation
description: >
  Documentation discipline: README, man pages, API docs, changelogs, architecture docs.
  When to write what, format conventions, keep docs with code. Load for sessions
  where documentation is expected or generated.
---

# Documentation Skill

## What to write, when

| Artifact | When | Format |
|----------|------|--------|
| README.md | Every project. First thing written. | Markdown |
| man page | CLI tools. `man <command>` support. | troff/man |
| API docs | Libraries with public interfaces. | Doc comments in code |
| CHANGELOG.md | Every release. | Keep a Changelog format |
| ARCHITECTURE.md | Multi-module projects. | Markdown diagram |
| CONTRIBUTING.md | Open source projects. | Markdown |
| Inline comments | Non-obvious WHY, not WHAT. | `// reason` not `// does X` |

## README.md template

```markdown
# Project Name

One-line description.

## Quick Start
```bash
git clone ...
cd project
make
./program --help
```

## What is this?
[2-3 paragraphs explaining the problem and solution]

## Usage
[Common workflows. Not exhaustive — point to man page or --help.]

## Building
[Prerequisites, build commands, options]

## Testing
[How to run tests]

## Architecture
[Link to ARCHITECTURE.md if it exists. Otherwise brief overview.]

## License
[License name]
```

## man page template

```troff
.TH PROGRAM 1 "DATE" "VERSION" "User Commands"
.SH NAME
program \- one-line description
.SH SYNOPSIS
.B program
[\fIoptions\fR] \fIfile\fR...
.SH DESCRIPTION
...
.SH OPTIONS
.TP
.B \-h, \-\-help
Show help and exit.
.SH EXAMPLES
...
.SH SEE ALSO
...
.SH AUTHOR
...
```

## API documentation (doc comments)

C/C++:
```c
/**
 * Brief description.
 *
 * @param x  Description of x
 * @return   Description of return value
 * @error    When this fails
 */
int do_thing(int x);
```

Python (docstrings):
```python
def do_thing(x: int) -> int:
    """Brief description.

    Longer description if needed.

    Args:
        x: Description of x.

    Returns:
        Description of return value.

    Raises:
        ValueError: When x is negative.
    """
```

## CHANGELOG.md (Keep a Changelog format)

```markdown
# Changelog

## [0.2.0] - 2026-05-12
### Added
- Feature X by @user

### Changed
- Refactored Y for performance

### Fixed
- Bug Z where edge case caused crash

### Removed
- Deprecated function A
```

## Rules

1. **Docs live with code.** README, CHANGELOG, ARCHITECTURE are in the repo root. API docs are in the source files.
2. **Write for the next person.** Assume they know the language, not the project.
3. **Examples over explanations.** Show a working command before explaining it.
4. **Keep it current.** Update docs when behavior changes. Stale docs are worse than no docs.
5. **No tutorials in comments.** Comments explain WHY. Functions/types explain WHAT. Tutorials are separate documents.

## Anti-patterns
1. **DO NOT write `// increment x` above `x++`.** Comments explain WHY it's incrementing, not THAT it's incrementing.
2. **DO NOT write a README that's just the project name.** If you don't have 2 minutes for a README, you don't have 2 hours for a user to figure it out.
3. **DO NOT let docs go stale.** When you change behavior, grep for the old behavior in docs and update.
4. **DO NOT document internal implementation.** Document the public API. Internal implementation changes — the contract shouldn't.
