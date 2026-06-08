#!/usr/bin/env python3
"""
harness-compliance.py — Test MK3 harness protocol compliance across providers/models.

Checks whether models emit correct XML protocol tags when given a harness prompt:
  - Rf✅ = <response final="true">  (fully compliant)
  - R⚠️  = <response> without final attribute
  - RL   = <response> with closing on same line (suspicious)
  - bare = no XML tags at all (raw text)
  - empty = empty content (reasoning-only models)

Usage:
  python3 tests/harness-compliance.py                          # all models, default harness
  python3 tests/harness-compliance.py --harness config/harness/default.mini.md
  python3 tests/harness-compliance.py --model llama-4-scout     # single model
  python3 tests/harness-compliance.py --all-harnesses           # test every harness variant
  python3 tests/harness-compliance.py --json                    # machine-readable output
"""
import argparse
import json
import os
import re
import sys
import time
from pathlib import Path

try:
    import httpx
except ImportError:
    print("ERROR: httpx required. pip install httpx", file=sys.stderr)
    sys.exit(1)

PROJECT_ROOT = Path(__file__).resolve().parents[1]  # agent-lib-MK3/
HARNESS_DIR = PROJECT_ROOT / "config" / "harness"
SYSTEM_PROMPT_TEMPLATE = PROJECT_ROOT / "config" / "harness" / "system-prompt-template.txt"

# ─── Model definitions ───────────────────────────────────────────────────

MODELS = {
    # Groq
    "llama-3.3-70b":   {"provider": "groq",      "model": "llama-3.3-70b-versatile"},
    "llama-4-scout":   {"provider": "groq",      "model": "meta-llama/llama-4-scout-17b-16e-instruct"},
    "qwen3-32b":       {"provider": "groq",      "model": "qwen/qwen3-32b"},
    "gpt-oss-120b":    {"provider": "groq",      "model": "openai/gpt-oss-120b"},
    # OpenRouter
    "or-nemotron":     {"provider": "openrouter", "model": "nvidia/nemotron-3-super-120b-a12b:free"},
    "or-gpt-oss":      {"provider": "openrouter", "model": "openai/gpt-oss-120b:free"},
    # OpenCode
    "oc-flash":        {"provider": "opencode",   "model": "deepseek-v4-flash-free"},
    "oc-nemotron":     {"provider": "opencode",   "model": "nemotron-3-super"},
    "oc-bigpickle": {"provider": "opencode", "model": "big-pickle"},
    # NVIDIA NIM
    "nim-ds-v4-pro": {"provider": "nvidia", "model": "deepseek-ai/deepseek-v4-pro"},
    "nim-ds-v4-flash": {"provider": "nvidia", "model": "deepseek-ai/deepseek-v4-flash"},
    "nim-minimax-m27": {"provider": "nvidia", "model": "minimaxai/minimax-m2.7"},
    "nim-glm-51": {"provider": "nvidia", "model": "z-ai/glm-5.1"},
    # DeepSeek direct
    "ds-v4-pro": {"provider": "deepseek", "model": "deepseek-v4-pro"},
    "ds-v4-flash": {"provider": "deepseek", "model": "deepseek-v4-flash"},
}

PROVIDERS = {
    "groq": {
        "base_url": "https://api.groq.com/openai/v1",
        "api_key_env": "GROQ_API_KEY",
    },
    "openrouter": {
        "base_url": "https://openrouter.ai/api/v1",
        "api_key_env": "OPENROUTER_API_KEY",
    },
    "opencode": {
        "base_url": "https://opencode.ai/zen/v1",
        "api_key_env": "OPENCODE_API_KEY",
    },
    "nvidia": {
        "base_url": "https://integrate.api.nvidia.com/v1",
        "api_key_env": "NVIDIA_API_KEY",
    },
    "deepseek": {
        "base_url": "https://api.deepseek.com/v1",
        "api_key_env": "DEEPSEEK_API_KEY",
    },
}

# ─── Test prompts ─────────────────────────────────────────────────────────

