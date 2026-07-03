# FruitClaw

FruitClaw is an Apache NuttX release workspace for the Adafruit Fruit Jam
RP2350. The production operator image turns the board into a small owner-mode
NuttX node with USB NSH, ESP-Hosted Wi-Fi, a Water.css documentation site,
YOLO MCP, Telegram/DeepSeek agent plumbing, Berry scripting, LVGL/Berry
bindings, NeoPixels, scheduler tools, Telnet/FTP service control, USB
keyboard/mouse/Xbox input, and watchdog recovery.

This repository is a wrapper around two source submodules so the NuttX and
apps histories stay clean and rebuildable:

- `apps`: `https://github.com/speccy88/FruitClaw-apps.git`
- `nuttx`: `https://github.com/speccy88/FruitClaw-nuttx.git`

## Current Production Release

The current FruitClaw operator UF2 is:

```text
artifacts/fruitclaw-operator-production-20260703-170751.uf2
```

SHA-256:

```text
4c7711a8243c78a7e1d8adabe632f022fb2a4511128f3247f17135c023faa59b
```

It is built from:

```text
FruitClaw wrapper: the GitHub release tag records the exact wrapper commit
apps submodule:    5451a14dbdf403ee158fd0851058b09bd1ff2c84
nuttx submodule:   6f193626230efe454f43913d7176b698ec38148d
profile:           adafruit-fruit-jam-rp2350:esp-hosted
```

Build size from `arm-none-eabi-size nuttx/nuttx`:

```text
text=1196044  data=272  bss=206876  total=1403192 bytes
```

Linker memory report:

```text
FLASH: 1196316 B / 16 MB  (7.13%)
RAM:    240940 B / 512 KB (45.96%)
```

Validation for this production release:

- Built with `make -C nuttx -j8` after regenerating the embedded uIP webserver
  filesystem.
- `nuttx/nuttx.uf2` matches the release asset byte for byte.
- The release profile disables the development max-uptime BOOTSEL fuse.
- Guarded runtime failures use watchdog reset back into NuttX/FruitClaw.
- `device.read` rejects raw block devices before opening them, so MCP/Telegram
  cannot wedge the board by reading `/dev/mmcsd0`.

## Production Behavior

FruitClaw is intentionally an owner-mode operator image, not a locked-down
consumer appliance.

- MCP is YOLO owner mode: no bearer token, no read-only policy layer, and MCP
  calls run with `owner_mode=true`.
- Local CLI and allowlisted Telegram chats are owner-capable by design.
- Watchdog guards stay enabled for risky operations.
- The development max-uptime timer is off:
  `CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS=0`.
- Automatic BOOTSEL recovery is off:
  `# CONFIG_FRUITCLAW_GUARD_BOOTSEL_RECOVERY is not set`.
- A guarded hang should reset back into the FruitClaw app. Use the `bootsel`
  command only when you intentionally want ROM BOOTSEL for flashing.
- Secrets are runtime files only. Do not put Wi-Fi, Telegram, DeepSeek, or other
  tokens in source, defconfig, README examples, or release notes.
- TRMNL is not enabled in this FruitClaw image. TRMNL source/configs live in
  the tree for the separate display-client work, but `CONFIG_SYSTEM_TRMNL` is
  off here.

## Flash The Release UF2

With the board in BOOTSEL:

```sh
picotool load -x artifacts/fruitclaw-operator-production-20260703-170751.uf2
```

Or flash a local build:

```sh
picotool load -x nuttx/nuttx.uf2
```

Open the USB console:

```sh
ls /dev/cu.usbmodem*
screen /dev/cu.usbmodem01 115200
# or: tio /dev/cu.usbmodem01
```

The production profile leaves NSH echoback enabled, so a default terminal
session should show typed characters and clean line returns. If an older alpha
image hides typing or repeats prompts on one line, rebuild with
`CONFIG_NSH_DISABLE_ECHOBACK` unset.

Useful first commands:

```sh
uname -a
fruitclaw status
fruitclaw selftest
dvictrl info
help
```

## Use The Board

The release autostarts `fruitclaw boot` from `rcS`. You can also run the main
foreground service manually:

```sh
fruitclaw start
```

The most useful status commands are:

```sh
fruitclaw status
fruitclaw config
fruitclaw tools
fruitclaw mcp status
fruitclaw service status
```

The default web page is:

```text
http://<board-ip>/
```

It is a small Water.css page whose Markdown body can be edited from the
FruitClaw data root or through the `web.home.write` MCP/agent tool.

The full browser manual is:

```text
http://<board-ip>/docs/
```

The `/docs/` manual is also Water.css themed. It is split into small Markdown
pages so the microcontroller only serves static files and the browser renders
Markdown client-side with Marked and DOMPurify. `/doc/` is kept as a small
compatibility redirect.

The MCP endpoint is:

```text
POST http://<board-ip>/mcp
```

Minimal MCP check:

```sh
curl -i http://<board-ip>/mcp \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"ping"}'
```

Expected response:

```json
{"jsonrpc":"2.0","id":1,"result":{}}
```

## Configure Wi-Fi And Secrets

