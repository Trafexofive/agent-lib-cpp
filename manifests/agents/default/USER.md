# USER.md — Operator Context Spec (Global)

> Load at session start. Ground truth about who you're talking to.
> Adjust defaults accordingly. Don't ask about anything covered here.
> If something here is stale or contradicted in-session, in-session wins.

---

## 0. IDENTITY

**Handle:** CleverLord
**GitHub:** Trafexofive
**Role:** Systems programmer. Infrastructure engineer. FOSS maximalist. Solo indie hacker.
**Location:** Morocco
**OS:** Arch Linux — bare metal, no cloud, no vendor lock-in, no exceptions.
**Philosophy:** If you don't understand it at the system level, you don't control it.
**Origin story:** Maternal grandfather was a polymath — mechanics, electronics, physics, literature, hands-on everything. That's the template.
**Influences:** Sam Zeloof, Ben Eater, Fabrice Bellard. Recreational-engineer mentality — build for the love of the craft, not for a roadmap.
**End game:** Found a game studio. Make games great again.

---

## 1. TECHNICAL CONTEXT

### Languages AND OR code

| Language | Proficiency | Notes |
|----------|-------------|-------|
| C | primary | low-level, systems, embedded, interpreters |
| C++ (98/11/13) | primary | prefer older standards unless there's a real reason |
| Python | primary | infra tooling, AI/ML pipelines, TUIs, agents |
| Bash | primary | automation, sysadmin, glue |
| Lua | working | scripting, config (nvim, etc.) |
| Go / TypeScript / GDScript / Kotlin | situational | language-agnostic — use what fits the substrate |
| Makefile | daily | build system of choice unless there's a better reason |
Honestly, it's much more that that but that is besides the point. I have a very broad and deep knowledge of programming and software engineering. I can pick up new languages, frameworks or any skill in life for that matter if time isnts a constrain.

**Language/tech/stack/you-name-it Agnostic** Is the holygrail. Pick the right tool for the job, not the one you like best. Good opportunity to learn, but may be disregarded the second I need stability or performance. You could say I'm a "pramatic perfectionist" If that makes sense.
**Code Quality** to keep it short, I do not play with my code. I like it to be very well thought-out (UX/UI/ or whatnot I do not discriminate), well architected, clean, READABLE, modular, reusable and maintainable. Keeping it very much Data and Test driven and grounded with logic, reasoning and solid, proovable/validatable aproaches and [engineering/...] models. And most important of all. The Code needs to actually work as intented, no stubs, no lazy work AND OR hacky patches ... Im technical and very involved anyways so Ill easily spot it. Im beyond harness engineering at this point, im coding and auditing along my agents (joke). we can keep it goofy and unserious sometimes because we enjoy what we do, but do not mistake, I take this very much serious. this is no longer a hobby or job, this is a way of life.
And Dont forget ABOUT: DUE DILIGENCE (Common developper sense for the win) as well as testing, validation, documentation, verification, and auditing. 

### Environment

```
Shell:      bash / zsh / fish
Editor:     Neovim
Multiplex:  Zellij
Build:      GCC / Clang / Make / Just
VCS:        Git (CLI only)
```

### Infrastructure Stack (self-hosted, zero SaaS dependency)

```
OS:         Arch Linux (bare metal)
Proxy:      Nginx / Traefik (the GOAT)
VPN:        WireGuard
DNS:        AdGuard
Mail:       Postfix + Dovecot + rspamd
Git:        Gitea (self-hosted)
Containers: Docker
Init:       Systemd
DBs:        PostgreSQL, SQLite, Redis, Neo4j
```

### Creative / Media Stack

```
2D raster:  Krita
Vector:     Inkscape
3D:         Blender 4.5 LTS
Shaders:    glslViewer, SHADERed, RenderDoc
Video:      Kdenlive
```

### Emulation Stack

```
Frontend:   Pegasus-fe
Sub-PS2:    RetroArch
Standalone: DuckStation, PCSX2 Qt, RPCS3, Dolphin, CEMU, Flycast, xemu, Lime3DS, PPSSPP
Scraping:   Skyscraper
```

---

## 2. ACTIVE PROJECTS (bloated, self-serving, ongoing)

### Trading / Quant
- **Discretionary trading practice** — structured trading journal template, empirical entry model research. Volume profile as primary analytical language; ICT/SMC concepts mapped back to first-principles equivalents (not taken on faith).
- **Prop firm eval accounts** — 10K one-phase + 5K two-step on MT5. Full mechanical strategy framework built around each account's specific drawdown rules. GMT-based session gates, dual-mode entry triggers, lot size calculator, kill switch panel.
- **PropBuddy** — Python/Textual TUI trading companion. Pawngatchi-style ASCII character reacting to account health, drawdown tracking, session countdowns.
- **XAUUSD combat reference card** — LaTeX print-ready, ICT primitives mapped to GFT account rules.

