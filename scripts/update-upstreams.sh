#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/lib/common.sh
source "$SCRIPT_DIR/lib/common.sh"

ROOT=$(nr_root)
NEW_NUTTX=
NEW_APPS=

usage() {
  cat <<'EOF'
Usage: scripts/update-upstreams.sh --nuttx <full-sha> --apps <full-sha>

Fetches explicit Apache commits without checking them out, applies both patch
stacks and builds all eight profiles in the pinned container.  Only after the
whole matrix succeeds are sources.lock.json and both submodule gitlinks moved.
The resulting tracked changes are intentionally left unstaged for review.
EOF
}

while (($#)); do
  case "$1" in
    --nuttx)
      (($# >= 2)) || nr_die "--nuttx requires a full commit SHA"
      NEW_NUTTX=$2
      shift
      ;;
    --apps)
      (($# >= 2)) || nr_die "--apps requires a full commit SHA"
      NEW_APPS=$2
      shift
      ;;
    -h|--help) usage; exit 0 ;;
    *) nr_die "unknown option: $1" ;;
  esac
  shift
done

for value in "$NEW_NUTTX" "$NEW_APPS"; do
  [[ $value =~ ^[0-9a-fA-F]{40}$ ]] || nr_die "both candidate pins must be full commit SHAs"
done

[[ -z $(git -C "$ROOT" status --porcelain=v1 --untracked-files=all --ignore-submodules=none) ]] || \
  nr_die "upstream updates require a completely clean wrapper and submodules"

OLD_NUTTX=$(nr_lock_commit "$ROOT" nuttx)
OLD_APPS=$(nr_lock_commit "$ROOT" apps)
NUTTX_URL=$(nr_lock_url "$ROOT" nuttx)
APPS_URL=$(nr_lock_url "$ROOT" apps)

nr_ensure_commit "$ROOT" nuttx "$NEW_NUTTX" "$NUTTX_URL"
nr_ensure_commit "$ROOT" apps "$NEW_APPS" "$APPS_URL"

STAMP=$(date -u +%Y%m%dT%H%M%SZ)-$$
UPDATE_ROOT=$ROOT/build/upstream-update/$STAMP
mkdir -p "$UPDATE_ROOT"

nr_note "validating candidate pins in an isolated full build matrix"
NUTTX_COMMIT_OVERRIDE=$NEW_NUTTX \
APPS_COMMIT_OVERRIDE=$NEW_APPS \
NUTTX_RP2350_ALLOW_PIN_OVERRIDE=1 \
NUTTX_RP2350_WORK_ROOT=$UPDATE_ROOT/work \
NUTTX_RP2350_DIST_DIR=$UPDATE_ROOT/dist \
NUTTX_RP2350_VERSION=upstream-candidate \
  "$SCRIPT_DIR/build-all.sh" --container --clean

MUTATED=0
rollback() {
  local status=$?
  if ((MUTATED)) && ((status != 0)); then
    printf 'Update failed after validation; restoring original pins.\n' >&2
    git -C "$ROOT/nuttx" checkout --detach "$OLD_NUTTX" >/dev/null 2>&1 || true
    git -C "$ROOT/apps" checkout --detach "$OLD_APPS" >/dev/null 2>&1 || true
    nr_python - "$ROOT/sources.lock.json" "$OLD_NUTTX" "$OLD_APPS" <<'PY' || true
import json
import os
import sys

path, nuttx, apps = sys.argv[1:]
with open(path, "r", encoding="utf-8") as stream:
    lock = json.load(stream)
lock["upstreams"]["nuttx"]["commit"] = nuttx
lock["upstreams"]["apps"]["commit"] = apps
temporary = path + ".rollback"
with open(temporary, "w", encoding="utf-8") as stream:
    json.dump(lock, stream, indent=2)
    stream.write("\n")
os.replace(temporary, path)
PY
  fi
  exit "$status"
}
trap rollback EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

MUTATED=1
git -C "$ROOT/nuttx" checkout --detach "$NEW_NUTTX"
git -C "$ROOT/apps" checkout --detach "$NEW_APPS"
nr_python - "$ROOT/sources.lock.json" "$NEW_NUTTX" "$NEW_APPS" <<'PY'
import json
import os
import sys

path, nuttx, apps = sys.argv[1:]
with open(path, "r", encoding="utf-8") as stream:
    lock = json.load(stream)
lock["upstreams"]["nuttx"]["commit"] = nuttx
lock["upstreams"]["apps"]["commit"] = apps
temporary = path + ".new"
with open(temporary, "w", encoding="utf-8") as stream:
    json.dump(lock, stream, indent=2)
    stream.write("\n")
os.replace(temporary, path)
PY

[[ $(git -C "$ROOT/nuttx" rev-parse HEAD) == "$NEW_NUTTX" ]]
[[ $(git -C "$ROOT/apps" rev-parse HEAD) == "$NEW_APPS" ]]
[[ $(nr_lock_commit "$ROOT" nuttx) == "$NEW_NUTTX" ]]
[[ $(nr_lock_commit "$ROOT" apps) == "$NEW_APPS" ]]

MUTATED=0
trap - EXIT INT TERM
nr_note "validated all profiles; lock and gitlinks now point at candidate commits"
git -C "$ROOT" status --short
