#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_root=${FC_DOCKER_BUILD_ROOT:-/tmp/fruitclaw-docker-build}
image=${FC_DOCKER_IMAGE:-ghcr.io/apache/nuttx/apache-nuttx-ci-linux:latest}
jobs=${FC_JOBS:-8}
do_configure=0
do_cold_configure=0

usage() {
  cat <<'EOF'
Usage: scripts/fruitclaw_docker_incremental.sh [--configure] [--cold-configure]

Incrementally rebuild the warm Docker NuttX tree used for FruitClaw.

Default:
  - rsync only FruitClaw-relevant source/config paths into /tmp/fruitclaw-docker-build
  - run make -j8 in Docker
  - copy nuttx.uf2 to artifacts/fruitclaw-esp-hosted-docker-latest.uf2

Use --configure after Kconfig/defconfig or selected-feature changes. It
refreshes .config with make olddefconfig and preserves compiled objects.

Use --cold-configure only when the warm tree is stale enough that a deliberate
distclean is worth the rebuild time.
EOF
}

while (($#)); do
  case "$1" in
    --configure)
      do_configure=1
      ;;
    --cold-configure)
      do_configure=1
      do_cold_configure=1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

if [[ ! -d "$build_root/nuttx" || ! -d "$build_root/apps" ]]; then
  cat >&2 <<EOF
Warm Docker tree not found at:
  $build_root

Create it once with the clean Docker configure/build flow, then use this helper
for normal FruitClaw iterations.
EOF
  exit 1
fi

sync_path() {
  local rel=$1
  if [[ -e "$repo_root/$rel" ]]; then
    mkdir -p "$build_root/$(dirname "$rel")"
    rsync -a --delete \
      --exclude='*.o' \
      --exclude='*.a' \
      --exclude='*.d' \
      --exclude='*.su' \
      --exclude='.depend' \
      --exclude='Make.dep' \
      "$repo_root/$rel" "$build_root/$rel"
  fi
}

sync_path apps/system/fruitclaw/
sync_path apps/system/bootguard/
sync_path apps/system/dvictrl/
sync_path apps/system/readline/
sync_path apps/system/Kconfig
sync_path apps/system/Make.defs

# FruitClaw's operator profile intentionally pulls in several app families.
# Keep this list explicit so incremental builds do not accidentally depend on
# stale files left in the warm Docker tree.
sync_path apps/examples/lvgldemo/Kconfig
sync_path apps/examples/lvgldemo/lvgldemo.c
sync_path apps/examples/webserver/
sync_path apps/examples/xbc_test/
sync_path apps/games/cgol/
sync_path apps/games/NXDoom/Kconfig
sync_path apps/games/brickmatch/Kconfig
sync_path apps/games/match4/Kconfig
sync_path apps/games/snake/Kconfig
sync_path apps/games/Kconfig
sync_path apps/games/Make.defs
sync_path apps/graphics/lvgl/Makefile
sync_path apps/graphics/lvgl/CMakeLists.txt
sync_path apps/include/netutils/httpd.h
sync_path apps/interpreters/berry/0001-Fix-Berry-default-port-for-NuttX.patch
sync_path apps/interpreters/berry/CMakeLists.txt
sync_path apps/interpreters/berry/Kconfig
sync_path apps/interpreters/berry/Make.defs
sync_path apps/interpreters/berry/Makefile
sync_path apps/interpreters/berry/be_lvgl.c
sync_path apps/interpreters/berry/berry_runner.c
sync_path apps/interpreters/berry/include/berry_conf.h
sync_path apps/interpreters/berry/include/berry_runner.h
sync_path apps/netutils/netlib/netlib_server.c
sync_path apps/netutils/webserver/

sync_path nuttx/arch/arm/src/rp23xx/Kconfig
sync_path nuttx/arch/arm/src/rp23xx/rp23xx_dvi.c
sync_path nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/Kconfig
sync_path nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/PSRAM.md
sync_path nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/README.md
sync_path nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/TRMNL_DVI_TIMING.md
sync_path nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/configs/esp-hosted/defconfig
sync_path nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/configs/hstxdvi/
sync_path nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/configs/lvgl/
sync_path nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/configs/nxdoom/
sync_path nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/examples_romfs/README.txt
sync_path nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/examples_romfs/lvgl_counter.be
sync_path nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/examples_romfs/lvgl_smoke.be
sync_path nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/include/board.h
sync_path nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/src/etc/init.d/rcS
sync_path nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/src/rp23xx_boardinitialize.c
sync_path nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/src/rp23xx_bringup.c
sync_path nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/src/rp23xx_examples_romfs.c
sync_path nuttx/boards/arm/rp23xx/common/src/rp23xx_spisd.c
sync_path nuttx/drivers/net/telnet.c
sync_path nuttx/drivers/usbhost/usbhost_xboxcontroller.c
sync_path nuttx/include/nuttx/input/xbox-controller.h