TEST_PROMPTS = [
    {
        "id": "simple_math",
        "prompt": "What is 2+2? Answer with just the number.",
        "expect": "final_response",  # should end with <response final="true">
    },
    {
        "id": "list_files",
        "prompt": "List the files in the current directory.",
        "expect": "action_then_response",  # should use <action> then <response final="true">
    },
]

# ─── Compliance classification ────────────────────────────────────────────

def classify_output(text: str, expect: str = "final_response") -> dict:
    """Classify model output by protocol compliance level.

    expect='final_response': output should end with <response final="true">
    expect='action_then_response': output should have <action> + <response> (non-final is OK for turn 1)
    """
    if not text or not text.strip():
        return {"code": "empty", "detail": "no content", "compliant": False}

    # Check for <response final="true">
    rf_match = re.search(r'<response\s+final\s*=\s*"true"', text)
    if rf_match:
        close_match = re.search(r'<response\s+final\s*=\s*"true"[^>]*>.*?</response>', text, re.DOTALL)
        if close_match:
            return {"code": "Rf", "detail": "<response final=\"true\">", "compliant": True}
        return {"code": "Rf-unclosed", "detail": "<response final=\"true\"> but no </response>", "compliant": True}

    # Check for <response> without final attribute
    r_match = re.search(r'<response(?:\s[^>]*)?>', text)
    if r_match:
        has_action = bool(re.search(r'<action\s', text))
        if expect == "action_then_response" and has_action:
            return {"code": "R+action", "detail": "<response> + <action> (correct turn 1)", "compliant": True}
        if re.search(r'<response\s+final\s*=\s*"false"', text):
            return {"code": "Rf-false", "detail": '<response final="false">', "compliant": False}
        return {"code": "R", "detail": f"<response> without final attr", "compliant": False}

    # Check for any XML tags at all
    any_tag = re.search(r'<(?:action|thought|result)', text)
    if any_tag:
        if expect == "action_then_response":
            return {"code": "action-only", "detail": "<action> only (valid turn 1, no narration)", "compliant": True}
        return {"code": "partial-xml", "detail": f"has XML but no <response>: {any_tag.group(0)}", "compliant": False}

    # Bare text
    return {"code": "bare", "detail": f"no XML tags: {text[:80]!r}", "compliant": False}

# ─── System prompt builder ─────────────────────────────────────────────────

def build_system_prompt(harness_text: str, mk3_mode: bool = False) -> str:
    """Build the system prompt. If mk3_mode=True, wrap the harness in the real
    cortex-mk3 system prompt template (with persona, tools, feeds)."""
    if not mk3_mode:
        return harness_text

    if not SYSTEM_PROMPT_TEMPLATE.exists():
        print(f"WARNING: template not found at {SYSTEM_PROMPT_TEMPLATE}, using raw harness", file=sys.stderr)
        return harness_text

    template = SYSTEM_PROMPT_TEMPLATE.read_text()
    # Find the <protocol>...</protocol> section and replace its content with the new harness
    protocol_start = template.find("<protocol>")
    protocol_end = template.find("</protocol>")
    if protocol_start < 0 or protocol_end < 0:
        print("WARNING: template missing <protocol> tags, using raw harness", file=sys.stderr)
        return harness_text

    new_prompt = (
        template[:protocol_start + len("<protocol>")]
        + "\n" + harness_text + "\n"
        + template[protocol_end:]
    )
    return new_prompt


# ─── API call ─────────────────────────────────────────────────────────────

