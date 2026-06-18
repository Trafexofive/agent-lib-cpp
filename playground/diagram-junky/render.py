#!/usr/bin/env python3
"""diagram-junky CLI renderer wrapper.

The reusable renderer now lives in ``diagram_junky.rendering`` so the CLI,
raw ANSI TUI, and future canvas backends share one implementation.
"""

from __future__ import annotations

import sys

from diagram_junky.rendering import main


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
