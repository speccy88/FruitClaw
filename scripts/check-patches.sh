#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/lib/common.sh
source "$SCRIPT_DIR/lib/common.sh"

ROOT=$(nr_root)
CLEAN_WORK=0
if [[ -n ${NUTTX_RP2350_PATCH_CHECK_WORK:-} ]]; then
  WORK=$NUTTX_RP2350_PATCH_CHECK_WORK
else
  nr_require_tool mktemp
  TEMP_ROOT=${TMPDIR:-/tmp}
  WORK=$(mktemp -d "${TEMP_ROOT%/}/nuttx-rp2350-check-patches.XXXXXX")
  CLEAN_WORK=1
fi

cleanup() {
  if ((CLEAN_WORK)); then
    rm -rf -- "$WORK"
  fi
}
trap cleanup EXIT

check_series_inventory() {
  local label=$1
  local directory=$2
  local entry
  local listed=$WORK/${label}.listed
  local actual=$WORK/${label}.actual

  : > "$listed"
  while IFS= read -r entry || [[ -n $entry ]]; do
    entry=${entry%%#*}
    entry=${entry#"${entry%%[![:space:]]*}"}
    entry=${entry%"${entry##*[![:space:]]}"}
    [[ -n $entry ]] || continue
    printf '%s\n' "$entry" >> "$listed"
  done < "$directory/series"

  if [[ $(sort "$listed" | uniq -d | wc -l | tr -d '[:space:]') != 0 ]]; then
    nr_die "duplicate entry in ${directory}/series"
  fi

  find "$directory" -maxdepth 1 -type f -name '*.patch' -exec basename {} \; | \
    LC_ALL=C sort > "$actual"
  LC_ALL=C sort -o "$listed" "$listed"
  if ! cmp -s "$listed" "$actual"; then
    diff -u "$listed" "$actual" >&2 || true
    nr_die "${label} series must list every patch exactly once"
  fi
}

check_patch_scope() {
  local patch
  local changed

  while IFS= read -r patch; do
    changed=$(sed -n 's|^+++ b/||p' "$patch")
    if grep -E -q '(^|/)\.github/workflows/|\.(zip|uf2|bin|o|a|d|su)$|(^|/)rp23xx_examples_romfs\.c$' \
      <<< "$changed"; then
      printf '%s\n' "$changed" | \
        grep -E '(^|/)\.github/workflows/|\.(zip|uf2|bin|o|a|d|su)$|(^|/)rp23xx_examples_romfs\.c$' >&2
      nr_die "excluded generated, binary, or upstream workflow path in ${patch#"$ROOT"/}"
    fi
  done < <(find "$ROOT/patches" -type f -name '*.patch' | LC_ALL=C sort)

  if find "$ROOT/overlays" -type f \
      \( -name '*.zip' -o -name '*.uf2' -o -name '*.bin' -o -name '*.o' \
         -o -name '*.a' -o -name '*.d' -o -name '*.su' \
         -o -name 'rp23xx_examples_romfs.c' \) -print | grep -q .; then
    find "$ROOT/overlays" -type f \
      \( -name '*.zip' -o -name '*.uf2' -o -name '*.bin' -o -name '*.o' \
         -o -name '*.a' -o -name '*.d' -o -name '*.su' \
         -o -name 'rp23xx_examples_romfs.c' \) -print >&2
    nr_die "overlays contain excluded generated or binary files"
  fi
}

[[ -f $ROOT/sources.lock.json ]] || nr_die "missing sources.lock.json"
for source in nuttx apps; do
  [[ -f $ROOT/patches/$source/series ]] || nr_die "missing patches/${source}/series"
  [[ -d $ROOT/overlays/$source ]] || nr_die "missing overlays/${source}"
done

# Validation archives the exact locked commits below.  Snapshot only wrapper
# state without opening the large submodule repositories.
BEFORE_STATUS=$(nr_wrapper_state "$ROOT")
rm -rf "$WORK"
mkdir -p "$WORK/nuttx" "$WORK/apps"

check_series_inventory nuttx "$ROOT/patches/nuttx"
check_series_inventory apps "$ROOT/patches/apps"
check_patch_scope

NUTTX_COMMIT=$(nr_lock_commit "$ROOT" nuttx)
APPS_COMMIT=$(nr_lock_commit "$ROOT" apps)
SOURCE_CACHE_ROOT=${NUTTX_RP2350_SOURCE_CACHE:-$ROOT/build/source-cache}
NUTTX_REPO=$SOURCE_CACHE_ROOT/nuttx.git
APPS_REPO=$SOURCE_CACHE_ROOT/apps.git
nr_prepare_source_cache "$NUTTX_REPO" nuttx "$NUTTX_COMMIT" \
  "$(nr_lock_url "$ROOT" nuttx)"
nr_prepare_source_cache "$APPS_REPO" apps "$APPS_COMMIT" \
  "$(nr_lock_url "$ROOT" apps)"

nr_materialize_commit "$NUTTX_REPO" "$NUTTX_COMMIT" "$WORK/nuttx"
nr_materialize_commit "$APPS_REPO" "$APPS_COMMIT" "$WORK/apps"
nr_apply_series NuttX "$WORK/nuttx" "$ROOT/patches/nuttx"
nr_apply_series apps "$WORK/apps" "$ROOT/patches/apps"

# These symbols do not exist at the locked Apache pins.  Their presence proves
# that Git changed the staged source, rather than merely returning success after
# discovering the wrapper repository and skipping paths under ignored build/.
grep -R -q '^[[:space:]]*config RP23XX_PSRAM$' "$WORK/nuttx/arch/arm/src/rp23xx" || \
  nr_die "NuttX patch marker missing after application"
grep -R -q '^[[:space:]]*config READLINE_FORCE_ECHO$' "$WORK/apps/system/readline" || \
  nr_die "apps patch marker missing after application"

nr_copy_overlay NuttX "$ROOT/overlays/nuttx" "$WORK/nuttx"
nr_copy_overlay apps "$ROOT/overlays/apps" "$WORK/apps"

grep -F -q "tools\$(DELIM)canonicalize_archive.sh" "$WORK/apps/Makefile" || \
  nr_die "deterministic libapps archive hook missing after patch application"
[[ -x $WORK/apps/tools/canonicalize_archive.sh ]] || \
  nr_die "deterministic libapps archive helper is missing or not executable"

AFTER_STATUS=$(nr_wrapper_state "$ROOT")
[[ $AFTER_STATUS == "$BEFORE_STATUS" ]] || \
  nr_die "patch validation changed the wrapper checkout"

nr_note "both ordered patch stacks apply cleanly to their locked upstream commits"
