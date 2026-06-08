#!/usr/bin/env python3
"""Tiny text utility CLI with one intentional bug for TDD repair."""
import argparse
import re


def slugify(text: str) -> str:
    """Convert text into a URL slug."""
    # BUG: leaves underscores as underscores; tests require all separators normalized to hyphens.
    text = text.strip().lower()
    text = re.sub(r"[^a-z0-9_\s-]", "", text)
    text = re.sub(r"[\s-]+", "-", text)
    return text.strip("-")


def titlecase(text: str) -> str:
    return " ".join(word.capitalize() for word in text.split())


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["slug", "title"])
    parser.add_argument("text")
    args = parser.parse_args(argv)

    if args.mode == "slug":
        print(slugify(args.text))
    else:
        print(titlecase(args.text))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
