#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/lib/common.sh
source "$SCRIPT_DIR/lib/common.sh"

ROOT=$(nr_root)
FORWARD=()
CLEAN=0

usage() {
  cat <<'EOF'
Usage: scripts/build-all.sh [--container] [--dev-bootsel] [--jobs N]
                            [--prepare-only] [--clean]

Builds every profile independently and continues after failures so the final
summary reports the complete matrix.  --clean removes only the selected ignored
dist directory before starting.
EOF
}

while (($#)); do
  case "$1" in
    --container|--dev-bootsel|--prepare-only) FORWARD+=("$1") ;;
    --jobs)
      (($# >= 2)) || nr_die "--jobs requires a value"
      FORWARD+=("$1" "$2")
      shift
      ;;
    --clean) CLEAN=1 ;;
    -h|--help) usage; exit 0 ;;
    *) nr_die "unknown option: $1" ;;
  esac
  shift
done

DIST_ROOT=${NUTTX_RP2350_DIST_DIR:-$ROOT/dist}
if ((CLEAN)); then
  rm -rf "$DIST_ROOT"
fi
mkdir -p "$DIST_ROOT"

mapfile_supported=0
if builtin help mapfile >/dev/null 2>&1; then
  mapfile_supported=1
fi

PROFILES=()
if ((mapfile_supported)); then
  mapfile -t PROFILES < <(nr_profile_ids "$ROOT")
else
  while IFS= read -r profile; do
    PROFILES+=("$profile")
  done < <(nr_profile_ids "$ROOT")
fi

((${#PROFILES[@]} > 0)) || nr_die "profiles/manifest.json contains no profiles"

FAILED=()
for profile in "${PROFILES[@]}"; do
  nr_note "building profile ${profile}"
  if ! "$SCRIPT_DIR/build-profile.sh" "$profile" "${FORWARD[@]}"; then
    FAILED+=("$profile")
  fi
done

if ((${#FAILED[@]})); then
  printf 'Failed profiles:\n' >&2
  printf '  %s\n' "${FAILED[@]}" >&2
  exit 1
fi

nr_note "all ${#PROFILES[@]} profiles completed successfully"
