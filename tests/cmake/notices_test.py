#!/usr/bin/env python3
"""THIRD_PARTY_NOTICES.txt must match the locked license sources exactly."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ORDER = ["wui", "FreeType", "HarfBuzz", "utf8proc", "Inter"]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    args = parser.parse_args()
    check = subprocess.run([sys.executable, str(args.source / "tools" / "collect_notices.py"), "--check"],
                           cwd=args.source)
    if check.returncode != 0:
        print("FAIL collect_notices.py --check")
        return 1
    text = (args.source / "THIRD_PARTY_NOTICES.txt").read_text(encoding="utf-8")
    position = 0
    for name in ORDER:
        heading = text.find(f"== {name} ", position)
        if heading < 0:
            print(f"FAIL missing or out-of-order section: {name}")
            return 1
        body_start = text.index("\n", heading) + 1
        following = text.find("\n== ", body_start)
        body = text[body_start:following if following >= 0 else len(text)]
        if len(body.strip()) < 200:
            print(f"FAIL section {name} has no full license text")
            return 1
        position = body_start
    if "doctest" in text.lower():
        print("FAIL doctest is test-only and must not appear in distributed notices")
        return 1
    print("PASS notices")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