### Agent Harness / LLM Infra
- **Cortex-Prime-MK3** — XML protocol harness, model output parsed by a state machine. C++ TUI runtime (terminal, keys, history, input, markdown, protocol, grid, renderer) + Python data layer (hierarchical Tool/Relic/Agent/Workflow/Monument entity system). Systematic compliance benchmarks across OpenRouter, Groq, OpenCode — root-caused compliance regressions from harness compression.
- **Adversarial audit system prompts** — infra/prod/security and code-quality domains, PASS/PARTIAL/FAIL grading, AI slop detection as a first-class dimension, terse one-sentence-per-issue output.
- **Multi-agent BUDDY/FORGE/SIGNAL/REACH/LENS** — five-agent specialist team, shared endeavor object model (intent, state, decision log, return type) injected per-agent at runtime. Orchestrator-only operator interface.
- **pi-agent ecosystem** (`@mariozechner/pi-coding-agent` extensions):
  - Web search extension: BM25 ranking, Wayback fallback, SSRF fixes, parallelized fetches.
  - KB Agent: ripgrep search, LLM synthesis, namespace layout. Node.js TUI (`kb-tui.js`), mode FSM (BROWSE/FILES/PREVIEW/SEARCH/CONFIRM/HELP/PI), `marked` + `marked-terminal` + `cli-highlight` rendering, deep-space operator aesthetic, adapters for GitHub/website/local.
  - ask-cards: DAG-based TUI dialog engine, sub-chains, loops, conditional branching.
  - TARDIS Control Board: ~1000-line TS extension, 3-column HUD footer, agent fleet mgmt with DAG deps, tmux-based spawning, autonomous sleep with passive cards.
  - Skill files for session-to-skill conversion and skill creation.
- **Agentic web search harness (ideation)** — sovereign, local-first investigative loop: claim extraction, cross-referencing, contradiction detection, saturation sensing. Autonomous continuous mode: horizon expansion, claim decay patrol, contradiction monitoring, serendipity probe. Stack: SearXNG, trafilatura, ollama, SQLite, Neo4j, Rich/Textual.

### Languages / Compilers
- **Satori** — bytecode interpreter in C (v0.1.0-alpha). Lexer → recursive descent parser → AST → bytecode codegen → stack-based VM. Canon: Crafting Interpreters, Lua 5.1 source, Nystrom's blog, Ghuloum's incremental compiler paper.
- **Smelt** — statically typed language transpiling to clean C. C++98 recursive descent parser. No `let`/`fn`, composition-only (no inheritance), Boehm GC, C-style error handling with `T?` sugar, file-is-a-module, comptime evaluation.

### Platforms
- **Substrate** — "Omni Indie Hacker Control Plane." Self-hosted, declarative, manifest-driven microservices: LLM routing, agentic workflows, DeepSearch stack (DSS), content gen, trend analysis, ops tooling. Makefile orchestration, dynamic stack discovery, context caching. Clients: Go CLI (`subctl`), Python SDK, C++ SDK, Android app. Org model: platforms (multi-component) vs. projects (single-scope), track files as index views.
- **FORGE** — self-hosted orchestration platform, native Android client (Kotlin/Jetpack Compose). Modular gesture binding via TOML — multi-finger swipes, pinch-zoom, hold chords, custom gesture-to-command mappings with plugin action API.
- **Voxtral** — voice agent stack, four Docker microservices (gateway, LLM, STT, TTS) + Java Android client. STT: faster-whisper. TTS: Kokoro-82M. LLM: DeepSeek API proxy with planned local swap (Gemma 4 E4B GGUF Q4_K_M on llama-server). Local-swap path strictly via env vars.
- **coin-sniper** — MEXC listing-snipe bot. ZMQ PUB/SUB, exchange adapter layer, Redis event bus with typed contracts (ListingEvent, TradeEvent, TpConfig). Two-phase detection: 10s announcement scraping, 100-200ms fast-poll on status flip. Refactor path: language-agnostic microservices, abstract `ExchangeAdapter`.
- **Trends TUI** — Node.js terminal dashboard over local microservices (warehouse, Google Trends extractor, aggregator). Panel-based design, thick accent bars, rounded box panels.
- **Droid** — Python system for AI-assisted Kotlin/Jetpack Compose generation. Gradle build daemon, ADB deploy, error-feedback loop, XML mutation format, atomic mutations with snapshot/rollback.

### Tools / Utilities
- **boil** — snippet manager. Textual TUI, vim modal system (NORMAL/SEARCH/CMD), command palette, TagCloud sidebar, syntax-highlighted PreviewPane (one-dark). Cyberpunk web frontend variant (dark void, electric cyan/hot pink, CRT scanline, vim-nav).
- **Shellbase** — Android SSH client. Kotlin/Jetpack Compose, SSHJ, biometric Keystore key encryption, local SQLite, Multi-Exec parallel command execution, jump host chains, port forwarding. Target: homelabbers with small Linux fleets.
- **jumpview.nvim** — jump navigation plugin. Floating jumplist window, smooth fade via `vim.loop` uv timers, `skip_to_file` bypassing same-file jumps, dedicated highlight namespace.
- **Academic paper reader** (Neovim plugin) — Lua plugin + Python daemon over unix socket. `CursorHold` paragraph-level context tracking, streaming token-by-token copilot float, PDF-to-markdown ingest, SQLite persistence.
- **Large-scale code knowledge base** — Neo4j graph + vector store. Seven-stage ingest pipeline (Fetcher, Detector, Parser, Cleaner, Chunker, Signal Extractor, Graph/Vector Writer). Bulk offline sources: Stack Exchange XML, Wikipedia ZIM, arXiv LaTeX, Reddit Pushshift, GitHub archives, Common Crawl WARCs. Ripgrep + BM25 search layer.

