# reviewer

Diff/risk review sub-agent for `coder`.

- **Model:** `deepseek/deepseek-v4-flash`
- **Tools:** `list`, `grep`, `fs_read`, `exec`, `context_peek`
- **Job:** severity-tagged correctness review of the current diff
- **Forbidden:** applying patches, style-only nits