normalize_docker_kconfig() {
  docker run --rm \
    -v "$build_root:/work" \
    -w /work \
    "$image" \
    bash -lc \
      'find /work/apps -mindepth 2 -maxdepth 2 -name Kconfig -type f -print0 |
       xargs -0 perl -0pi -e '"'"'s|source "/[^"]*/apps/|source "/work/apps/|g; s|source "(?!/)([^"]+)|source "/work/apps/$1|g'"'"''
}

newer_config_input() {
  local config=$build_root/nuttx/.config
  local path
  local newer

  [[ -f "$config" ]] || return 0

  local inputs=(
    "$build_root/apps/system"
    "$build_root/apps/examples/lvgldemo"
    "$build_root/apps/examples/webserver"
    "$build_root/apps/examples/xbc_test"
    "$build_root/apps/games"
    "$build_root/apps/graphics/lvgl"
    "$build_root/apps/interpreters/berry"
    "$build_root/apps/netutils/webserver"
    "$build_root/nuttx/arch/arm/src/rp23xx"
    "$build_root/nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/Kconfig"
    "$build_root/nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/configs/esp-hosted/defconfig"
  )

  for path in "${inputs[@]}"; do
    if [[ -d "$path" ]]; then
      newer=$(find "$path" \( -name Kconfig -o -name defconfig \) -type f -newer "$config" -print -quit)
      if [[ -n "$newer" ]]; then
        printf '%s\n' "$newer"
        return 0
      fi
    elif [[ -f "$path" && "$path" -nt "$config" ]]; then
      printf '%s\n' "$path"
      return 0
    fi
  done

  return 1
}

reset_generated_etc_romfs() {
  rm -rf "$build_root/nuttx/boards/arm/rp23xx/common/etctmp"
  rm -f "$build_root/nuttx/boards/arm/rp23xx/common/etctmp.c"
  rm -f "$build_root/nuttx/boards/arm/rp23xx/common/etctmp.o"
}

maybe_reset_generated_etc_romfs() {
  local generated=$build_root/nuttx/boards/arm/rp23xx/common/etctmp/etc/init.d/rcS
  local config=$build_root/nuttx/.config
  local source=$build_root/nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/src/etc/init.d/rcS

  if [[ ! -f "$generated" ]] ||
     [[ -f "$config" && "$config" -nt "$generated" ]] ||
     [[ -f "$source" && "$source" -nt "$generated" ]]; then
    echo "Refreshing generated /etc ROMFS startup scripts"
    reset_generated_etc_romfs
  fi
}

if (( do_configure )); then
  if (( do_cold_configure )) || [[ ! -f "$build_root/nuttx/.config" ]]; then
    normalize_docker_kconfig
    docker run --rm \
      -v "$build_root:/work" \
      -w /work/nuttx \
      "$image" \
      bash -lc './tools/configure.sh -E -l -a ../apps adafruit-fruit-jam-rp2350:esp-hosted'
    reset_generated_etc_romfs
  elif newer=$(newer_config_input); then
    echo "Config input changed since warm .config: ${newer#$build_root/}"
    normalize_docker_kconfig
    docker run --rm \
      -v "$build_root:/work" \
      -w /work/nuttx \
      "$image" \
      bash -lc 'set -euo pipefail
        cp boards/arm/rp23xx/adafruit-fruit-jam-rp2350/configs/esp-hosted/defconfig .config
        perl -0pi -e '"'"'s/^CONFIG_HOST_MACOS=y$/# CONFIG_HOST_MACOS is not set/m;
                       s/^# CONFIG_HOST_LINUX is not set$/CONFIG_HOST_LINUX=y/m;
                       $_ .= "\nCONFIG_HOST_LINUX=y\n" unless /^CONFIG_HOST_LINUX=y$/m'"'"' .config
        make olddefconfig'
    reset_generated_etc_romfs
  else
    echo "Warm .config is current; skipping olddefconfig"
  fi
fi

maybe_reset_generated_etc_romfs

docker run --rm \
  -v "$build_root:/work" \
  -w /work/nuttx \
  "$image" \
  bash -lc "make -j${jobs}"

stamp=$(date -u +%Y%m%d-%H%M%S)
mkdir -p "$repo_root/artifacts"
install -m 0644 "$build_root/nuttx/nuttx.uf2" \
  "$repo_root/artifacts/fruitclaw-esp-hosted-mcp-incremental-${stamp}.uf2"
install -m 0644 "$build_root/nuttx/nuttx.uf2" \
  "$repo_root/artifacts/fruitclaw-esp-hosted-docker-latest.uf2"

shasum -a 256 \
  "$repo_root/artifacts/fruitclaw-esp-hosted-mcp-incremental-${stamp}.uf2" \
  "$repo_root/artifacts/fruitclaw-esp-hosted-docker-latest.uf2"
