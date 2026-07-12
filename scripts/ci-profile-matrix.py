#!/usr/bin/env python3
"""Select the smallest safe firmware matrix for a set of changed paths."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def profile_ids() -> list[str]:
    manifest = json.loads((ROOT / "profiles/manifest.json").read_text(encoding="utf-8"))
    return [profile["id"] for profile in manifest["profiles"]]


def select(paths: list[str]) -> list[str]:
    profiles = profile_ids()
    selected: set[str] = set()

    if not paths:
        return profiles

    for path in paths:
        parts = Path(path).parts
        if len(parts) >= 3 and parts[0] == "profiles" and parts[1] in profiles:
            selected.add(parts[1])
            continue

        # Documentation does not affect firmware.  It may be present in the
        # same commit as a profile-only adjustment even though it does not
        # independently trigger the build workflow.

        if path == "README.md" or path.startswith("docs/"):
            continue

        # Any shared source, patch, overlay, lock, script, manifest, or CI
        # change can affect every profile and must keep the full matrix.

        return profiles

    return [profile for profile in profiles if profile in selected] or profiles


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="*")
    parser.add_argument(
        "--stdin", action="store_true", help="read one changed path per line from stdin"
    )
    args = parser.parse_args()
    paths = list(args.paths)
    if args.stdin:
        paths.extend(line.strip() for line in sys.stdin if line.strip())
    print(json.dumps(select(paths), separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
