#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/lib/common.sh
source "$SCRIPT_DIR/lib/common.sh"
# shellcheck source=scripts/lib/hardware.sh
source "$SCRIPT_DIR/lib/hardware.sh"

ROOT=$(nr_root)
PROFILE=
UF2=
SERIAL=
DEV_BOOTSEL=0
BUILD=0
USE_CONTAINER=0
FORCE_BOARD=0
DIST_ROOT=${NUTTX_RP2350_DIST_DIR:-$ROOT/dist}

usage() {
  cat <<'EOF'
Usage: scripts/flash-profile.sh <profile-id> [options]

Options:
  --uf2 PATH       Flash this exact artifact instead of selecting from dist/.
  --build          Build the profile first.
  --container      Use the pinned container if --build is selected.
  --dev-bootsel    Select/build the development BOOTSEL-recovery variant.
  --serial PATH    Serial device to use for NSH/1200-baud BOOTSEL recovery.
  --force-board    Permit flashing if existing firmware metadata names another board.
EOF
}

while (($#)); do
  case "$1" in
    --uf2)
      (($# >= 2)) || nr_die "--uf2 requires a path"
      UF2=$2
      shift
      ;;
    --serial)
      (($# >= 2)) || nr_die "--serial requires a path"
      SERIAL=$2
      shift
      ;;
    --build) BUILD=1 ;;
    --container) USE_CONTAINER=1 ;;
    --dev-bootsel) DEV_BOOTSEL=1 ;;
    --force-board) FORCE_BOARD=1 ;;
    -h|--help) usage; exit 0 ;;
    --*) nr_die "unknown option: $1" ;;
    *)
      [[ -z $PROFILE ]] || nr_die "only one profile may be flashed"
      PROFILE=$1
      ;;
  esac
  shift
done

[[ -n $PROFILE ]] || { usage >&2; exit 2; }
ARTIFACT_SLUG=$(nr_profile_field "$ROOT" "$PROFILE" artifact_slug) || \
  nr_die "unknown profile: $PROFILE"

if ((BUILD)); then
  BUILD_ARGS=()
  ((USE_CONTAINER)) && BUILD_ARGS+=(--container)
  ((DEV_BOOTSEL)) && BUILD_ARGS+=(--dev-bootsel)
  "$SCRIPT_DIR/build-profile.sh" "$PROFILE" "${BUILD_ARGS[@]}"
fi

if [[ -z $UF2 ]]; then
  if ((DEV_BOOTSEL)); then
    PATTERN="$DIST_ROOT/nuttx-rp2350-*-${ARTIFACT_SLUG}-dev-bootsel.uf2"
  else
    PATTERN="$DIST_ROOT/nuttx-rp2350-*-${ARTIFACT_SLUG}.uf2"
  fi
  UF2=$(nr_python - "$PATTERN" <<'PY'
import glob
import os
import sys

matches = glob.glob(sys.argv[1])
if matches:
    print(max(matches, key=os.path.getmtime))
PY
  )
fi

[[ -n $UF2 && -f $UF2 ]] || \
  nr_die "no matching UF2 found; build it with --build or specify --uf2"
nr_require_tool picotool

if ! nr_recover_bootsel "$SERIAL" 30; then
  nr_die "unable to enter BOOTSEL automatically; connect the board in BOOTSEL and retry"
fi
INFO=$(nr_confirm_rp2350_bootsel)

if ((FORCE_BOARD == 0)); then
  NORMALIZED_INFO=$(tr '[:upper:]' '[:lower:]' <<< "$INFO")
  case "$PROFILE:$NORMALIZED_INFO" in
    pico-2-w-*:*fruit*jam*)
      nr_die "connected firmware metadata identifies Fruit Jam, not Pico 2 W"
      ;;
    fruit-jam-*:*pico*2*w*)
      nr_die "connected firmware metadata identifies Pico 2 W, not Fruit Jam"
      ;;
  esac
fi

nr_note "flashing ${UF2}"
picotool load -x "$UF2"
nr_note "flash complete: $(nr_sha256 "$UF2")"
