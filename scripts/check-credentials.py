#!/usr/bin/env python3
"""Reject likely embedded credentials in project-owned integration sources."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCAN_ROOTS = (
    ROOT / ".github",
    ROOT / "docs",
    ROOT / "overlays",
    ROOT / "patches",
    ROOT / "profiles",
    ROOT / "scripts",
)
SCAN_FILES = (ROOT / "README.md", ROOT / "sources.lock.json")
SKIP_SUFFIXES = {
    ".a",
    ".bin",
    ".elf",
    ".gif",
    ".ico",
    ".jpg",
    ".jpeg",
    ".o",
    ".pdf",
    ".png",
    ".pyc",
    ".uf2",
    ".zip",
}

KEY_PATTERN = (
    r"(?:"
    r"CONFIG_[A-Z0-9_]*(?:SSID|PASSWORD|PASSWD|PASSPHRASE|PSK|TOKEN|API_KEY)"
    r"|(?:WIFI[_-]?)?(?:SSID|PASSWORD|PASSWD|PASSPHRASE|PSK)"
    r"|TELEGRAM[_-]?(?:TOKEN|BOT[_-]?TOKEN)"
    r"|DEEPSEEK[_-]?(?:KEY|TOKEN)"
    r"|API[_-]?KEY"
    r")"
)
QUOTED_ASSIGNMENT = re.compile(
    rf"(?ix)(?:(?P<key_quote>[\"']){KEY_PATTERN}(?P=key_quote)"
    rf"|(?<![\"']){KEY_PATTERN})"
    rf"\s*[:=]\s*(?P<value_quote>[\"'])(?P<value>[^\"']*)(?P=value_quote)"
)
AUTHENTICATED_URL = re.compile(r"https?://[^\s/:]+:[^\s/@]+@", re.IGNORECASE)

PLACEHOLDER_WORDS = {
    "changeme",
    "example",
    "none",
    "null",
    "password",
    "redacted",
    "runtime",
    "secret",
    "ssid",
    "unset",
}


def project_files() -> list[Path]:
    paths = [path for path in SCAN_FILES if path.is_file()]
    for directory in SCAN_ROOTS:
        if not directory.is_dir():
            continue
        paths.extend(path for path in directory.rglob("*") if path.is_file())
    return sorted(set(paths))


def is_placeholder(value: str) -> bool:
    stripped = value.strip()
    if not stripped or stripped in {"0", "-"}:
        return True
    if any(marker in stripped for marker in ("%", "$", "<", ">", "{", "}")):
        return True
    lowered = stripped.lower()
    if lowered in PLACEHOLDER_WORDS:
        return True
    return any(word in lowered for word in ("your_", "your-", "example_", "example-"))


def main() -> int:
    findings: list[str] = []
    scanned = 0
    for path in project_files():
        if path.suffix.lower() in SKIP_SUFFIXES:
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        scanned += 1
        for line_number, line in enumerate(text.splitlines(), 1):
            if AUTHENTICATED_URL.search(line):
                findings.append(
                    f"{path.relative_to(ROOT)}:{line_number}: authenticated URL"
                )
            for match in QUOTED_ASSIGNMENT.finditer(line):
                value = match.group("value")
                if not is_placeholder(value):
                    findings.append(
                        f"{path.relative_to(ROOT)}:{line_number}: "
                        "literal assigned to credential-like key"
                    )

    if findings:
        print("error: possible embedded credentials found:", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1

    print(f"credential scan passed across {scanned} project-owned text files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
