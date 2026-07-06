#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
UF2=${1:-"$ROOT/nuttx/nuttx.uf2"}

if [ ! -f "$UF2" ]; then
  printf 'UF2 not found: %s\n' "$UF2" >&2
  exit 1
fi

picotool load -x "$UF2"
