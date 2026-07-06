#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

apply_one()
{
  repo=$1
  patch=$2
  label=$3

  if git -C "$repo" apply --reverse --check "$patch" >/dev/null 2>&1; then
    printf '%s patch already applied\n' "$label"
    return 0
  fi

  printf 'Applying %s patch\n' "$label"
  git -C "$repo" apply --check "$patch"
  git -C "$repo" apply --whitespace=nowarn "$patch"
}

git -C "$ROOT" submodule update --init --recursive

apply_one "$ROOT/nuttx" "$ROOT/patches/nuttx/0001-fruitclaw-nuttx.patch" "NuttX"
apply_one "$ROOT/apps" "$ROOT/patches/apps/0001-fruitclaw-apps.patch" "apps"

printf 'FruitClaw patches are ready\n'
