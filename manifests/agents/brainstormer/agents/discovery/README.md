# Discovery — read-only recon sub-agent

Nested specialist under `brainstormer`. Scopes, maps, and surfaces information
across the local filesystem and the public web — and **never mutates anything**.

## Capability surface

| Tool | Kind | Backend |
|---|---|---|
| `web_search` | custom | SearXNG (`SEARXNG_URL`) → DuckDuckGo HTML |
| `web_open_url` | custom | `requests` + trafilatura (bs4 fallback) |
| `search_image_by_text` | custom | SearXNG images → Wikimedia Commons |
| `search_image_by_image` | custom | local dHash fingerprint + optional corpus scan / SearXNG |
| `get_data_source_desc` | custom | catalog in `tools/_shared/src/data_sources.json` |
| `get_data_source` | custom | arxiv · crossref · openalex · wikipedia · yahoo · github |
| `ipython` | custom | restricted in-process Python (numpy + stdlib data modules) |
| `list` / `grep` / `tree` / `squeezer` / `fs_read` / `context_peek` | builtin | filesystem inspection |

## Layout

```
discovery/
├── agent.yml
├── system.md · persona.md
├── tools/
│   ├── _shared/src/            # shared library (not an importable tool)
│   │   ├── common.py           # http, search, extraction, image fingerprinting
│   │   ├── data_sources.py     # data-source adapters + registry
│   │   └── data_sources.json   # catalog (rendered by get_data_source_desc)
│   └── <tool>/{tool.yml, src/main.py}
```

Tool scripts follow the runtime contract: JSON params via `argv[1]` (a temp-file
path), a single JSON document on stdout, non-zero exit on failure. Each `main.py`
bootstraps `tools/_shared/src` onto `sys.path` using `__file__`, so execution is
independent of the process CWD.

## Read-only guarantees

- No `exec` / `fs_write` tools imported.
- `ipython` runs in a restricted namespace: `open`, `os`, `subprocess`,
  `__import__`, `exec`, `eval` are all unavailable; a `SIGALRM` timeout caps
  runtime. numpy and data modules are pre-imported for local analysis only.
- Every network adapter is a GET; no cookies persisted, no writes, no mutation.

## Environment

| Var | Effect | Default |
|---|---|---|
| `SEARXNG_URL` | self-hosted SearXNG JSON API (enables SearXNG search, image, reverse) | unset → DuckDuckGo / Wikimedia fallbacks |
| `GITHUB_TOKEN` | raises GitHub search rate limit | unset → 10 req/min |
| `DISCOVERY_TIMEOUT` | per-request HTTP timeout (s) | `30` |
| `DISCOVERY_USER_AGENT` | UA string | `cortex-discovery/1.0` |

## Notes

- The tool runtime caps a single script invocation at **30 s** (`runShell` in
  `src/tools/tool.hpp`); each web query is one invocation, so a multi-search
  mission spreads across many 30 s budgets rather than one long call.
- No `relics/` are registered: the data sources are external public REST APIs
  queried directly, not local managed services. Add a SearXNG relic here if you
  stand one up locally.