def call_model(provider: str, model_id: str, harness_text: str, user_prompt: str, max_retries: int = 15) -> dict:
    """Call a model via OpenAI-compatible API. Returns raw response info.
    Retries on 429 rate limit with exponential backoff."""
    prov = PROVIDERS.get(provider)
    if not prov:
        return {"error": f"Unknown provider: {provider}", "content": ""}

    api_key = os.environ.get(prov["api_key_env"], "")
    if not api_key:
        return {"error": f"Missing {prov['api_key_env']}", "content": ""}

    # IMPORTANT: system prompt goes in messages[0] role=system, NOT top-level "system" param.
    # OpenAI chat completions spec requires messages-based system prompt.
    # Top-level "system" is silently dropped by some providers (OpenRouter).
    messages = [
        {"role": "system", "content": harness_text},
        {"role": "user", "content": user_prompt},
    ]

    payload = {
        "model": model_id,
        "messages": messages,
        "temperature": 0.1,
        "max_tokens": 2048,
        "stream": False,
    }

    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }
    if provider == "openrouter":
        headers["HTTP-Referer"] = "https://cortex-prime.local"
        headers["X-Title"] = "Cortex MK3 Compliance Test"

    url = f"{prov['base_url']}/chat/completions"

    for attempt in range(max_retries + 1):
        try:
            with httpx.Client(timeout=60.0) as client:
                resp = client.post(url, json=payload, headers=headers)

                # Retry on 429 rate limit
                if resp.status_code == 429 and attempt < max_retries:
                    wait = (2 ** attempt) * 30  # 30s, 60s, 120s, 240s...
                    print(f"    ⏳ 429 rate limited, waiting {wait}s (attempt {attempt+1}/{max_retries})...", file=sys.stderr)
                    time.sleep(wait)
                    continue

                resp.raise_for_status()
                data = resp.json()

            # Extract content — handle reasoning models that put content in different fields
            choice = data.get("choices", [{}])[0]
            message = choice.get("message", {})

            content = message.get("content", "") or ""
            reasoning = message.get("reasoning_content", "") or message.get("reasoning", "") or ""

            # If content is empty but reasoning exists, the model may have put everything in reasoning
            if not content.strip() and reasoning.strip():
                return {
                    "content": reasoning,
                    "reasoning_only": True,
                    "usage": data.get("usage", {}),
                    "model": data.get("model", model_id),
                }

            return {
                "content": content,
                "reasoning_only": False,
                "usage": data.get("usage", {}),
                "model": data.get("model", model_id),
            }

        except httpx.HTTPStatusError as e:
            return {"error": f"HTTP {e.response.status_code}: {e.response.text[:200]}", "content": ""}
        except httpx.TimeoutException:
            return {"error": "timeout", "content": ""}
        except Exception as e:
            return {"error": str(e), "content": ""}

    return {"error": f"429 rate limit after {max_retries} retries", "content": ""}


# ─── Runner ───────────────────────────────────────────────────────────────

def run_test(harness_path: Path, model_key: str, prompt: dict, mk3_mode: bool = False) -> dict:
    """Run a single test: harness + model + prompt."""
    harness_text = harness_path.read_text()
    system_prompt = build_system_prompt(harness_text, mk3_mode=mk3_mode)
    model_def = MODELS[model_key]
    provider = model_def["provider"]
    model_id = model_def["model"]

    result = call_model(provider, model_id, system_prompt, prompt["prompt"])
    classification = classify_output(result.get("content", ""), expect=prompt.get("expect", "final_response"))

    return {
        "harness": harness_path.name,
        "model_key": model_key,
        "provider": provider,
        "model_id": model_id,
        "prompt_id": prompt["id"],
        "classification": classification,
        "reasoning_only": result.get("reasoning_only", False),
        "error": result.get("error"),
        "content_preview": result.get("content", "")[:200],
        "usage": result.get("usage", {}),
    }


def format_result(r: dict) -> str:
    """Format a single result for terminal display."""
    cls = r["classification"]
    code = cls["code"]
    compliant = "✅" if cls["compliant"] else "⚠️ "
    error = r.get("error", "")

    if error:
        return f"  {r['model_key']:20s} {r['prompt_id']:15s} ❌ ERROR: {error[:60]}"
    if r.get("reasoning_only"):
        return f"  {r['model_key']:20s} {r['prompt_id']:15s} 🔵 {code} (reasoning_only)"
    return f"  {r['model_key']:20s} {r['prompt_id']:15s} {compliant} {code}"


