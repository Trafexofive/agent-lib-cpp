# Examples

Reference implementations and templates. Copy these as starting points for new modules.

## Module Standard
Every example follows the same pattern as production manifests:
```
<module>/
├── README.md      ← What it does, how to customize
├── <manifest>.yml ← Schema definition
└── <script>       ← Implementation (if not builtin)
```

## Available Examples

| Type | Example | Runtime | Description |
|------|---------|---------|-------------|
| feed | example-python | python3 | Python feed with timestamp, PID, uptime |
| feed | example-bash | bash | Shell feed with load average, disk usage |
| agent | c_agent | C | Minimal C agent example |
| agent | python_agent | python3 | Python agent example |

## How to Use
```bash
# Copy an example to staging for development
cp -r examples/feeds/example-python config/staging/feeds/my-feed/

# Customize the manifest and script
vim config/staging/feeds/my-feed/feed.yml
vim config/staging/feeds/my-feed/feed.py

# Test
make test-feeds
```
