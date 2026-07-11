# FruitClaw Manual

FruitClaw is the onboard operator agent for this Adafruit Fruit Jam RP2350
NuttX build. If you are new to the system, think of it as three layers:

| Layer | What it does |
| --- | --- |
| Fruit Jam hardware | The RP2350 board, SD card, DVI output, NeoPixels, USB host port, and ESP32-C6 Wi-Fi coprocessor. |
| NuttX | The small real-time operating system running on the RP2350. It provides the shell, filesystems, networking, `/dev`, framebuffer, watchdog, and built-in apps. |
| FruitClaw | A native C application that connects Telegram, MCP, DeepSeek, schedules, Berry scripts, terminal commands, device access, NeoPixels, and persistent memory. |

The port 80 site you are reading is a static manual served by the embedded
uIP webserver. The board serves `index.html`, Markdown files, and JSON. Your
browser renders the Markdown. The microcontroller does not run React, a docs
generator, or server-side Markdown.

## What The Current Build Can Do

The current `adafruit-fruit-jam-rp2350:esp-hosted` profile is an owner-mode
guarded alpha image. It currently includes:

- FruitClaw agent app with boot autostart.
- Static web manual on port 80.
- MCP JSON-RPC endpoint at `POST /mcp`.
- Telegram text polling and Telegram replies.
- DeepSeek OpenAI-compatible chat/completions calls.
- Tool calling through one native capability registry.
- Telnet server on port 23 and FTP server on port 21.
- Data root that prefers `/mnt/sd0/fruitclaw` when SD is mounted and writable,
  with `/data/fruitclaw` fallback so SD cannot block serial/MCP recovery.
- Berry interpreter and constrained FruitClaw Berry runner.
- LVGL and Berry LVGL bindings.
- DVI framebuffer at `/dev/fb0`.
- Conway's Game of Life as the `cgol` built-in app.
- NeoPixel control through `/dev/leds0`.
- USB keyboard, USB mouse, and Xbox controller support.
- Watchdog and bootguard-style recovery tools for unattended operator use.
- A temporary 10 minute max-uptime BOOTSEL recovery guard for bring-up and
  flashing safety.

This is still active operator software, not a locked-down consumer appliance.
The direction is a practical board agent that a device owner can operate from
Telegram, MCP, serial, Telnet, or local NuttX shell commands.

## Main Ways To Use It

| Surface | How to reach it | Use it for |
| --- | --- | --- |
| Web manual | `http://DEVICE_IP/` | Read docs and onboarding from the board itself. |
| NSH shell | USB CDC serial or Telnet port 23 | Run NuttX and FruitClaw commands directly. |
| MCP | `POST http://DEVICE_IP/mcp` | Let an MCP client discover and call FruitClaw tools. |
| Telegram | Allowed chat with the configured bot | Talk to the agent and let it call tools. |
| FTP | Port 21 | Inspect or move files when the service is running. |

For the board used during bring-up, the IP has usually been `192.168.1.7`.
Check the live address with `ifconfig wlan0` from the NuttX shell.

## Important Runtime Files

The current profile enables the RP23xx SPI-SD automount path and then prefers
SD when `/mnt/sd0` is mounted and writable:

```text
/mnt/sd0/fruitclaw
/mnt/sd0/fruitclaw/.fruitclaw-ready
```

If SD is absent or unhealthy, FruitClaw falls back to volatile tmpfs:

```text
/data/fruitclaw
```

This keeps serial, Telnet, MCP, and watchdog recovery responsive even when SD
is absent or unhealthy. FruitClaw creates the SD data directory and ready
marker only when the mounted SD path is writable; it does not run raw
block-device mounts from owner tools or MCP.

Important files under the data root:

```text
system.md
user.md
memory.jsonl
router_rules.json
schedules.json
telegram_offset
telegram_allowed_chats.txt
wifi.conf
http_allowlist.txt
certs/roots.pem
secrets/telegram_token
secrets/deepseek_api_key
sessions/<safe-session-id>.jsonl
scripts/*.be
notes/
tmp/
```

This alpha profile also pins the main Wi-Fi/secrets files to SD-backed paths:

```text
/mnt/sd0/fruitclaw_wifi.conf
/mnt/sd0/fruitclaw/secrets/telegram_token
/mnt/sd0/fruitclaw/secrets/deepseek_api_key
/mnt/sd0/fruitclaw/telegram_allowed_chats.txt
/mnt/sd0/fruitclaw/certs/roots.pem
```

Secrets are runtime files only. The docs can say whether they are expected,
but they must not contain token values.

## Start Here

1. Open `/#/quickstart`.
2. Run `fruitclaw status` from USB serial or Telnet.
3. Run `fruitclaw tools` to see the capability registry.
4. Try `curl` or an MCP client against `POST /mcp`.
5. Try a safe local command such as `fruitclaw terminal-run uname -a`.
6. Try hardware with `fruitclaw neopixels blue`, then `fruitclaw neopixels off`.

Sources: `apps/system/fruitclaw/README.md`, `apps/system/fruitclaw/Kconfig`,
`apps/system/fruitclaw/fruitclaw_main.c`,
`apps/system/fruitclaw/caps/fc_caps_builtin.c`,
`nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/configs/esp-hosted/defconfig`.
