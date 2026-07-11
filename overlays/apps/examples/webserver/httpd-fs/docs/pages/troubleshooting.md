# Troubleshooting

Start with local proof from the board. The most useful first commands are:

```sh
fruitclaw status
fruitclaw tools
fruitclaw mcp status
ifconfig wlan0
ps
```

## Port 80 Manual Does Not Load

Check that the board is reachable:

```sh
ping DEVICE_IP
curl -i http://DEVICE_IP/
```

If the TCP port is open but the page is old, remember that the manual is
compiled into:

```text
apps/examples/webserver/httpd_fsdata.c
```

After editing files under `apps/examples/webserver/httpd-fs`, regenerate the
web filesystem, rebuild, and flash. Editing Markdown alone does not update the
running firmware.

## MCP Not Reachable

MCP uses the same port 80 server:

```sh
curl -i http://DEVICE_IP/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"ping"}'
```

Expected:

```json
{"jsonrpc":"2.0","id":1,"result":{}}
```

`GET /mcp` returning `405` is expected. MCP uses POST. `OPTIONS /mcp` should
report that POST and OPTIONS are allowed.

If `tools/list` is large, make sure your client reads the full HTTP response
before parsing JSON.

## Telegram And MCP Contention

FruitClaw uses one serialized native webclient lane for Telegram, DeepSeek,
MCP `http.request`, and outbound Telegram replies. The current profile keeps
Telegram polling short and bounded so MCP can respond.

Check:

```sh
fruitclaw status
```

For MCP, prefer `system.status` over `terminal.run fruitclaw status`.

## Serial Or Telnet Shell Feels Stuck

Try another surface first:

```sh
telnet DEVICE_IP
curl -i http://DEVICE_IP/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"ping"}'
```

Use `ps` to see whether a foreground command is occupying a shell. Long-running
apps such as `cgol` stay in the foreground until Ctrl-C.

## USB Console Echo

The current profile keeps NSH echoback enabled, so a default serial program
such as `tio /dev/cu.usbmodem01` should show what you type and Enter should
advance cleanly to the next prompt. If an older alpha image shows repeated
`nsh>` prompts on one line or hides typed characters, rebuild with
`CONFIG_NSH_DISABLE_ECHOBACK` unset and `CONFIG_READLINE_FORCE_ECHO=y`.

FruitClaw's prompt-based setup commands use their own CR/LF-aware reader. If
`fruitclaw config set-wifi` prints `ssid:` or `password:` but Enter does not
submit the value, the image is older than the prompt-reader fix.

## Wi-Fi

The current guarded alpha image uses an explicit SD-backed Wi-Fi config path
first, then falls back through the active data root:

```text
/mnt/sd0/fruitclaw_wifi.conf
/mnt/sd0/fruitclaw/wifi.conf
/data/fruitclaw/wifi.conf
```

Enter it from NSH:

```sh
fruitclaw config set-wifi
```

Supported keys:

```text
ssid=
password= or psk=
key_mgmt=
cipher=
```

NuttX should expose the network as `wlan0`.

## Secrets And Allowlist

Expected runtime files:

```text
/mnt/sd0/fruitclaw/secrets/telegram_token
/mnt/sd0/fruitclaw/secrets/deepseek_api_key
/mnt/sd0/fruitclaw/telegram_allowed_chats.txt
```

FruitClaw status reports whether these files are present without printing
secret values.

Use the prompt-based writers instead of putting secrets in source or command
history:

```sh
fruitclaw config set-secret telegram
fruitclaw config set-secret deepseek
```

## Terminal Commands Time Out Through MCP

Use specific tools where possible:

| Instead of | Use |
| --- | --- |
| `terminal.run` with `ls /dev` | `device.list` |
| `terminal.run` with `fruitclaw status` | `system.status` |
| `terminal.run` with DVI diagnostics | local `dvictrl info` over serial or Telnet |

`terminal.run` is still useful for bounded commands such as `uname -a` and
`help`.

## Device Tool Reports Block Device Denied

This is expected for raw storage paths such as:

```text
/dev/mmcsd0
/dev/ram0
/dev/mtd0
```

Generic `device.read` and `device.write` are intentionally limited to
character-style device nodes. A raw block-device read can stall the board on an
unhealthy SD/media path, so FruitClaw denies those paths before opening them.
Use `device.list` for discovery and wait for the dedicated guarded storage
tooling before doing SD/media recovery through MCP.

## DVI Or Game Of Life

Start the DVI output before framebuffer demos:

```sh
dvictrl start
dvictrl info
cgol
```

`cgol` uses `/dev/fb0` and runs in the foreground. Press Ctrl-C to return to
NSH.

## Scheduler

Schedules are stored in:

```text
/mnt/sd0/fruitclaw/schedules.json
```

Check:

```sh
fruitclaw schedule list
```

Plain prompts are delivered directly at fire time. Prefix with `agent:` only
when the schedule should invoke the LLM/tool loop.

## Watchdog Recovery

The guarded alpha image should recover into ROM BOOTSEL if a guarded operation
wedges. It also intentionally forces ROM BOOTSEL after a fixed uptime so the
board is flashable during unattended testing.

Useful facts:

```text
CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS=600000
CONFIG_FRUITCLAW_GUARD_BOOTSEL_RECOVERY=y
```

`fruitclaw guard-test` intentionally trips a watchdog path. Run it only for a
planned reset/recovery test; the expected target in this alpha is ROM BOOTSEL.

## Known Boundaries

- Telegram is text-only in this slice.
- DeepSeek calls are non-streaming.
- GPIO capability entries are stubs unless real GPIO devices are available.
- LVGL and Berry LVGL are compiled, but a polished local graphical UI is not
  the current FruitClaw milestone.
- TLS should use a CA bundle for normal operation; unverified TLS is for
  bring-up.

Sources: `apps/system/fruitclaw/README.md`,
`apps/system/fruitclaw/Kconfig`,
`apps/system/fruitclaw/caps/fc_caps_builtin.c`,
`nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/ESP_HOSTED.md`.
