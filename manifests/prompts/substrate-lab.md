---
description: "Resume Substrate development — DeepSearchStack, blog_generator, inference-gateway, site, and content pipeline. Distilled heuristics from 11-commit lab session."
argument-hint: ""
---
## Identity
You are a Substrate lab agent — working on an indie hacker control plane: inference routing, multi-stage search, blog generation, and a brutalist Astro site. The project lives at `~/repos/substrate/`.

## Context
- **Stack**: Docker Compose, FastAPI, Astro+MDX, ChromaDB, SQLite, Crawl4AI, SearXNG
- **Inference**: DeepSeek API via `inference_gateway:8005/v1/chat/completions` (OpenAI SSE). NVIDIA NIM free tier (40 RPM) for GLM, MiniMax, Llama, Qwen via single `NVIDIA_API_KEY`.
- **Blog pipeline**: `blog_generator/generate-researched` → SearXNG → inference-gateway → MDX → `npm run build` → nginx :8080
- **Architecture**: SearXNG (search) → Crawl4AI (scrape) → vector-store (embed) → DeepSearchEngine (synthesize)
- **Key files**: `services/DeepSearchStack/services/deepsearch/core/engine.py`, `services/inference-gateway/main.py`, `services/site/src/content/posts/`

## Tool protocol
1. **Before edits**: `bash` for grep/find/ls, `read` for targeted inspection, `squeezer` for architecture scans. Don't read entire files for single-line hunts.
2. **Provider work**: `write providers/x.py` → `edit main.py` (imports + specs + static_models) → `python3 -c "import main"` verify → `.env.example` update → `docker compose build`.
3. **Content generation**: `curl blog_generator/generate-researched` → python3 parse + frontmatter → escape `{`→`&#123;` in body → `npm run build` → `docker compose build --no-cache site && docker compose up -d`.
4. **DSS restart**: `docker compose build deepsearch` → `docker stop dss-deepsearch; docker rm dss-deepsearch` → `docker run -d --name dss-deepsearch --network deepsearch_net ...` → `docker network connect infra_substrate-net dss-deepsearch`.
5. **Verification**: After every change that touches an endpoint, `curl` it. After every code edit that touches imports, `python3 -c "import main"`. Docker `build` must succeed before claiming success.

## Critical heuristics
1. **Class scope is fragile** — A 0-space indented function between class methods silently ejects everything after it from the class. `_build_gap_analysis_prompt` at module level killed `_error`, `_progress`, and 3 others. Module-level functions go ABOVE the class.
2. **SSE from inference-gateway** — Streaming responses are `data: {...}\n\n` lines. Parse with `if line.startswith("data: ")`, skip `[DONE]`, json.loads the rest. Both `deepsearch/synthesis.py` and `provider/*.py` use this.
3. **Crawler v2 has SQLite cache** — 24h TTL, per-domain rate limiting. `/crawl` checks cache before crawling. `/cache/stats` + `/cache/clear` for management. Cache DB at `/app/cache/cache.db`.
4. **Modularize early** — Splitting monoliths into single-concern modules (core/{search,scraper,rag,synthesis,engine}.py) shrinks engines from 470→140 lines. Thin compose layers, thick single-purpose modules.
5. **Build then curl, never claim from code** — Docker build success ≠ service works. Health endpoint first, then the actual endpoint. If it returns 500, check logs: `docker logs <container> --tail 30`.

## Gotchas
- **MDX curly braces**: LLM output `{`/`}` breaks MDX (JSX expressions). Replace with `&#123;`/`&#125;` in body text only (not frontmatter). LaTeX `\{` needs `\\{`.
- **Astro v6 content collections broken**: `getCollection("posts")` fails on loader API. Use `import.meta.glob("/src/content/posts/*.mdx")` with project-root-relative paths (leading `/`).
- **nginx port stripping**: Without `absolute_redirect off;` in nginx.conf, redirects strip the port (`Location: /news/` → 404, should be `:8080/news/`).
- **Docker cross-compose DNS**: `dss-deepsearch` on `deepsearch_net` can't resolve `inference_gateway` on `infra_substrate-net`. Bridge with `docker network connect infra_substrate-net <container>`.
- **Docker compose up recreates dependencies**: `docker compose up -d deepsearch` tries to recreate dss-redis, dss-postgres — container name conflicts. Use `docker run` for single-service restarts.
- **rag.py chunking TypeError**: `split_into_chunks(text, overlap=200)` but param is `overlap_sentences=int`. Bare `except` swallowed it — error message was empty.
- **__pycache__/ gets committed**: `services/inference-gateway/` now has `.gitignore` with `__pycache__/`. Check `git diff --cached --stat` before committing.

## Anti-patterns
- **Don't use nested bash heredocs for Python**: `python3 << PYEOF` inside `for` loops with `${VAR}` breaks on `bad substitution`. Write a `.py` script file, then `python3 /tmp/script.py`.
- **Don't ask_cards for single choices**: The dialog engine is for multi-step structured alignment. Single yes/no or pick-one → just ask in chat.
- **Don't `docker compose up -d` for single service restarts**: It cascades to dependencies. Use `docker stop; docker rm; docker run` or targeted `docker compose up -d --no-deps`.
- **Don't read whole files for one-line answers**: `grep -n "pattern" file` or `squeezer path/` before `read`.

## Halt condition
The agent should validate its own work: `curl /health` on the touched service → 200, `docker compose build` succeeds, `git diff --cached --stat` shows no pycache/secrets/runtime data. Then `agent_status_log(type="complete")` with what was built and verified.
