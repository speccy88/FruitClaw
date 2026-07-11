#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/lib/common.sh
source "$SCRIPT_DIR/lib/common.sh"

ROOT=$(nr_root)
PROFILE=
DEV_BOOTSEL=0
USE_CONTAINER=0
PREPARE_ONLY=0
JOBS=${JOBS:-}
WORK_ROOT=${NUTTX_RP2350_WORK_ROOT:-$ROOT/build/work}
DIST_ROOT=${NUTTX_RP2350_DIST_DIR:-$ROOT/dist}

usage() {
  cat <<'EOF'
Usage: scripts/build-profile.sh <profile-id> [options]

Options:
  --dev-bootsel   Generate a development-only image with a 10-minute
                  automatic BOOTSEL recovery backstop.
  --container     Build in the digest-pinned container from sources.lock.json.
  --jobs N        Parallel make jobs (default: detected CPU count).
  --prepare-only  Materialize, patch, overlay, and configure without compiling.
  -h, --help      Show this help.

Environment:
  NUTTX_RP2350_VERSION    Artifact version; otherwise exact tag or dev-<sha>.
  NUTTX_RP2350_WORK_ROOT Work-tree root (default: build/work).
  NUTTX_RP2350_DIST_DIR  Artifact directory (default: dist).
EOF
}

while (($#)); do
  case "$1" in
    --dev-bootsel) DEV_BOOTSEL=1 ;;
    --container) USE_CONTAINER=1 ;;
    --prepare-only) PREPARE_ONLY=1 ;;
    --jobs)
      (($# >= 2)) || nr_die "--jobs requires a value"
      JOBS=$2
      shift
      ;;
    -h|--help) usage; exit 0 ;;
    --*) nr_die "unknown option: $1" ;;
    *)
      [[ -z $PROFILE ]] || nr_die "only one profile may be built at a time"
      PROFILE=$1
      ;;
  esac
  shift
done

[[ -n $PROFILE ]] || { usage >&2; exit 2; }
[[ $PROFILE =~ ^[a-z0-9][a-z0-9-]*$ ]] || nr_die "invalid profile id: $PROFILE"
[[ -f $ROOT/sources.lock.json ]] || nr_die "missing sources.lock.json"
[[ -f $ROOT/profiles/manifest.json ]] || nr_die "missing profiles/manifest.json"

CONTAINER_PLATFORM=
if ((USE_CONTAINER)); then
  CONTAINER_PLATFORM=$(nr_json_value "$ROOT/sources.lock.json" build.container_platform)
  [[ $CONTAINER_PLATFORM =~ ^linux/(amd64|arm64)$ ]] || \
    nr_die "unsupported locked container platform: ${CONTAINER_PLATFORM}"
fi

DEFCONFIG_REL=$(nr_profile_field "$ROOT" "$PROFILE" defconfig) || \
  nr_die "profile is not declared in profiles/manifest.json: $PROFILE"
DEFCONFIG_DEST=$(nr_profile_field "$ROOT" "$PROFILE" defconfig_destination) || \
  nr_die "profile ${PROFILE} has no defconfig_destination"
ARTIFACT_SLUG=$(nr_profile_field "$ROOT" "$PROFILE" artifact_slug 2>/dev/null || printf '%s\n' "$PROFILE")
BOARD=$(nr_profile_field "$ROOT" "$PROFILE" board 2>/dev/null || printf 'unknown\n')
BOARD_CONFIG=$(nr_profile_field "$ROOT" "$PROFILE" board_config) || \
  nr_die "profile ${PROFILE} has no board_config"
NETWORKING=$(nr_profile_field "$ROOT" "$PROFILE" networking) || \
  nr_die "profile ${PROFILE} has no networking contract"