Do not commit real credentials. Enter them on the board:

```sh
fruitclaw config set-wifi "<ssid>" "<password>"
fruitclaw config set-secret telegram "<telegram-bot-token>"
fruitclaw config set-secret deepseek "<deepseek-api-key>"
```

If no values are passed, the commands prompt on the serial console:

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

On boot, FruitClaw looks for Wi-Fi credentials in this order:

```text
CONFIG_FRUITCLAW_WIFI_CONFIG_PATH, when set to an absolute path
<active-data-root>/wifi.conf
/mnt/sd0/fruitclaw/wifi.conf
/data/fruitclaw/wifi.conf
```

The profile seeds the known owner chat ID into `telegram_allowed_chats.txt` on
first data-root initialization. To discover or change the allowlist:

```sh
fruitclaw telegram-discover
cat /data/fruitclaw/telegram_allowed_chats.txt
echo "<numeric-chat-id>" > /data/fruitclaw/telegram_allowed_chats.txt
```

Use `/mnt/sd0/fruitclaw/...` instead of `/data/fruitclaw/...` when SD is the
active data root.

## Runtime Storage

FruitClaw prefers SD storage when mounted and writable, and falls back to tmpfs
when SD is absent or unhealthy:

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
wifi.conf
http_allowlist.txt
certs/roots.pem
sessions/
scripts/
secrets/
services/
www/home.md
```

The root web page serves `/site/home.md`, backed by `www/home.md` in the active
data root. The default page appears if that file is absent.

## Installed User-Facing Features

The `adafruit-fruit-jam-rp2350:esp-hosted` production profile enables:

- `fruitclaw`: operator agent CLI, boot supervisor, MCP route, Telegram/DeepSeek
  hooks, scheduler, memory, sessions, Berry wrapper, service control, web-home
  tools, and hardware tools.
- uIP webserver on port 80 with a customizable Water.css home page and static
  Water.css Markdown docs under `/docs/`.
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

The running board is the source of truth:

```sh
fruitclaw status
fruitclaw tools
help
```

## Tool Examples

Local CLI:

```sh
fruitclaw terminal-run uname -a
fruitclaw terminal-run ls /dev
fruitclaw neopixels blue
fruitclaw neopixels off
fruitclaw service start ftpd
fruitclaw service start telnetd
fruitclaw schedule list
fruitclaw berry-run hello.be
```

MCP tools include `system.status`, `system.info`, `time.now`, `terminal.run`,
`device.list`, `device.read`, `device.write`, `neopixels.set`,
`neopixels.effect`, `scheduler.add`, `scheduler.list`, `scheduler.remove`,
`telegram.send_message`, `berry.run_script`, `script.write`, `script.run`,
`web.home.read`, and `web.home.write`.

Telegram is text-only in this release. Inbound Telegram messages are accepted
only from numeric chat IDs listed in `telegram_allowed_chats.txt`.

## Display Profile

FruitClaw uses the safe color DVI profile, not the TRMNL 800x480 grayscale
profile:

```text
CONFIG_RP23XX_HSTX_DVI_FB_RGB565_320X240=y
# CONFIG_RP23XX_HSTX_DVI_FB_Y2_800X480 is not set
CONFIG_RP23XX_HSTX_DVI_PIXEL_CLOCK=25200000
CONFIG_RP23XX_HSTX_DVI_SCANOUT_PSRAM=y
```

The 25.2 MHz DVI pixel clock and PSRAM scanout setting come from the
TRMNL/HSTX bring-up notes, but this operator build stays on the reliable
320x240 RGB565 path for LVGL, CGOL, and general board work.

TRMNL-specific timing notes are in:

```text
nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/TRMNL_DVI_TIMING.md
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

## Clone The Source

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

## Build The Production UF2

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

The helper below is for a warm Docker build tree. It avoids rebuilding the
whole world when only FruitClaw-relevant paths changed:

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

## Documentation Locations

More detail lives in the source tree and on the board:

- Browser home page served at `/`:
  `apps/examples/webserver/httpd-fs/index.html`
- Browser manual served at `/docs/`:
  `apps/examples/webserver/httpd-fs/docs/`
- Legacy `/doc/` compatibility redirect:
  `apps/examples/webserver/httpd-fs/doc/index.html`
- FruitClaw app manual:
  `apps/system/fruitclaw/README.md`
- Fruit Jam board notes:
  `nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/README.md`
- PSRAM notes:
  `nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/PSRAM.md`
- TRMNL/HSTX timing notes:
  `nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/TRMNL_DVI_TIMING.md`
- ESP-Hosted board notes:
  `nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/ESP_HOSTED.md`

## Known Release Boundaries

- Telegram and DeepSeek need runtime credentials before end-to-end chat works.
- TLS is allowed unverified for bring-up unless CA roots are installed.
- SD must be mounted for persistent `/mnt/sd0/fruitclaw`; otherwise the board
  falls back to `/data/fruitclaw`.
- MCP is intentionally dangerous YOLO mode in this release.
- Local graphical UI polish is still future work; the current web UI is static
  docs plus the editable root page, and the board display profile is the safe
  320x240 RGB565 DVI mode.