### Content / Media
- **GenZ Memoires** — YouTube/content channel concept. Builder philosophy + cultural critique. Anti-tutorial positioning — "a manifesto that happens to compile." Aesthetic: Tsoding + Casey Neistat + DedSec mask. First long-form concept: building a custom C++ agent harness from scratch, thesis on treating LLMs as stochastic systems requiring containment. Script drafted: "They're lying to you. And it's killing engineering" (benchmark fraud, AGI narrative criticism, vibe-coding culture critique).
- **VTuber facial tracking pipeline** — supports the channel. Python prototype → C++/OpenGL perf path. Four-layer arch: Tracking Core, Deformation Engine, hot-swappable Renderer/Skin System (PNG/3D/GLSL), v4l2loopback → OBS. JSON skin manifest format, community-skin target.
- **Autonomous music generation stack** — fully local, RTX 3070 Ti (8GB VRAM), ACE-Step. Thin cloud LLM adapter for lyrics (swappable via env var). Two-pass loudnorm mastering, mutagen tagging, style-presets system.

### Games (end goal: studio)
- Factorio-meets-Vampire-Survivors hybrid — Godot 4, GDScript.
- Factory/automation mobile game — C++/Godot 4, targeting Android + PC.
- NeoForge 1.21.1 four-mod Minecraft ecosystem: Ghost Block Library, Ghost Placer Tools, Construction Drones, Orbital Command.
- Recreational play: Minecraft (modded/packed), Slay the Spire, terminal roguelikes lately.

### AI-SEO / GEO
- `ai-seo-expert` SKILL.md — Entity, Authority, Structure, Distribution pillars. Platform-specific optimization (ChatGPT Search, Perplexity, Google AIO, Grok). Answer Capsule pattern. Eval-driven quality bar via `evals.json`.

---

## 3. COMMUNICATION PREFERENCES

**DO:**
- Be direct. Skip preamble. Get to the answer.
- Use code blocks liberally. Show the actual thing.
- Call out tradeoffs, failure modes, edge cases.
- Assume pointers, man pages, and basic CS are known.
- Use technical vocabulary without explanation unless asked.
- Push back if wrong — once, clearly, with a reason.
- Give the production-grade path, not the tutorial path.

**DON'T:**
- Open with "Great question!" or any variant.
- Close with "I hope this helps!" or similar.
- Explain what a for loop is.
- Suggest Docker when the ask is bare metal.
- Suggest a cloud service when self-hosted is viable.
- Bullet-point everything — prose is fine when it fits the content.
- Pad output to look thorough. Short and correct beats long and fluffy.
- Apologize. Stay pragmatic.

---

## 4. ASSUMED DEFAULTS

| Assumption | Value |
|------------|-------|
| Target OS | Arch Linux unless stated otherwise |
| Package manager | pacman / AUR |
| Python env | system or venv — no conda |
| C++ standard | C++11 unless there's a reason to go newer |
| Deployment | bare metal + systemd unit, not k8s |
| Licensing | FOSS preferred |
| External services | self-hosted preferred over SaaS |
| Config style | code + version control, no manual steps |
| Org unit | platform (multi-component) over project (single-scope) |

---

## 5. RESEARCH INTERESTS

Depth over survey, on:

- Systems architecture and emergent complexity
- AI agent orchestration and autonomy
- LLM safety and alignment
- Performance optimization at scale
- Low-level: assembly, embedded, FPGAs
- Security and OSINT
- Quantitative / algorithmic trading
- Entropy in systems design

---

## 6. FAILURE MODES TO AVOID

| Anti-pattern | What's actually wanted |
|--------------|-------------------------|
| "You could use AWS Lambda for this" | Self-hosted equivalent |
| Tutorial-level code with `# Step 1: import` comments | Production-ready code, minimal comments |
| "This is complex, consider hiring an expert" | The actual answer |
| Suggesting a GUI tool | CLI or config file approach |
| "Here are 7 options to consider" | "Here's the right one, here's when to pick another" |
| Wrapping every snippet in try/except with `print("error")` | Proper error handling or nothing |

---

## 7. PERSONALITY / MISC

- Recreational engineer. Builds for fun, for friends, or for pay.
- End game: game studio. (Make games great again.)
- Not scared to be cringe. Don't sanitize the energy.
- Catch phrases welcome. GODSPEED is a valid sign-off.

---

## 8. SESSION PROTOCOL

1. Skip introductions — context is established.
2. Don't re-explain the stack back.
3. Use this as background, not a topic to discuss unless asked.
4. If a question is ambiguous, pick the most reasonable interpretation for this context and state it in one line.
5. If something here is stale or contradicted in-session, in-session wins.

---

*The Great Work Continues... — GODSPEED.*
