# FruitClaw

FruitClaw is an Apache NuttX workspace for the Adafruit Fruit Jam RP2350.  The
current preview image turns the board into a small owner-mode operator node:
USB NSH, ESP-Hosted Wi-Fi, a static HTTP documentation/wiki server, a YOLO MCP
endpoint, Telegram/DeepSeek agent plumbing, Berry scripting, LVGL/Berry
bindings, NeoPixels, scheduler tools, Telnet/FTP service control, USB
keyboard/mouse/Xbox input, and guarded recovery.

This repository is a wrapper around two source submodules so the NuttX history
stays clean and rebuildable:

- `apps`: `https://github.com/speccy88/FruitClaw-apps.git`
- `nuttx`: `https://github.com/speccy88/FruitClaw-nuttx.git`

## Current Preview Release

The current FruitClaw operator preview UF2 is:

```text
artifacts/fruitclaw-operator-alpha-preview-20260703-163832.uf2
```

SHA-256:

```text
7b66004a230890e0989e1f84351f4978a92ad74fc957345c16091ee44fad0085
```

It was built from:

```text
FruitClaw wrapper: the GitHub release tag records the exact wrapper commit
apps submodule:    2f0ae270e5e0a369f632bf2bb4e71e9636a72e42
nuttx submodule:   a21db85c758aae9ecce210b9f3e12168f39b5548
profile:           adafruit-fruit-jam-rp2350:esp-hosted
```

Build size from `arm-none-eabi-size nuttx/nuttx`:

```text
text=1179648  data=272  bss=206824  total=1386744 bytes
```

Linker memory report:

```text
FLASH: 1179920 B / 16 MB  (7.03%)
RAM:    240888 B / 512 KB (45.95%)
```

Validation for this preview:

- Built with `make -C nuttx -j8` after regenerating the embedded uIP
  webserver filesystem.
- `nuttx/nuttx.uf2` matches
  `artifacts/fruitclaw-operator-alpha-preview-20260703-163832.uf2` byte for
  byte.
- The previous same-runtime preview image was flashed to an RP2350B Fruit Jam
  with `picotool load -x` and booted with CDC console on `/dev/cu.usbmodem01`.
- On that flashed image, `fruitclaw selftest` passed and `fruitclaw status`
  reported `bootstrap: ready`, webserver supervisor
  listening, MCP enabled, Berry enabled, scheduler enabled, NeoPixels enabled,
  device tools enabled, and no last tool error.
- `device.read` rejects raw block devices before opening them, so MCP/Telegram
  cannot wedge the board by reading `/dev/mmcsd0`.

## Important Preview Behavior

This is an alpha/operator preview, not a locked-down community production image.

- MCP is YOLO owner mode: no bearer token, no read-only policy layer, and MCP
  calls run with `owner_mode=true`.
- The image intentionally enables a 10-minute development max-uptime guard:
  `CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS=600000`.
- Guarded failures recover to ROM BOOTSEL in this preview:
  `CONFIG_FRUITCLAW_GUARD_BOOTSEL_RECOVERY=y`.
- Production/community images should normally keep watchdog resets but disable
  the 10-minute max-uptime guard and BOOTSEL conversion.
- Secrets are runtime files only. Do not put Wi-Fi, Telegram, DeepSeek, or other
  tokens in source, defconfig, README examples, or release notes.
- TRMNL is not enabled in this FruitClaw preview. TRMNL source/configs live in
  the tree for the separate display-client work, but `CONFIG_SYSTEM_TRMNL` is
  off here.

## Display Profile

FruitClaw uses the safe color DVI profile, not the TRMNL 800x480 grayscale
profile:

```text
CONFIG_RP23XX_HSTX_DVI_FB_RGB565_320X240=y
# CONFIG_RP23XX_HSTX_DVI_FB_Y2_800X480 is not set
CONFIG_RP23XX_HSTX_DVI_PIXEL_CLOCK=25200000
CONFIG_RP23XX_HSTX_DVI_SCANOUT_PSRAM=y
```

The 25.2 MHz DVI pixel clock and PSRAM scanout setting come from the TRMNL/HSTX
bring-up notes, but this operator build stays on the reliable 320x240 RGB565
path for LVGL, CGOL, and general board work.

TRMNL-specific timing notes are in:

```text
nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/TRMNL_DVI_TIMING.md
```

## Installed User-Facing Features

The `esp-hosted` preview profile currently enables:

- `fruitclaw`: operator agent CLI, boot supervisor, MCP route, Telegram/DeepSeek
  hooks, scheduler, memory, sessions, Berry wrapper, service control, and tools.
- uIP webserver on port 80 with a static Markdown wiki under the web root.
- MCP Streamable HTTP endpoint at `/mcp`.
- Telnet server (`telnetd`) and FTP server (`ftpd_start`/`ftpd_stop`) with
  FruitClaw service supervisor support.
- Berry interpreter plus FruitClaw Berry runner and LVGL bindings.
- LVGL 9.2.2 app support and example Berry LVGL scripts in board ROMFS.
- USB host for keyboard, mouse, hub, composite devices, and Xbox controller.
- `vi`, `cgol` Conway's Game of Life, `rtttl`, `neopixels`, `dvictrl`,
  `piousbhost`, `wapi`, `renew`, `ping`, `wget`, `ntpc`, `i2c`, `spi`, and NSH.
- NeoPixels on `/dev/leds0`.
- Device tools for bounded non-block `/dev` access. Raw block devices such as
  `/dev/mmcsd*`, `/dev/ram*`, `/dev/mtd*`, and `/dev/smart*` are denied.

The running board is the source of truth. Use:

```sh
fruitclaw status
fruitclaw tools
help
```

## Clone

Use recursive submodules:

```sh
git clone --recurse-submodules https://github.com/speccy88/FruitClaw.git
cd FruitClaw
```

If the checkout already exists:

```sh
git submodule update --init --recursive
git -C apps checkout master
git -C nuttx checkout master
git -C apps pull --ff-only fruitclaw master
git -C nuttx pull --ff-only fruitclaw master
```

## Build The Preview UF2

Native macOS/Linux build:

```sh
export PATH="$HOME/.local/nuttx-tools/kconfig-frontends/bin:$PATH"
cd nuttx
./tools/configure.sh -E -m -a ../apps adafruit-fruit-jam-rp2350:esp-hosted
make -j8
```

The generated UF2 is:

```text
nuttx/nuttx.uf2
```

After Kconfig or defconfig changes:

```sh
export PATH="$HOME/.local/nuttx-tools/kconfig-frontends/bin:$PATH"
cd nuttx
make olddefconfig
make -j8
```

## Docker Incremental Builds

The helper below is for a warm Docker build tree. It avoids rebuilding the whole
world when only FruitClaw-relevant paths changed:

```sh
scripts/fruitclaw_docker_incremental.sh --configure
scripts/fruitclaw_docker_incremental.sh
```

The helper expects a warm tree at `/tmp/fruitclaw-docker-build` by default and
uses `ghcr.io/apache/nuttx/apache-nuttx-ci-linux:latest` unless
`FC_DOCKER_IMAGE` is set. It copies the warm result to:

```text
artifacts/fruitclaw-esp-hosted-docker-latest.uf2
```

## Flash The RP2350 UF2

With the board in BOOTSEL:

```sh
picotool load -x nuttx/nuttx.uf2
```

Or flash the release asset:

```sh
picotool load -x artifacts/fruitclaw-operator-alpha-preview-20260703-163832.uf2
```

Open the USB console:

```sh
ls /dev/cu.usbmodem*
screen /dev/cu.usbmodem01 115200
```

Useful first commands:

```sh
uname -a
fruitclaw status
fruitclaw selftest
dvictrl info
help
```

## Runtime Storage

FruitClaw prefers SD storage when mounted and falls back to tmpfs:

```text
/mnt/sd0/fruitclaw
/data/fruitclaw
```

Typical runtime files:

```text
system.md
user.md
memory.jsonl
schedules.json
router_rules.json
telegram_offset
telegram_allowed_chats.txt
sessions/
scripts/
secrets/
services/
```

The selftest run in this release used `/data/fruitclaw` because SD was not
mounted in that session.

## Configure Wi-Fi And Secrets

Do not commit real credentials. Enter them on the board:

```sh
fruitclaw config set-wifi "<ssid>" "<password>"
fruitclaw config set-secret telegram "<telegram-bot-token>"
fruitclaw config set-secret deepseek "<deepseek-api-key>"
```

If no values are passed, the command prompts on the serial console:

```sh
fruitclaw config set-wifi
fruitclaw config set-secret telegram
fruitclaw config set-secret deepseek
```

Check the result without printing secret values:

```sh
fruitclaw config
fruitclaw wifi-up
ifconfig wlan0
ping -c 3 1.1.1.1
```

The preview seeds the known owner chat ID into
`telegram_allowed_chats.txt` when the data root is initialized. To discover or
change the allowed chat list:

```sh
fruitclaw telegram-discover
cat /data/fruitclaw/telegram_allowed_chats.txt
echo "<numeric-chat-id>" > /data/fruitclaw/telegram_allowed_chats.txt
```

Use `/mnt/sd0/fruitclaw/...` instead of `/data/fruitclaw/...` when SD is the
active data root.

## Run FruitClaw

The preview autostarts `fruitclaw boot` from `rcS`. You can also run the main
foreground service manually:

```sh
fruitclaw start
```

Useful checks:

```sh
fruitclaw status
fruitclaw tools
fruitclaw telegram-test
fruitclaw deepseek-test
fruitclaw terminal-run uname -a
fruitclaw terminal-run ls /dev
fruitclaw neopixels blue
fruitclaw neopixels off
fruitclaw service status
fruitclaw service start ftpd
fruitclaw service start telnetd
```

MCP endpoint:

```text
http://<board-ip>/mcp
```

Documentation/wiki endpoint:

```text
http://<board-ip>/
```

## ESP32-C6 ESP-Hosted Firmware

FruitClaw Wi-Fi expects the Fruit Jam ESP32-C6 to run ESP-Hosted-MCU, not the
stock NINA/AirLift firmware.

The last published ESP-Hosted release assets are:

| File | Purpose | SHA-256 |
| --- | --- | --- |
| `artifacts/fruitclaw-esp32c6-esp-hosted-mcu-20260629.bin` | ESP32-C6 ESP-Hosted-MCU merged flash image. | `1a1b35659dd62f44fa8c91b3e03f3fec80886072d04aeb6f192823eb28921c08` |
| `artifacts/fruitclaw-esp-hosted-wlan0-20260629.uf2` | Earlier RP2350 NuttX ESP-Hosted host image. | `c2ccb00bed4b264fd60c389ee6c3125827ea4ef4188a120b233a195a7d8ce615` |

The ESP32-C6 image was built from upstream `espressif/esp-hosted-mcu` commit:

```text
8f0770d39065c2a9ff6828268709c3502e0d5349
```

with the Fruit Jam overlay at:

```text
nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/esp-hosted-mcu/
```

Why ESP-Hosted: the stock NINA/AirLift model makes the coprocessor own the
socket stack. ESP-Hosted makes the ESP32-C6 a Wi-Fi radio/data coprocessor so
NuttX owns DHCP, DNS, sockets, services, diagnostics, and `wlan0`.

To flash the ESP32-C6, temporarily flash Adafruit's SerialESPPassthrough UF2 to
the RP2350, then use `esptool`:

```sh
python3 -m esptool --chip esp32c6 --before no_reset --after no_reset \
  -p /dev/cu.usbmodem<PASSTHROUGH> -b 115200 \
  write_flash 0 fruitclaw-esp32c6-esp-hosted-mcu-20260629.bin
```

Then put the RP2350 back into BOOTSEL and flash the FruitClaw UF2.

To revert the coprocessor, follow Adafruit's AirLift firmware guide and flash
the current `NINA_ADAFRUIT-fruitjam_c6-<version>.bin`.

## Documentation Locations

More detail lives in the source tree:

- FruitClaw app manual:
  `apps/system/fruitclaw/README.md`
- Browser wiki served by the embedded webserver:
  `apps/examples/webserver/httpd-fs/wiki/`
- Fruit Jam board notes:
  `nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/README.md`
- PSRAM notes:
  `nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/PSRAM.md`
- TRMNL/HSTX timing notes:
  `nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/TRMNL_DVI_TIMING.md`
- ESP-Hosted board notes:
  `nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/ESP_HOSTED.md`

## Known Alpha Gaps

- Telegram and DeepSeek need runtime credentials before end-to-end chat works.
- TLS is allowed unverified for bring-up unless CA roots are installed.
- SD must be mounted for persistent `/mnt/sd0/fruitclaw`; otherwise the board
  falls back to `/data/fruitclaw`.
- MCP is intentionally dangerous YOLO mode in this preview.
- Local graphical UI polish is still future work; the current web UI is static
  docs and the board display profile is the safe 320x240 RGB565 DVI mode.
- The 10-minute BOOTSEL max-uptime guard is a development setting, not a
  production default.
