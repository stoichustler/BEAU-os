#!/usr/bin/env python3
"""Verify that an exported model manifest matches the scheduler feature ABI."""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from schema import FEATURE_COLUMNS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if manifest.get("schema") != "AI_SCHED":
        raise SystemExit("unexpected schema")
    if tuple(manifest.get("features", ())) != FEATURE_COLUMNS:
        raise SystemExit("feature ABI mismatch")
    if not args.manifest.with_suffix(".h").is_file():
        raise SystemExit("generated C header missing")
    print("AI scheduler model manifest valid")


if __name__ == "__main__":
    main()
