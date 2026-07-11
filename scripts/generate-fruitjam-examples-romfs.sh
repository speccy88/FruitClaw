#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
NUTTX_DIR=${NUTTX_DIR:-"$ROOT/nuttx"}
BOARD_DIR="$NUTTX_DIR/boards/arm/rp23xx/adafruit-fruit-jam-rp2350"
SOURCE_DIR="$BOARD_DIR/examples_romfs"
OUTPUT_C="$BOARD_DIR/src/rp23xx_examples_romfs.c"
TMPDIR_ROMFS=$(mktemp -d "${TMPDIR:-/tmp}/fruitjam-examples-romfs.XXXXXX")

cleanup()
{
  rm -rf "$TMPDIR_ROMFS"
}

trap cleanup EXIT HUP INT TERM

if [ ! -d "$SOURCE_DIR" ]; then
  echo "Fruit Jam examples ROMFS source not found: $SOURCE_DIR" >&2
  exit 1
fi

if ! command -v genromfs >/dev/null 2>&1; then
  echo "genromfs is required to rebuild the Fruit Jam examples ROMFS" >&2
  exit 1
fi

if ! command -v xxd >/dev/null 2>&1; then
  echo "xxd is required to rebuild the Fruit Jam examples ROMFS" >&2
  exit 1
fi

genromfs \
  -f "$TMPDIR_ROMFS/examples.img" \
  -d "$SOURCE_DIR" \
  -V FruitJamExamples

{
  printf '%s\n' \
    '/****************************************************************************' \
    ' * boards/arm/rp23xx/adafruit-fruit-jam-rp2350/src/rp23xx_examples_romfs.c' \
    ' *' \
    ' * SPDX-License-Identifier: Apache-2.0' \
    ' *' \
    ' * This file was generated from the board examples_romfs directory.' \
    ' *' \
    ' ****************************************************************************/' \
    '' \
    '#include <nuttx/compiler.h>' \
    ''

  xxd -i -n rp23xx_romfs_img "$TMPDIR_ROMFS/examples.img" | sed \
    -e 's/^unsigned char rp23xx_romfs_img/const unsigned char aligned_data(4) rp23xx_romfs_img/' \
    -e 's/^unsigned int rp23xx_romfs_img_len/const unsigned int rp23xx_romfs_img_len/'
} > "$TMPDIR_ROMFS/rp23xx_examples_romfs.c"

mv "$TMPDIR_ROMFS/rp23xx_examples_romfs.c" "$OUTPUT_C"

bytes=$(wc -c < "$TMPDIR_ROMFS/examples.img" | tr -d ' ')
echo "Generated $OUTPUT_C ($bytes-byte ROMFS image)"