# ─── Main ─────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="MK3 harness compliance test")
    ap.add_argument("--harness", type=Path, default=None,
                    help="Single harness file to test (default: default.md)")
    ap.add_argument("--all-harnesses", action="store_true",
                    help="Test all harness variants in config/harness/")
    ap.add_argument("--model", action="append", dest="models",
                    help="Model key to test (repeatable). Default: all Groq models.")
    ap.add_argument("--all-models", action="store_true",
                    help="Test all defined models (Groq + OpenRouter + OpenCode)")
    ap.add_argument("--prompt", action="append", dest="prompts",
                    help="Prompt ID to test (repeatable). Default: all.")
    ap.add_argument("--json", action="store_true",
                    help="Output results as JSON")
    ap.add_argument("--repeat", type=int, default=1,
                    help="Repeat each test N times for stability")
    ap.add_argument("--delay", type=float, default=2.0,
                    help="Delay between API calls in seconds (rate limiting)")
    ap.add_argument("--mk3", action="store_true",
                    help="Wrap harness in full cortex-mk3 system prompt (persona + tools + feeds)")
    args = ap.parse_args()

    # Resolve harness files
    if args.all_harnesses:
        harness_files = sorted(
            f for f in HARNESS_DIR.iterdir()
            if f.name.startswith("default") and ".md" in f.name and f.is_file()
        )
    elif args.harness:
        harness_files = [args.harness]
    else:
        harness_files = [HARNESS_DIR / "default.md"]

    for h in harness_files:
        if not h.exists():
            print(f"ERROR: harness not found: {h}", file=sys.stderr)
            sys.exit(1)

    # Resolve models
    if args.all_models:
        model_keys = list(MODELS.keys())
    elif args.models:
        model_keys = args.models
        unknown = [k for k in model_keys if k not in MODELS]
        if unknown:
            print(f"ERROR: unknown models: {unknown}", file=sys.stderr)
            print(f"Available: {list(MODELS.keys())}", file=sys.stderr)
            sys.exit(1)
    else:
        # Default: Groq models only (most reliable for testing)
        model_keys = [k for k, v in MODELS.items() if v["provider"] == "groq"]

    # Resolve prompts
    if args.prompts:
        test_prompts = [p for p in TEST_PROMPTS if p["id"] in args.prompts]
    else:
        test_prompts = TEST_PROMPTS

    all_results = []

    for harness_path in harness_files:
        print(f"\n{'='*70}")
        print(f"HARNESS: {harness_path.name} ({harness_path.stat().st_size:,} bytes)")
        print(f"{'='*70}")

        for model_key in model_keys:
            for prompt in test_prompts:
                for run_idx in range(args.repeat):
                    label = f"[{run_idx+1}/{args.repeat}]" if args.repeat > 1 else ""
                    result = run_test(harness_path, model_key, prompt, mk3_mode=args.mk3)
                    result["run_idx"] = run_idx
                    all_results.append(result)
                    print(format_result(result) + (f" {label}" if label else ""))

                    if args.delay and not (model_key == model_keys[-1] and prompt == test_prompts[-1] and run_idx == args.repeat - 1):
                        time.sleep(args.delay)

    # Summary table
    print(f"\n{'='*70}")
    print("SUMMARY")
    print(f"{'='*70}")
    print(f"{'Model':<20s} {'Harness':<25s} {'Code':<10s} {'Compliant':<10s}")
    print("-" * 70)

    # Aggregate by model+harness (take best across prompts/repeats)
    from collections import defaultdict
    agg = defaultdict(list)
    for r in all_results:
        key = (r["model_key"], r["harness"])
        agg[key].append(r)

    for (model_key, harness_name), results in sorted(agg.items()):
        # Best compliance: any Rf = compliant
        best = max(results, key=lambda r: (r["classification"]["compliant"], r["classification"]["code"]))
        cls = best["classification"]
        comp_str = "✅ YES" if cls["compliant"] else "❌ NO"
        print(f"{model_key:<20s} {harness_name:<25s} {cls['code']:<10s} {comp_str}")

    if args.json:
        print("\n--- JSON ---")
        print(json.dumps(all_results, indent=2))

    # Exit code: 0 if all compliant, 1 if any non-compliant
    non_compliant = [r for r in all_results if not r["classification"]["compliant"] and not r.get("error")]
    return 1 if non_compliant else 0


if __name__ == "__main__":
    raise SystemExit(main())
