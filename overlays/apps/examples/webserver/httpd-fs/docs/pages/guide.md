# Complete Guide

This is the complete FruitClaw guide map. The manual is intentionally split
across pages so the embedded board only serves the Markdown page you open.

If you are new to Fruit Jam and NuttX, read these in order:

1. [Getting Started](#/quickstart)
2. [Features](#/features)
3. [Tools](#/tools)
4. [Examples](#/examples)
5. [Hardware](#/hardware)
6. [Build](#/build)
7. [Troubleshooting](#/troubleshooting)

## How The Pages Fit Together

| Page | Read it when you want to understand |
| --- | --- |
| [Getting Started](#/quickstart) | First boot, first shell commands, first MCP call, first Telegram check, first hardware tests. |
| [Features](#/features) | What is compiled into this exact `esp-hosted` profile. |
| [Tools](#/tools) | FruitClaw CLI commands, MCP methods, and every visible capability tool. |
| [Examples](#/examples) | Copy-paste commands for status, MCP, Telegram, schedules, Berry, NeoPixels, DVI, and Game of Life. |
| [Hardware](#/hardware) | Fruit Jam architecture, ESP-Hosted Wi-Fi, SD storage, DVI, PSRAM, LVGL, USB host, and NeoPixels. |
| [Build](#/build) | Configure, build, regenerate the web filesystem, flash, and verify the image. |
| [Troubleshooting](#/troubleshooting) | What to check when web, MCP, Telegram, shell, Wi-Fi, watchdog, or hardware behavior is wrong. |

## System Summary

FruitClaw is a native NuttX application running on the RP2350. It is the board
agent layer above NuttX and the Fruit Jam hardware.

The current build exposes several operator surfaces:

- Static web manual on port 80.
- MCP JSON-RPC endpoint at `POST /mcp` on port 80.
- USB CDC serial shell.
- Telnet shell on port 23.
- FTP daemon on port 21.
- Telegram text chat through the configured bot.
- Local NSH commands such as `fruitclaw status`, `dvictrl`, and `cgol`.

The important first command is:

```sh
fruitclaw status
```

The important first MCP call is:

```json
{"jsonrpc":"2.0","id":1,"method":"ping"}
```

The important first hardware checks are:

```sh
fruitclaw neopixels blue
fruitclaw neopixels off
dvictrl start
dvictrl info
```

The normal image uses the LVGL-friendly `320x240 RGB565` framebuffer profile
with 1:1 output inside the 640x480 DVI timing. TRMNL-style `800x480 FB_FMT_Y2`
grayscale work is kept in separate TRMNL/HSTX profiles; it is not part of the
FruitClaw operator image.

## What Makes This Image Special

This is an owner-mode development image. MCP is intentionally YOLO in this
profile: no bearer token, no default policy layer, and owner-mode tools are
available to local MCP clients. Keep it on a trusted network.

This build is configured for unattended bring-up and alpha testing. Most
guarded wedges use the RP2350 watchdog plus Fruit Jam bootguard scratch path
to return to ROM BOOTSEL for reflashing. The temporary max-uptime fuse is
enabled with `FRUITCLAW_MAX_UPTIME_GUARD_MS=600000`, and
`FRUITCLAW_GUARD_BOOTSEL_RECOVERY=y` is on. That means a flashed alpha should
survive experimentation without you babysitting the reset button; it will come
back to BOOTSEL either after a guarded stall or after roughly 10 minutes.

Sources: `apps/system/fruitclaw/README.md`,
`apps/system/fruitclaw/Kconfig`,
`apps/system/fruitclaw/fruitclaw_main.c`,
`apps/system/fruitclaw/caps/fc_caps_builtin.c`,
`nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/configs/esp-hosted/defconfig`.
