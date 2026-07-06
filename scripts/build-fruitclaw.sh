#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
JOBS=${JOBS:-}

if [ -z "$JOBS" ]; then
  JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || printf '4')
fi

if [ -d "$HOME/.local/nuttx-tools/kconfig-frontends/bin" ]; then
  PATH="$HOME/.local/nuttx-tools/kconfig-frontends/bin:$PATH"
  export PATH
fi

"$ROOT/scripts/apply-fruitclaw-patches.sh"

cd "$ROOT/nuttx"
./tools/configure.sh -E -m -a ../apps adafruit-fruit-jam-rp2350:esp-hosted
make -j"$JOBS"

if [ -f nuttx.uf2 ]; then
  printf '\nBuilt %s\n' "$ROOT/nuttx/nuttx.uf2"
  shasum -a 256 nuttx.uf2 2>/dev/null || sha256sum nuttx.uf2
fi