[[ $DEFCONFIG_REL != /* && $DEFCONFIG_REL != *../* && $DEFCONFIG_REL != ../* ]] || \
  nr_die "unsafe defconfig path in manifest: $DEFCONFIG_REL"
[[ $DEFCONFIG_DEST != /* && $DEFCONFIG_DEST != *../* && $DEFCONFIG_DEST != ../* ]] || \
  nr_die "unsafe defconfig destination in manifest: $DEFCONFIG_DEST"
[[ -f $ROOT/$DEFCONFIG_REL ]] || nr_die "profile defconfig not found: $DEFCONFIG_REL"

if [[ -z $JOBS ]]; then
  JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || printf '4')
fi
[[ $JOBS =~ ^[1-9][0-9]*$ ]] || nr_die "--jobs must be a positive integer"

NUTTX_COMMIT=${NUTTX_COMMIT_OVERRIDE:-$(nr_lock_commit "$ROOT" nuttx)}
APPS_COMMIT=${APPS_COMMIT_OVERRIDE:-$(nr_lock_commit "$ROOT" apps)}
NUTTX_URL=$(nr_lock_url "$ROOT" nuttx)
APPS_URL=$(nr_lock_url "$ROOT" apps)

if [[ -n ${NUTTX_COMMIT_OVERRIDE:-}${APPS_COMMIT_OVERRIDE:-} && \
      ${NUTTX_RP2350_ALLOW_PIN_OVERRIDE:-0} != 1 ]]; then
  nr_die "source pin overrides are reserved for scripts/update-upstreams.sh"
fi

for commit in "$NUTTX_COMMIT" "$APPS_COMMIT"; do
  [[ $commit =~ ^[0-9a-fA-F]{40}$ ]] || nr_die "source commit is not a full SHA: $commit"
done

nr_ensure_commit "$ROOT" nuttx "$NUTTX_COMMIT" "$NUTTX_URL"
nr_ensure_commit "$ROOT" apps "$APPS_COMMIT" "$APPS_URL"

if [[ -z ${NUTTX_COMMIT_OVERRIDE:-} ]]; then
  [[ $(nr_gitlink_commit "$ROOT" nuttx) == "$NUTTX_COMMIT" ]] || \
    nr_die "NuttX lock commit does not match the recorded submodule gitlink"
fi
if [[ -z ${APPS_COMMIT_OVERRIDE:-} ]]; then
  [[ $(nr_gitlink_commit "$ROOT" apps) == "$APPS_COMMIT" ]] || \
    nr_die "apps lock commit does not match the recorded submodule gitlink"
fi

BEFORE_STATUS=$(git -C "$ROOT" status --porcelain=v1 --untracked-files=all)
WORK=$WORK_ROOT/$PROFILE
LOCK=$WORK_ROOT/.${PROFILE}.lock
mkdir -p "$WORK_ROOT" "$DIST_ROOT"
if ! mkdir "$LOCK" 2>/dev/null; then
  nr_die "another build is already using profile ${PROFILE}: ${LOCK}"
fi

cleanup() {
  rmdir "$LOCK" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

rm -rf "$WORK"
mkdir -p "$WORK/nuttx" "$WORK/apps"
nr_note "materializing immutable NuttX ${NUTTX_COMMIT}"
nr_materialize_commit "$ROOT/nuttx" "$NUTTX_COMMIT" "$WORK/nuttx"
nr_note "materializing immutable apps ${APPS_COMMIT}"
nr_materialize_commit "$ROOT/apps" "$APPS_COMMIT" "$WORK/apps"

nr_apply_series NuttX "$WORK/nuttx" "$ROOT/patches/nuttx"
nr_apply_series apps "$WORK/apps" "$ROOT/patches/apps"
nr_copy_overlay NuttX "$ROOT/overlays/nuttx" "$WORK/nuttx"
nr_copy_overlay apps "$ROOT/overlays/apps" "$WORK/apps"

BERRY_COMMIT=$(nr_json_value "$ROOT/sources.lock.json" dependencies.berry.version dependencies.berry.commit)
BERRY_URL=$(nr_json_value "$ROOT/sources.lock.json" dependencies.berry.url)
BERRY_HASH=$(nr_json_value "$ROOT/sources.lock.json" dependencies.berry.sha256)
BERRY_CACHE=$ROOT/build/deps/berry-${BERRY_COMMIT}.zip
BERRY_STAGE=$WORK/apps/interpreters/berry/berry-${BERRY_COMMIT}.zip
grep -E -q "^BERRY_COMMIT_ID[[:space:]]*=[[:space:]]*${BERRY_COMMIT}$" \
  "$WORK/apps/interpreters/berry/Makefile" || \
  nr_die "Berry Makefile commit does not match sources.lock.json"
nr_note "staging checksum-verified Berry ${BERRY_COMMIT} archive"
nr_download_verified "$BERRY_URL" "$BERRY_HASH" "$BERRY_CACHE"
install -m 0644 "$BERRY_CACHE" "$BERRY_STAGE"

PICO_SDK_NATIVE_PATH=
PICO_SDK_BUILD_PATH=
if [[ $NETWORKING == cyw43439 ]]; then
  PICO_SDK_VERSION=$(nr_json_value "$ROOT/sources.lock.json" dependencies.pico_sdk.version)
  CYW_HEADER_URL=$(nr_json_value "$ROOT/sources.lock.json" dependencies.pico_sdk.firmware_header_url)
  CYW_HEADER_HASH=$(nr_json_value "$ROOT/sources.lock.json" dependencies.pico_sdk.cyw43439_firmware_header_sha256)
  CYW_CACHE=$ROOT/build/deps/pico-sdk-${PICO_SDK_VERSION}/lib/cyw43-driver/firmware/w43439A0_7_95_49_00_combined.h
  PICO_SDK_NATIVE_PATH=$WORK/deps/pico-sdk
  CYW_STAGE=$PICO_SDK_NATIVE_PATH/lib/cyw43-driver/firmware/w43439A0_7_95_49_00_combined.h
  nr_note "staging checksum-verified Pico SDK ${PICO_SDK_VERSION} CYW43439 firmware"
  nr_download_verified "$CYW_HEADER_URL" "$CYW_HEADER_HASH" "$CYW_CACHE"
  mkdir -p "$(dirname -- "$CYW_STAGE")"
  install -m 0644 "$CYW_CACHE" "$CYW_STAGE"
  if ((USE_CONTAINER)); then
    PICO_SDK_BUILD_PATH=/work/deps/pico-sdk
  else
    PICO_SDK_BUILD_PATH=$PICO_SDK_NATIVE_PATH
  fi
  export PICO_SDK_PATH=$PICO_SDK_BUILD_PATH
fi

mkdir -p "$WORK/nuttx/$(dirname -- "$DEFCONFIG_DEST")"
install -m 0644 "$ROOT/$DEFCONFIG_REL" "$WORK/nuttx/$DEFCONFIG_DEST"

if ((USE_CONTAINER)); then
  BUILD_HOST=linux
else
  case $(uname -s) in
    Darwin) BUILD_HOST=macos ;;
    Linux) BUILD_HOST=linux ;;
    *) nr_die "native builds are supported only on macOS and Linux; use --container" ;;
  esac
fi

if [[ $BOARD == adafruit-fruit-jam-rp2350 && \
      -d $WORK/nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/examples_romfs ]]; then
  nr_note "generating staged Fruit Jam examples ROMFS"
  if ((USE_CONTAINER)); then
    nr_require_tool docker
    ROMFS_CONTAINER=$(nr_container_ref "$ROOT")
    docker run --rm \
      --platform "$CONTAINER_PLATFORM" \
      --user "$(id -u):$(id -g)" \
      -e HOME=/tmp \
      -e PICO_SDK_PATH="${PICO_SDK_BUILD_PATH:-/work/deps/pico-sdk}" \
      -e NUTTX_DIR=/work/nuttx \
      -v "$ROOT:/repo:ro" \
      -v "$WORK:/work" \
      "$ROMFS_CONTAINER" \
      sh /repo/scripts/generate-fruitjam-examples-romfs.sh
  else
    NUTTX_DIR=$WORK/nuttx "$SCRIPT_DIR/generate-fruitjam-examples-romfs.sh"
  fi
fi

nr_note "installing NuttX board configuration ${BOARD}:${BOARD_CONFIG}"
if ((USE_CONTAINER)); then
  CONFIGURE_CONTAINER=$(nr_container_ref "$ROOT")
  docker run --rm \
    --platform "$CONTAINER_PLATFORM" \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -e PICO_SDK_PATH="${PICO_SDK_BUILD_PATH:-/work/deps/pico-sdk}" \
    -v "$WORK:/work" \
    -w /work/nuttx \
    "$CONFIGURE_CONTAINER" \
    bash -lc "./tools/configure.sh -E -l -a ../apps '${BOARD}:${BOARD_CONFIG}'"
else
  if [[ -d $HOME/.local/nuttx-tools/kconfig-frontends/bin ]]; then
    PATH=$HOME/.local/nuttx-tools/kconfig-frontends/bin:$PATH
    export PATH
  fi
  (
    cd "$WORK/nuttx"
    if [[ $BUILD_HOST == linux ]]; then
      ./tools/configure.sh -E -l -a ../apps "${BOARD}:${BOARD_CONFIG}"
    else
      ./tools/configure.sh -E -m -a ../apps "${BOARD}:${BOARD_CONFIG}"
    fi
  )
fi
nr_normalize_host_config "$WORK/nuttx/.config" "$BUILD_HOST"
if [[ $NETWORKING == cyw43439 ]]; then
  nr_config_set "$WORK/nuttx/.config" CYW43439_FIRMWARE_HEADER_PATH \
    "\"${PICO_SDK_BUILD_PATH}/lib/cyw43-driver/firmware/w43439A0_7_95_49_00_combined.h\""
  nr_config_set "$WORK/nuttx/.config" CYW43439_FIRMWARE_BIN_PATH \
    "\"${PICO_SDK_BUILD_PATH}/lib/cyw43-driver/firmware/43439A0-7.95.49.00.combined\""
fi

if ((DEV_BOOTSEL)); then
  nr_note "generating non-release BOOTSEL recovery override"
  DEV_SUPPORTED=0
  DEV_BOARD_SUPPORTED=1

  if [[ $PROFILE != fruit-jam-full-fruitclaw ]] && \
     nr_config_symbol_known "$WORK" SYSTEM_RP2350WATCHDOG_BOOTSEL_RECOVERY; then
    nr_config_set "$WORK/nuttx/.config" SYSTEM_RP2350WATCHDOG_BOOTSEL_RECOVERY y
    nr_config_set "$WORK/nuttx/.config" SYSTEM_RP2350WATCHDOG_DEV_BOOTSEL_MS 600000
    DEV_SUPPORTED=1
  fi
  if [[ $PROFILE == fruit-jam-full-fruitclaw ]] && \
     nr_config_symbol_known "$WORK" FRUITCLAW_GUARD_BOOTSEL_RECOVERY; then
    nr_config_set "$WORK/nuttx/.config" FRUITCLAW_GUARD_BOOTSEL_RECOVERY y
    nr_config_set "$WORK/nuttx/.config" FRUITCLAW_MAX_UPTIME_GUARD_MS 600000
    DEV_SUPPORTED=1
  fi
  if [[ $BOARD == adafruit-fruit-jam-rp2350 ]]; then
    DEV_BOARD_SUPPORTED=0
    if nr_config_symbol_known "$WORK" ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD; then
      nr_config_set "$WORK/nuttx/.config" ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD y
      nr_config_set "$WORK/nuttx/.config" ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD_TIMEOUT_MS 600000
      nr_config_disable "$WORK/nuttx/.config" ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD_CDC_DISARM
      DEV_BOARD_SUPPORTED=1
    fi
  elif [[ $BOARD == raspberrypi-pico-2-w ]]; then
    DEV_BOARD_SUPPORTED=0
    if nr_config_symbol_known "$WORK" RP23XX_PICO_BOOTSEL_ON_WATCHDOG; then
      nr_config_set "$WORK/nuttx/.config" RP23XX_PICO_BOOTSEL_ON_WATCHDOG y
      DEV_BOARD_SUPPORTED=1
    fi
  fi

  ((DEV_SUPPORTED && DEV_BOARD_SUPPORTED)) || \
    nr_die "profile ${PROFILE} has no supported automatic BOOTSEL recovery symbol"
fi

WRAPPER_SHA=$(git -C "$ROOT" rev-parse HEAD)
SOURCE_DATE_EPOCH=$(nr_source_date_epoch "$ROOT")
export SOURCE_DATE_EPOCH TZ=UTC LC_ALL=C LANG=C

VERSION=${NUTTX_RP2350_VERSION:-}
if [[ -z $VERSION ]]; then
  VERSION=$(git -C "$ROOT" describe --exact-match --tags HEAD 2>/dev/null || true)
fi
if [[ -z $VERSION ]]; then
  VERSION=dev-${WRAPPER_SHA:0:12}
fi
[[ $VERSION =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || nr_die "unsafe artifact version: $VERSION"

NUTTX_VERSION=$(nr_json_value "$ROOT/sources.lock.json" \
  upstreams.nuttx.version sources.nuttx.version nuttx.version 2>/dev/null || printf '0.0.0\n')
NUTTX_VERSION=${NUTTX_VERSION#nuttx-}
[[ $NUTTX_VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+ ]] || NUTTX_VERSION=0.0.0
NUTTX_VERSION=${NUTTX_VERSION%%-*}
cat > "$WORK/nuttx/.version" <<EOF
#!/bin/bash

CONFIG_VERSION_STRING="${NUTTX_VERSION}"
CONFIG_VERSION_MAJOR=${NUTTX_VERSION%%.*}
CONFIG_VERSION_MINOR=$(printf '%s' "$NUTTX_VERSION" | cut -d. -f2)
CONFIG_VERSION_PATCH=$(printf '%s' "$NUTTX_VERSION" | cut -d. -f3)
CONFIG_VERSION_BUILD="${WRAPPER_SHA:0:12}-${PROFILE}"
EOF
chmod 0755 "$WORK/nuttx/.version"

run_native_build() {
  if [[ -d $HOME/.local/nuttx-tools/kconfig-frontends/bin ]]; then
    PATH=$HOME/.local/nuttx-tools/kconfig-frontends/bin:$PATH
    export PATH
  fi
  (
    cd "$WORK/nuttx"
    make olddefconfig
    if ((PREPARE_ONLY == 0)); then
      make -j"$JOBS"
    fi
  )
}

run_container_build() {
  local container
  nr_require_tool docker
  container=$(nr_container_ref "$ROOT")
  nr_note "using pinned build container ${container}"
  docker run --rm \
    --platform "$CONTAINER_PLATFORM" \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -e CCACHE_DIR=/tmp/ccache \
    -e CCACHE_TEMPDIR=/tmp/ccache-tmp \
    -e PICO_SDK_PATH="${PICO_SDK_BUILD_PATH:-/work/deps/pico-sdk}" \
    -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
    -e TZ=UTC \
    -e LC_ALL=C \
    -e LANG=C \
    -v "$WORK:/work" \
    -w /work/nuttx \
    "$container" \
    bash -lc "make olddefconfig && $([[ $PREPARE_ONLY == 1 ]] && printf ':' || printf 'make -j%s' "$JOBS")"
}

nr_note "configuring ${PROFILE} (${BOARD})"
if ((USE_CONTAINER)); then
  run_container_build
else
  run_native_build
fi

if ((DEV_BOOTSEL)); then
  if [[ $PROFILE == fruit-jam-full-fruitclaw ]]; then
    grep -q '^CONFIG_FRUITCLAW_GUARD_BOOTSEL_RECOVERY=y$' "$WORK/nuttx/.config" || \
      nr_die "olddefconfig rejected the FruitClaw development BOOTSEL override"
    grep -q '^CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS=600000$' "$WORK/nuttx/.config" || \
      nr_die "FruitClaw development BOOTSEL backstop is not 10 minutes"
  else
    grep -q '^CONFIG_SYSTEM_RP2350WATCHDOG_BOOTSEL_RECOVERY=y$' "$WORK/nuttx/.config" || \
      nr_die "olddefconfig rejected the RP2350 development BOOTSEL override"
    grep -q '^CONFIG_SYSTEM_RP2350WATCHDOG_DEV_BOOTSEL_MS=600000$' "$WORK/nuttx/.config" || \
      nr_die "RP2350 development BOOTSEL backstop is not 10 minutes"
  fi
  if [[ $BOARD == adafruit-fruit-jam-rp2350 ]]; then
    grep -q '^CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD=y$' "$WORK/nuttx/.config" || \
      nr_die "Fruit Jam development image lacks the BOOTSEL scratch-magic consumer"
  elif [[ $BOARD == raspberrypi-pico-2-w ]]; then
    grep -q '^CONFIG_RP23XX_PICO_BOOTSEL_ON_WATCHDOG=y$' "$WORK/nuttx/.config" || \
      nr_die "Pico 2 W development image lacks the BOOTSEL scratch-magic consumer"
  fi
else
  "$SCRIPT_DIR/check-profiles.py" --resolved-config "$PROFILE" "$WORK/nuttx/.config"
fi

if ((PREPARE_ONLY)); then
  nr_note "prepared ${PROFILE} in ${WORK}"
else
  [[ -f $WORK/nuttx/nuttx.uf2 ]] || nr_die "build completed without nuttx.uf2"
  SUFFIX=
  ((DEV_BOOTSEL)) && SUFFIX=-dev-bootsel
  ARTIFACT=$(nr_artifact_name "$VERSION" "$ARTIFACT_SLUG" "$SUFFIX")
  install -m 0644 "$WORK/nuttx/nuttx.uf2" "$DIST_ROOT/$ARTIFACT"

  CONFIG_HASH=$(nr_sha256 "$ROOT/$DEFCONFIG_REL")
  PATCH_HASH=$(nr_hash_tree "$ROOT/patches")
  OVERLAY_HASH=$(nr_hash_tree "$ROOT/overlays")
  ARTIFACT_HASH=$(nr_sha256 "$DIST_ROOT/$ARTIFACT")
  ARTIFACT_SIZE=$(wc -c < "$DIST_ROOT/$ARTIFACT" | tr -d '[:space:]')
  mkdir -p "$DIST_ROOT/build-records"
  nr_python - "$DIST_ROOT/build-records/${PROFILE}${SUFFIX}.json" \
    "$PROFILE" "$ARTIFACT" "$DEV_BOOTSEL" "$WRAPPER_SHA" \
    "$NUTTX_COMMIT" "$APPS_COMMIT" "$CONFIG_HASH" "$PATCH_HASH" \
    "$OVERLAY_HASH" "$SOURCE_DATE_EPOCH" "$ARTIFACT_SIZE" \
    "$ARTIFACT_HASH" <<'PY'
import json
import sys

record = {
    "schema_version": 1,
    "profile": sys.argv[2],
    "artifact": sys.argv[3],
    "development_bootsel": sys.argv[4] == "1",
    "wrapper_commit": sys.argv[5],
    "nuttx_commit": sys.argv[6],
    "apps_commit": sys.argv[7],
    "config_sha256": sys.argv[8],
    "patches_sha256": sys.argv[9],
    "overlays_sha256": sys.argv[10],
    "source_date_epoch": int(sys.argv[11]),
    "size": int(sys.argv[12]),
    "sha256": sys.argv[13],
}
with open(sys.argv[1], "w", encoding="utf-8") as stream:
    json.dump(record, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY
  nr_note "built ${DIST_ROOT}/${ARTIFACT}"
  printf '%s  %s\n' "$ARTIFACT_HASH" "$ARTIFACT"
fi

AFTER_STATUS=$(git -C "$ROOT" status --porcelain=v1 --untracked-files=all)
if [[ $AFTER_STATUS != "$BEFORE_STATUS" ]]; then
  printf '%s\n' "Repository state changed during build:" >&2
  diff -u <(printf '%s\n' "$BEFORE_STATUS") <(printf '%s\n' "$AFTER_STATUS") >&2 || true
  nr_die "build must not modify the wrapper or either submodule"
fi
