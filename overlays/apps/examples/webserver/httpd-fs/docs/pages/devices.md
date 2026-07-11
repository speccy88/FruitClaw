# Kernel And Devices

This release runs on Apache NuttX, so board hardware is exposed through normal
NuttX device files, network interfaces, filesystems, and built-in apps. Use
`ls /dev`, `mount`, `ifconfig`, `dvictrl info`, and `fruitclaw status` to see
the live board state.

## Core Filesystems

| Path | Meaning |
| --- | --- |
| `/data` | tmpfs-backed fallback storage created by board bring-up. |
| `/mnt/sd0` | FAT mount point for the microSD card when available. |
| `/mnt/sd0/fruitclaw` | Preferred persistent FruitClaw data root when SD is mounted and writable. |
| `/data/fruitclaw` | Volatile fallback FruitClaw data root. |
| `/proc` | procfs runtime/kernel information when mounted. |

The Fruit Jam profile enables the RP23xx SPI-SD automount path. If SD is not
mounted and writable, FruitClaw uses `/data/fruitclaw` so serial, Telnet, MCP,
and recovery remain responsive.

## FruitClaw Data Root

Important runtime files under the active FruitClaw data root:

```text
wifi.conf
system.md
user.md
memory.jsonl
schedules.json
router_rules.json
telegram_offset
telegram_allowed_chats.txt
www/home.md
sessions/
scripts/
scripts/generated/
notes/
secrets/
services/
certs/roots.pem
```

`www/home.md` is the owner-editable Markdown rendered by the root web page.
Use MCP `web.home.write`, `web.home.read`, FTP, or local shell access to
maintain it.

## Expected Device Nodes

These are the important device paths used by this release:

| Device | Role |
| --- | --- |
| `/dev/console` | NSH console path. |
| `/dev/fb0` | 320x240 RGB565 framebuffer for DVI/LVGL/CGOL. |
| `/dev/leds0` | WS2812/NeoPixel LED device. |
| `/dev/watchdog0` | Hardware watchdog used by FruitClaw guards and `dvictrl` guard. |
| `/dev/mmcsd0` | microSD block device when card registration succeeds. |
| `/dev/kbd0` | USB HID keyboard input device. |
| `/dev/mouse0` | USB HID mouse input device and Berry/LVGL mouse path. |
| `/dev/xboxa` | Xbox controller ABI used by the Xbox support path. |
| `/dev/buttons` | Board button input lower-half. |
| `/dev/userleds` | User LED lower-half. |
| `/dev/audio/pcm0` | I2S/audio PCM path when initialized by the board support. |
| `/dev/i2c*` | I2C device(s) from the RP23xx I2C driver. |
| `/dev/spi*` | SPI device(s) from the RP23xx SPI driver. |

Not every node appears if the matching peripheral is absent or has not been
initialized yet. `device.list` and `ls /dev` are the safest live checks.

## Device Tool Safety

MCP and Telegram can use:

```text
device.list
device.read
device.write
```

Those tools are intentionally bounded. `device.read` and `device.write` reject
raw block/media paths such as:

```text
/dev/mmcsd*
/dev/ram*
/dev/mtd*
/dev/smart*
```

This prevents an owner-mode model call from wedging the board by opening raw SD
or flash devices. Use filesystem tools, FTP, or dedicated storage commands for
media work.

## Network Interface

`wlan0` is the NuttX network interface backed by the ESP32-C6 running
ESP-Hosted-MCU. In this release:

- ESP32-C6 handles Wi-Fi radio work.
- NuttX owns DHCP, DNS, sockets, Telnet, FTP, HTTP, MCP, NTP, and outbound TLS.
- FruitClaw Wi-Fi setup reads `wifi.conf` from the explicit Kconfig path when
  set, then the active data root, then `/mnt/sd0/fruitclaw`, then
  `/data/fruitclaw`.

Useful checks:

```sh
ifconfig wlan0
fruitclaw wifi-up
ping -c 3 1.1.1.1
```

## Display And Input Stack

The current FruitClaw display profile is:

```text
CONFIG_RP23XX_HSTX_DVI_FB_RGB565_320X240=y
CONFIG_RP23XX_HSTX_DVI_PIXEL_CLOCK=25200000
CONFIG_RP23XX_HSTX_DVI_SCANOUT_PSRAM=y
CONFIG_RP23XX_HSTX_DVI_SINGLE_SCANOUT_INTERNAL=y
```

Use `/dev/fb0` for framebuffer work and `/dev/mouse0` plus `/dev/kbd0` for
input. The current FruitClaw image uses a PSRAM application framebuffer and one
internal SRAM scanout buffer for a reliable monitor signal. TRMNL 800x480 Y2
firmware is a separate profile and is not enabled in this FruitClaw release.

## Watchdog Recovery

This guarded alpha release keeps watchdog guards and does use automatic
BOOTSEL conversion:

```text
CONFIG_FRUITCLAW_GUARD_BOOTSEL_RECOVERY=y
CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS=600000
```

If a guarded Berry, terminal, device, HTTP, LLM, or MCP owner operation hangs,
the watchdog resets the RP2350 into ROM BOOTSEL. A separate 10 minute
max-uptime guard also returns the board to BOOTSEL during bring-up so testing
does not require physical button babysitting.

Sources: `nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/configs/esp-hosted/defconfig`,
`nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/src/rp23xx_bringup.c`,
`apps/system/fruitclaw/caps/fc_caps_builtin.c`,
`nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/ESP_HOSTED.md`,
`nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/SDIO.md`.
