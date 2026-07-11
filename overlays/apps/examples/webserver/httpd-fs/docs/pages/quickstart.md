# Quick Start

This page assumes you are new to Fruit Jam, NuttX, and FruitClaw.

## 1. Know What Is Running

The RP2350 runs NuttX, not Linux. The shell prompt is NuttX NSH. Commands such
as `help`, `ps`, `ifconfig`, `mount`, `ls`, `cat`, and `uname` are NuttX shell
commands or built-in apps.

FruitClaw is one built-in app inside that NuttX image. Most day-to-day checks
start with:

```sh
fruitclaw status
fruitclaw config
fruitclaw tools
```

`fruitclaw status` prints non-secret runtime health: data root, Wi-Fi setup,
Telegram/DeepSeek secret presence, queues, scheduler, MCP, webserver, services,
watchdog state, and visible tools.

`fruitclaw tools` prints the registered FruitClaw capability tools.

## 2. Find The Board

From NSH:

```sh
ifconfig wlan0
```

From a computer on the same network, try:

```sh
ping DEVICE_IP
curl http://DEVICE_IP/
```

The web manual is served by the board on port 80.

## 3. Enter Local Runtime Config

FruitClaw stores runtime files under the active data root. The current profile
enables RP23xx SPI-SD automount, prefers `/mnt/sd0/fruitclaw` when SD is
mounted and writable, and falls back to `/data/fruitclaw` when SD is absent or
not writable. In this guarded alpha profile, the explicit Wi-Fi config path is
`/mnt/sd0/fruitclaw_wifi.conf` and the Telegram/DeepSeek/allowlist paths are
under `/mnt/sd0/fruitclaw/`. Enter Wi-Fi and API secrets from NSH:

```sh
fruitclaw config set-wifi
fruitclaw config set-secret telegram
fruitclaw config set-secret deepseek
```

The secret prompts do not print the entered value when the console supports
terminal echo control. The USB CDC console echoes ordinary NSH commands once
with default `tio`, and FruitClaw setup prompts accept carriage return.
`fruitclaw status` reports only whether each secret is present.

## 4. Check Core Services

The current profile is configured to start these services from `fruitclaw boot`:

| Service | Port | Check |
| --- | ---: | --- |
| Web manual and MCP route | 80 | `curl http://DEVICE_IP/` |
| Telnet shell | 23 | `telnet DEVICE_IP` |
| FTP daemon | 21 | FTP client to `DEVICE_IP` |
| Telegram polling | outbound HTTPS | `fruitclaw telegram-test` |
| DeepSeek | outbound HTTPS | `fruitclaw deepseek-test` |

For a quick MCP health check:

```sh
curl -i http://DEVICE_IP/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"ping"}'
```

Expected result:

```json
{"jsonrpc":"2.0","id":1,"result":{}}
```

## 5. Try Safe Commands First

From NSH:

```sh
fruitclaw terminal-run uname -a
fruitclaw device list
fruitclaw schedule list
fruitclaw mcp status
```

From MCP, prefer FruitClaw tools over terminal commands:

```json
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"system.status","arguments":{}}}
```

Use `device.list` instead of `terminal.run` for `/dev` enumeration, and use
`system.status` instead of `terminal.run` for FruitClaw health. Raw block
devices such as `/dev/mmcsd0` are denied by `device.read` and `device.write`;
they need dedicated guarded storage tooling.

## 6. Try Hardware

NeoPixels:

```sh
fruitclaw neopixels blue
fruitclaw neopixels rainbow blue 2
fruitclaw neopixels off
```

DVI framebuffer:

```sh
dvictrl start
dvictrl info
```

Conway's Game of Life:

```sh
dvictrl start
cgol
```

`cgol` runs in the foreground. Press Ctrl-C from the shell to stop it.

## 7. Try Telegram

Telegram is text-only in this build. Inbound messages are accepted only from
allowed chats. The allowlist file is:

```text
/mnt/sd0/fruitclaw/telegram_allowed_chats.txt
```

Useful checks:

```sh
fruitclaw telegram-discover
fruitclaw telegram-test
fruitclaw telegram-inject "what time is it? use the time tool"
```

When Telegram and MCP are both active, MCP tool calls can enqueue Telegram
notifications such as:

```text
FruitClaw MCP tool call: time.now -> ok
```

## 8. Understand Owner Mode

This build intentionally runs MCP in YOLO owner mode. That means:

- `POST /mcp` has no bearer-token layer in this profile.
- MCP tool calls run as the device owner.
- Tools such as `terminal.run`, `device.write`, `scheduler.add`, Berry, and
  NeoPixels can mutate board state.
- Keep this on a trusted local network.

Telegram still uses the allowed chat file for inbound filtering.

## 9. Recovery Expectations

This alpha is configured as an unattended bring-up image. It is intentionally
biased toward returning to ROM BOOTSEL so it can be reflashed without someone
pressing reset or holding BOOTSEL.
The current profile has:

```text
CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS=600000
CONFIG_FRUITCLAW_GUARD_BOOTSEL_RECOVERY=y
```

Risky operations are wrapped with watchdog guards, the session guard watches
runtime progress, and watchdog trips use the bootguard scratch path to enter
BOOTSEL. The max-uptime guard is a blunt temporary safety fuse: after about 10
minutes the board returns to BOOTSEL even if it appears healthy. Disable that
only for final production-style images after unattended testing is complete.

Sources: `apps/system/fruitclaw/README.md`,
`apps/system/fruitclaw/fruitclaw_main.c`,
`apps/system/fruitclaw/caps/fc_caps_builtin.c`,
`nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/configs/esp-hosted/defconfig`.
