# Build And Packaging

This checkout contains an Apache NuttX tree plus `nuttx-apps`. The FruitClaw
target profile is:

```text
adafruit-fruit-jam-rp2350:esp-hosted
```

## Normal Configure And Build

From the source checkout:

```sh
export PATH="$HOME/.local/nuttx-tools/kconfig-frontends/bin:$PATH"
cd nuttx
./tools/configure.sh -E -m -a ../apps adafruit-fruit-jam-rp2350:esp-hosted
make olddefconfig
make -j8
```

The generated RP2350 UF2 is:

```text
nuttx/nuttx.uf2
```

## Docker Incremental Build

This checkout also has a helper used during bring-up:

```sh
./scripts/fruitclaw_docker_incremental.sh
```

It syncs the FruitClaw app, DVI control app, webserver sources, selected board
sources, the `esp-hosted` defconfig, and the `apps/games/cgol` files into the
warm Docker build tree. Use `--configure` only after Kconfig/defconfig or
selected-feature changes.

The helper writes:

```text
artifacts/fruitclaw-esp-hosted-docker-latest.uf2
```

## Framebuffer Profile Switch

The default `esp-hosted` image keeps the LVGL-friendly display profile:

```text
CONFIG_RP23XX_HSTX_DVI_FB_RGB565_320X240=y
CONFIG_RP23XX_HSTX_DVI_OUTPUT_SCALING=1
CONFIG_RP23XX_HSTX_DVI_PIXEL_CLOCK=25200000
CONFIG_RP23XX_HSTX_DVI_SCANOUT_PSRAM=y
CONFIG_RP23XX_HSTX_DVI_SINGLE_SCANOUT_INTERNAL=y
CONFIG_READLINE_CMD_HISTORY_LINELEN=1024
CONFIG_READLINE_CMD_HISTORY_LEN=8
```

FruitClaw intentionally does not enable the TRMNL app or the experimental
800x480 Y2 framebuffer in this profile. For future TRMNL-style grayscale work,
use the separate `trmnl` or `hstxdvi` profile and switch the DVI framebuffer
profile at configure time:

```text
CONFIG_RP23XX_HSTX_DVI_FB_Y2_800X480=y
CONFIG_RP23XX_HSTX_DVI_PIXEL_CLOCK=25200000
CONFIG_RP23XX_HSTX_DVI_SCANOUT_PSRAM=y
CONFIG_RP23XX_HSTX_DVI_SINGLE_SCANOUT_INTERNAL=y
CONFIG_RP23XX_HSTX_DVI_Y2_OUTPUT_NATIVE_800X480=y
```

After changing this, run `make olddefconfig` or the Docker helper with
`--configure`. The Y2 profile exposes `/dev/fb0` as 800x480 2-bit grayscale
and is not the current FruitClaw RGB565 profile. The tested TRMNL timing is
the tight 800x480 mode at 25.2 MHz; the older 31.5 MHz experiment was blank on
the validated monitor and should not be used as the baseline.

Do not remove the FruitClaw single internal scanout flag casually. A PSRAM-only
scanout build registered `/dev/fb0` and reported clean DVI counters, but the
validated HDMI monitor still showed no signal. The current build frees enough
internal SRAM for the live scanout by reducing shell history depth to 8 while
keeping 1024-byte command lines.

## Web Manual Packaging

The port 80 manual source root is:

```text
apps/examples/webserver/httpd-fs
```

The browser shell and Markdown pages are:

```text
index.html
docs/index.html
docs/index.json
docs/_nav.md
docs/pages/*.md
```

The embedded web filesystem C image is:

```text
apps/examples/webserver/httpd_fsdata.c
```

`apps/examples/webserver/Makefile` regenerates it from `httpd-fs`:

```text
$(TOPDIR)/tools/mkfsdata.pl
```

If you edit any web file, regenerate `httpd_fsdata.c`, rebuild, and flash.
Otherwise the firmware will still serve the old compiled copy.

## Flashing The RP2350

When the board is in BOOTSEL:

```sh
picotool load -x nuttx/nuttx.uf2
```

or, for the Docker helper artifact:

```sh
picotool load -x artifacts/fruitclaw-esp-hosted-docker-latest.uf2
```

When the running app is reachable, NSH has a `bootsel` app that can reboot the
board into ROM BOOTSEL for reflashing:

```sh
bootsel
```

`picotool info` is the quick host-side proof that the board is really in
BOOTSEL.

## ESP32-C6 Coprocessor

The ESP-Hosted setup needs both chips programmed:

- ESP32-C6 coprocessor: ESP-Hosted-MCU merged `.bin`.
- RP2350 host: NuttX `.uf2`.

The ESP32-C6 firmware path is documented under:

```text
nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/esp-hosted-mcu/README.md
```

That flow uses Adafruit's RP2350 serial-passthrough UF2 as a temporary bridge
and `python -m esptool --chip esp32c6 ... write_flash 0 build/merged-binary.bin`.

## Post-Flash Checks

After flashing:

```sh
help
fruitclaw status
curl http://DEVICE_IP/
curl -i http://DEVICE_IP/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"ping"}'
```

For a display check:

```sh
dvictrl start
cgol
```

Sources: `README.md`, `apps/system/fruitclaw/README.md`,
`apps/examples/webserver/Makefile`, `nuttx/tools/mkfsdata.pl`,
`scripts/fruitclaw_docker_incremental.sh`,
`nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/configs/esp-hosted/defconfig`.
