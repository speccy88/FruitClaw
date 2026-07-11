# Installed Features

This page describes what the current Fruit Jam RP2350 `esp-hosted` profile
selects in this checkout. It does not describe every possible NuttX feature.

## FruitClaw Core

`CONFIG_SYSTEM_FRUITCLAW=y` enables the `fruitclaw` application. FruitClaw
uses cJSON for JSON, webclient for outbound HTTP, and mbedTLS when TLS support
is enabled.

Current profile highlights:

```text
CONFIG_SYSTEM_FRUITCLAW=y
CONFIG_FRUITCLAW_BOOT_AUTOSTART=y
CONFIG_FRUITCLAW_BOOT_START_WEBSERVER=y
CONFIG_FRUITCLAW_BOOT_START_NTPC=y
CONFIG_FRUITCLAW_BOOT_START_TELNETD=y
CONFIG_FRUITCLAW_BOOT_START_FTPD=y
CONFIG_FRUITCLAW_WIFI_AUTOSTART=y
CONFIG_FRUITCLAW_TLS_ALLOW_UNVERIFIED=y
CONFIG_FRUITCLAW_MCP_SERVER=y
CONFIG_FRUITCLAW_MCP_YOLO_MODE=y
CONFIG_FRUITCLAW_MCP_MAX_RESPONSE=32768
CONFIG_FRUITCLAW_MCP_TOOL_TIMEOUT_MS=30000
CONFIG_FRUITCLAW_BERRY_EXPERIMENTAL_RUNNER=y
```

This guarded alpha profile keeps watchdog recovery enabled and deliberately
routes guarded resets to BOOTSEL:

```text
CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS=600000
CONFIG_FRUITCLAW_GUARD_BOOTSEL_RECOVERY=y
```

Recovery is handled by per-operation guards, session watchdog behavior, and
the temporary 10 minute max-uptime fuse. In this alpha, ROM BOOTSEL is the
automatic failure target so the board is flashable after bad tests.

## Networking

The board uses ESP-Hosted Wi-Fi, so NuttX sees a normal `wlan0` interface.

Compiled network services:

```text
CONFIG_NETUTILS_WEBSERVER=y
CONFIG_EXAMPLES_WEBSERVER=y
CONFIG_NETUTILS_HTTPD_CGIPATH=y
CONFIG_NETUTILS_HTTPD_POST=y
CONFIG_NETUTILS_HTTPD_MAX_BODY=16384
CONFIG_NETUTILS_HTTPD_TIMEOUT=3
CONFIG_NETUTILS_HTTPD_KEEPALIVE_DISABLE=y
CONFIG_NETUTILS_WEBCLIENT=y
CONFIG_NETUTILS_FTPD=y
CONFIG_NETUTILS_TELNETD=y
CONFIG_NETUTILS_MQTTC=y
CONFIG_NETUTILS_NTPCLIENT=y
CONFIG_NETDB_DNSCLIENT=y
CONFIG_SYSTEM_PING=y
```

Port 80 serves both the static manual and the MCP route.

FruitClaw also exposes service maintenance through CLI and MCP:

```text
service.status
service.control
fruitclaw service status [telnetd|ftpd]
fruitclaw service <start|stop|restart|enable|disable> <telnetd|ftpd>
```

FTP supports the full lifecycle through `ftpd_start` and `ftpd_stop`. Telnet
uses NuttX `telnetd` with a tmpfs PID file, so `telnetd -k` lets FruitClaw
stop and restart the NSH Telnet daemon too.
Disable state is stored under `services/*.disabled` in the FruitClaw data root.

## Storage

Compiled filesystems:

```text
CONFIG_FS_FAT=y
CONFIG_FS_PROCFS=y
CONFIG_FS_TMPFS=y
CONFIG_FSUTILS_MKFATFS=y
```

FruitClaw data-root selection in this profile:

```text
/mnt/sd0/fruitclaw
/mnt/sd0/fruitclaw/.fruitclaw-ready
/data/fruitclaw
```

`CONFIG_RP23XX_SPISD_AUTOMOUNT=y` and
`CONFIG_FRUITCLAW_PREFER_SD_DATA_DIR=y` are enabled. If `/mnt/sd0` is mounted
and writable, FruitClaw creates/uses `/mnt/sd0/fruitclaw` and its ready marker.
If SD is missing or not writable, it falls back to `/data/fruitclaw` so serial,
Telnet, MCP, and watchdog diagnostics still come up.

## Shell And Local Apps

Useful shell and built-in app features:

```text
CONFIG_NSH_BUILTIN_APPS=y
CONFIG_NSH_READLINE=y
CONFIG_NSH_USBCONSOLE=y
CONFIG_READLINE_FORCE_ECHO=y
CONFIG_READLINE_CMD_HISTORY=y
CONFIG_READLINE_TABCOMPLETION=y
CONFIG_SYSTEM_VI=y
CONFIG_SYSTEM_DVICTRL=y
CONFIG_SYSTEM_NEOPIXELS=y
CONFIG_SYSTEM_NXPLAYER=y
CONFIG_SYSTEM_I2CTOOL=y
CONFIG_SYSTEM_SPITOOL=y
```

NuttX `help` shows the live command and app list.

The CDC driver itself starts with driver echo off; readline provides the single
visible command echo. FruitClaw's interactive prompts accept CR and LF so
default `tio /dev/cu.usbmodem01` input works for setup commands.

## Display And Graphics

Compiled display and graphics features:

```text
CONFIG_VIDEO_FB=y
CONFIG_RP23XX_HSTX_DVI=y
CONFIG_RP23XX_HSTX_DVI_FB_RGB565_320X240=y
CONFIG_RP23XX_HSTX_DVI_OUTPUT_SCALING=1
CONFIG_RP23XX_HSTX_DVI_PIXEL_CLOCK=25200000
CONFIG_RP23XX_HSTX_DVI_SCANOUT_PSRAM=y
CONFIG_RP23XX_HSTX_DVI_SINGLE_SCANOUT_INTERNAL=y
CONFIG_GRAPHICS_LVGL=y
```

The DVI control app is `dvictrl`. The framebuffer is `/dev/fb0`. This
profile uses the safe LVGL-friendly 320x240 RGB565 framebuffer with 1:1 output
inside the 640x480 DVI timing. The framebuffer memory is in PSRAM, while the
live HSTX scanout uses one internal SRAM buffer; PSRAM-only scanout registered
cleanly but did not produce a signal on the validated monitor. TRMNL-style
800x480 2-bit grayscale work lives in separate TRMNL/HSTX profiles and is not
part of the FruitClaw operator image.

## Game Of Life

The current image includes NuttX Conway's Game of Life:

```text
CONFIG_GAMES_CGOL=y
CONFIG_GAMES_CGOL_MAPWIDTH=320
CONFIG_GAMES_CGOL_MAPHEIGHT=240
CONFIG_GAMES_CGOL_FRAMEDELAY=50000
```

Run with:

```sh
dvictrl start
cgol
```

## Berry And LVGL Binding

Compiled interpreter features:

```text
CONFIG_INTERPRETERS_BERRY=y
CONFIG_INTERPRETERS_BERRY_STACKSIZE=32768
CONFIG_INTERPRETERS_BERRY_LVGL=y
CONFIG_INTERPRETERS_BERRY_LVGL_GLOBAL=y
CONFIG_INTERPRETERS_BERRY_LVGL_MOUSE=y
CONFIG_INTERPRETERS_BERRY_LVGL_MOUSE_DEVPATH="/dev/mouse0"
```

FruitClaw's Berry runner is constrained to scripts under the data-root
`scripts/` directory and exposes the constrained `claw` module. The direct
FruitClaw runner is enabled in this profile, so `berry.run_script`,
`fruitclaw berry-run`, generated Berry scripts, and scheduled generated Berry
scripts all use the same guarded VM path.

## USB Host And Input

Compiled USB host features:

```text
CONFIG_USBHOST=y
CONFIG_USBHOST_HUB=y
CONFIG_USBHOST_HID=y
CONFIG_USBHOST_HIDKBD=y
CONFIG_USBHOST_HIDMOUSE=y
CONFIG_USBHOST_XBOXCONTROLLER=y
CONFIG_EXAMPLES_XBC_TEST=y
CONFIG_EXAMPLES_TOUCHSCREEN_DEVPATH="/dev/mouse0"
```

## NeoPixels

Compiled LED features:

```text
CONFIG_RP23XX_BOARD_HAS_WS2812=y
CONFIG_WS2812=y
CONFIG_SYSTEM_NEOPIXELS=y
CONFIG_EXAMPLES_WS2812=y
```

FruitClaw targets `/dev/leds0` with `neopixels.*` tools.

## Watchdog And Recovery

Compiled recovery features:

```text
CONFIG_WATCHDOG=y
CONFIG_SYSTEM_BOOTSEL=y
CONFIG_SYSTEM_BOOTGUARD=y
CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD=y
CONFIG_FRUITCLAW_ENABLE_SESSION_GUARD=y
CONFIG_FRUITCLAW_GUARD_BOOTSEL_RECOVERY=y
CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS=600000
```

This release is meant to be usable without babysitting the reset button. During
alpha bring-up, guarded failures and the 10 minute max-uptime fuse reset the
RP2350 into ROM BOOTSEL so the next flash can happen immediately.

Sources: `nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/configs/esp-hosted/defconfig`,
`apps/system/fruitclaw/Kconfig`,
`apps/system/fruitclaw/README.md`,
`apps/system/fruitclaw/fruitclaw_main.c`.
