# Tools

FruitClaw tools are the actions the board can perform. There are two ways to
reach them:

- Local NSH command: `fruitclaw ...`
- MCP or Telegram agent tool calls through the shared capability registry

For a first-time user, start with:

```sh
fruitclaw status
fruitclaw tools
fruitclaw mcp status
```

## FruitClaw CLI

These commands are compiled by `apps/system/fruitclaw/fruitclaw_main.c`:

```sh
fruitclaw boot
fruitclaw start
fruitclaw status
fruitclaw status-net
fruitclaw reboot
fruitclaw wifi-up
fruitclaw service status [telnetd|ftpd]
fruitclaw service <start|stop|restart|enable|disable> <telnetd|ftpd>
fruitclaw once <message>
fruitclaw selftest
fruitclaw tools
fruitclaw config
fruitclaw config set-wifi
fruitclaw config set-secret <telegram|deepseek>
fruitclaw telegram-discover
fruitclaw telegram-test
fruitclaw telegram-inject <message>
fruitclaw deepseek-test
fruitclaw schedule list
fruitclaw schedule add-interval <id> <seconds> <prompt>
fruitclaw schedule add-once <id> <epoch-seconds> <prompt>
fruitclaw schedule add-after <id> <seconds> <prompt>
fruitclaw schedule add-cron <id> <expr> <prompt>
fruitclaw schedule add-boot <id> <prompt>
fruitclaw schedule remove <id>
fruitclaw berry-run <path> [json-args]
fruitclaw berry-smoke
fruitclaw guard-test
fruitclaw terminal-run <cmd...>
fruitclaw device list
fruitclaw device read <path> [max-bytes]
fruitclaw device write-text <path> <text...>
fruitclaw device write-hex <path> <hex...>
fruitclaw neopixels <off|color|effect...>
fruitclaw mcp status
```

`fruitclaw status` avoids slow live network calls. Use `fruitclaw status-net`
when you specifically want live `wlan0` netlib status.

`fruitclaw guard-test` intentionally trips the watchdog guard. Do not run it
unless you are testing reset/recovery behavior.

## Service Control

FruitClaw starts the configured maintenance services during boot:

- Telnet NSH shell on port 23.
- FTP daemon on port 21.

The service controller is shared by CLI, MCP, and the Telegram agent:

```sh
fruitclaw service status
fruitclaw service stop ftpd
fruitclaw service start ftpd
fruitclaw service restart ftpd
fruitclaw service restart telnetd
fruitclaw service disable telnetd
fruitclaw service enable telnetd
```

Disable creates a persistent marker under the FruitClaw data root, for example
`services/ftpd.disabled`. It prevents the service from autostarting on future
FruitClaw boots without changing the firmware image.

FTP has real `ftpd_start` and `ftpd_stop` commands in this build, so it
supports start, stop, restart, enable, and disable. Telnet exposes a normal
NSH service through `telnetd` and records the daemon PID in tmpfs, so
FruitClaw can stop it with `telnetd -k` and restart it through the same
service controller.

## MCP Endpoint

MCP runs over the same port 80 webserver as this manual:

```text
POST http://DEVICE_IP/mcp
```

Supported JSON-RPC methods:

| Method | Purpose |
| --- | --- |
| `initialize` | MCP client handshake. |
| `notifications/initialized` | Accepted as an MCP notification. |
| `ping` | Health check. |
| `tools/list` | Return visible FruitClaw tools and JSON schemas. |
| `tools/call` | Execute one named FruitClaw tool. |

`GET /mcp` returns `405`. `OPTIONS /mcp` reports that POST and OPTIONS are
allowed. There is no WebSocket or SSE server in this build.

This profile is YOLO owner mode: no bearer token and no default policy layer.
MCP tools run as owner and can mutate board state.

## Visible Capability Tools

The current MCP `tools/list` surface contains 31 visible tools:

| Tool | Use |
| --- | --- |
| `time.now` | Return current board time. |
| `system.info` | Return non-secret board and compiled feature summary. |
| `system.status` | Return non-secret runtime counters without running a shell command. |
| `service.status` | Return Telnet/FTP service state, autostart flags, counters, and limitations. |
| `service.control` | Start, stop, restart, enable, or disable Telnet/FTP services where supported. |
| `memory.append` | Append a short note to `memory.jsonl`. |
| `memory.read` | Read a bounded memory tail. |
| `file.read` | Read bounded files under the FruitClaw data root. |
| `file.write_limited` | Write files under `scripts/` or `notes/`. |
| `web.home.read` | Read the Markdown rendered by the root web page. |
| `web.home.write` | Replace the Markdown rendered by the root web page and served from `/site/home.md`. |
| `script.list` | List generated scripts under `scripts/generated/` and uploaded root-level scripts under `scripts/`, including source, validation status, and schedule references. |
| `script.read` | Read a generated Berry or NSH script for inspection. |
| `script.write` | Create or replace a generated Berry or NSH script, with optional run or Berry syntax validation. |
| `script.validate` | Validate a generated script by guarded run, or use `mode:"syntax"` for Berry parse-only validation. |
| `script.remove` | Remove a generated Berry or NSH script from `scripts/generated/`. |
| `script.run` | Run a generated Berry or NSH script directly. |
| `script.schedule` | Schedule a generated script at boot, once, on an interval, or by cron. |
| `telegram.send_message` | Send plain text to an allowed Telegram chat. |
| `scheduler.add` | Add boot, interval, once, after, or cron schedules. |
| `scheduler.list` | List configured schedules. |
| `scheduler.remove` | Remove a schedule by ID. |
| `berry.run_script` | Run a Berry script under the data-root `scripts/` directory. |
| `terminal.run` | Run a bounded NuttX/NSH command and return captured output. |
| `rtttl.play` | Play a built-in or bounded RTTTL tune through the NuttX RTTTL app. |
| `neopixels.set` | Set `/dev/leds0` to a color or effect. |
| `neopixels.off` | Turn `/dev/leds0` off. |
| `neopixels.effect` | Run `rainbow`, `chase`, or `pulse` on `/dev/leds0`. |
| `device.list` | List registered `/dev` nodes. |
| `device.read` | Read bounded bytes from a non-block `/dev` path. |
| `device.write` | Write bounded text or hex bytes to a non-block `/dev` path. |
| `http.request` | Run an allowlisted HTTP GET or POST request. |

Hidden/non-advertised entries include GPIO stubs and a legacy shell alias when
compiled. They are not the normal operator interface.

## Tool Usage Notes

Prefer specific tools over shell commands:

| Need | Prefer |
| --- | --- |
| Runtime health | `system.status` |
| Feature summary | `system.info` |
| Telnet/FTP state | `service.status` |
| FTP start/stop/restart | `service.control` |
| List devices | `device.list` |
| Read a device | `device.read` |
| Set LEDs | `neopixels.set` or `neopixels.effect` |
| Schedule work | `scheduler.add` |
| Run Berry | `berry.run_script` or generated `script.run` with `kind=berry` |
| Run generated NSH | `script.run` with `kind=shell` |
| Remove generated script | `script.remove` |

`terminal.run` has fast in-process handling for simple commands such as
`help` and `uname -a`. It deliberately points `/dev` enumeration and DVI
diagnostics toward safer dedicated paths so a shell command cannot monopolize
the HTTP worker.

`device.read` and `device.write` are for bounded character-style devices.
They reject block devices such as `/dev/mmcsd0`, `/dev/ram*`, and `/dev/mtd*`
before opening them. SD card recovery should use a dedicated storage command,
not raw MCP or Telegram reads from the block device.

When MCP tool notifications are enabled, the board can also send Telegram
messages like:

```text
FruitClaw MCP tool call: system.info -> ok
```

Sources: `apps/system/fruitclaw/fruitclaw_main.c`,
`apps/system/fruitclaw/caps/fc_caps_builtin.c`,
`apps/system/fruitclaw/net/fc_mcp.c`,
`apps/system/fruitclaw/README.md`.
