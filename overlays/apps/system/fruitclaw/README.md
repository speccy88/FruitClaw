# FruitClaw

FruitClaw is a small NuttX-native embedded agent app for the Adafruit Fruit Jam
RP2350.  It is an embedded client, not a local LLM: Telegram messages are
routed through a bounded NuttX event queue, sent to DeepSeek through the
OpenAI-compatible chat/completions API, optionally resolved through registered
tools, and persisted to local JSONL files.

FruitClaw is still in active development.  The target direction is a practical
owner-operated board agent that can be reached from Telegram and, now, from a
local MCP client.  The unfinished work is hardware validation and polish, not a
change in direction: Telegram, DeepSeek tool calling, Berry scripts, schedules,
terminal capture, `/dev` access, NeoPixels, SD persistence, and MCP all share
the same native C capability registry.

This app is intentionally not a Go, WASM, Linux, ESP-IDF, Node.js, Python, or
Lua runtime.  The core is plain C and uses NuttX/POSIX-style APIs.

## Build

Enable `CONFIG_SYSTEM_FRUITCLAW` in a networked Fruit Jam configuration such as
`adafruit-fruit-jam-rp2350:esp-hosted`.  The app selects cJSON and webclient;
HTTPS uses mbedTLS when `CONFIG_FRUITCLAW_ENABLE_TLS=y`.

```sh
export PATH="$HOME/.local/nuttx-tools/kconfig-frontends/bin:$PATH"
cd nuttx
./tools/configure.sh -E -m -a ../apps adafruit-fruit-jam-rp2350:esp-hosted
kconfig-tweak --file .config --enable SYSTEM_FRUITCLAW
kconfig-tweak --file .config --enable NETUTILS_HTTPD_CGIPATH
kconfig-tweak --file .config --enable NETUTILS_HTTPD_POST
kconfig-tweak --file .config --set-val NETUTILS_HTTPD_MAX_BODY 16384
kconfig-tweak --file .config --set-val NETUTILS_HTTPD_TIMEOUT 3
kconfig-tweak --file .config --enable FRUITCLAW_ENABLE_SESSION_GUARD
kconfig-tweak --file .config --enable FRUITCLAW_BOOT_AUTOSTART
kconfig-tweak --file .config --enable FRUITCLAW_WIFI_AUTOSTART
kconfig-tweak --file .config --enable FRUITCLAW_MCP_SERVER
kconfig-tweak --file .config --enable FRUITCLAW_MCP_YOLO_MODE
kconfig-tweak --file .config --disable NETUTILS_HTTPD_SINGLECONNECT
kconfig-tweak --file .config --enable FRUITCLAW_GUARD_BOOTSEL_RECOVERY
kconfig-tweak --file .config --set-val FRUITCLAW_GUARD_ACQUIRE_TIMEOUT_MS 5000
kconfig-tweak --file .config --set-val FRUITCLAW_TELEGRAM_POLL_TIMEOUT_SEC 0
kconfig-tweak --file .config --set-val FRUITCLAW_TELEGRAM_POLL_IDLE_MS 5000
kconfig-tweak --file .config --set-val FRUITCLAW_TELEGRAM_START_DELAY_MS 30000
kconfig-tweak --file .config --set-val FRUITCLAW_TELEGRAM_MCP_YIELD_MS 60000
kconfig-tweak --file .config --set-val FRUITCLAW_TELEGRAM_MCP_MAX_YIELD_MS 5000
kconfig-tweak --file .config --set-val FRUITCLAW_CLI_GUARD_TIMEOUT_MS 60000
kconfig-tweak --file .config --set-val FRUITCLAW_DEEPSEEK_HTTP_TIMEOUT_SEC 15
kconfig-tweak --file .config --set-val FRUITCLAW_LLM_GUARD_TIMEOUT_MS 60000
kconfig-tweak --file .config --set-val FRUITCLAW_AGENT_GUARD_TIMEOUT_MS 0
kconfig-tweak --file .config --set-val FRUITCLAW_TELEGRAM_HTTP_TIMEOUT_SEC 8
kconfig-tweak --file .config --set-val FRUITCLAW_TELEGRAM_HTTP_GUARD_TIMEOUT_MS 0
kconfig-tweak --file .config --set-val FRUITCLAW_SESSION_GUARD_TIMEOUT_MS 180000
kconfig-tweak --file .config --set-val FRUITCLAW_MAX_UPTIME_GUARD_MS 600000
make olddefconfig
make -j8
```

The current community-oriented `esp-hosted` profile is intended to bring the
useful board features into one build: FruitClaw, MCP, Telegram, DeepSeek,
telnet, FTP, uIP webserver/docs, LVGL, Berry, Berry LVGL bindings, PSRAM user
heap, PIO USB host, USB HID keyboard/mouse, Xbox controller support, SD/FAT,
NeoPixels at `/dev/leds0`, and the Fruit Jam DVI/framebuffer stack selected by
the board profile.  The operator image deliberately uses the safe 320x240
RGB565 framebuffer, centered 1:1 in a 640x480 DVI signal, with the same stable
`CONFIG_RP23XX_HSTX_DVI_PIXEL_CLOCK=25200000` discipline learned during the
TRMNL bring-up.  It does
not include the separate TRMNL display client or switch `/dev/fb0` to the
800x480 Y2 TRMNL framebuffer.  Use `fruitclaw config`, `fruitclaw status`, and
`fruitclaw tools` on the board to see what the running image actually compiled.

The current Fruit Jam `esp-hosted` profile enables
`CONFIG_RP23XX_SPISD_AUTOMOUNT=y` and
`CONFIG_FRUITCLAW_PREFER_SD_DATA_DIR=y`.  Board bring-up registers the SPI SD
block device and attempts the NuttX RP23xx automount path for `/mnt/sd0`.
FruitClaw then uses the SD data root only when that path is mounted and
writable:

```text
/mnt/sd0/fruitclaw
/mnt/sd0/fruitclaw/.fruitclaw-ready
```

If SD is missing or not writable, FruitClaw falls back to volatile tmpfs:

```text
/data/fruitclaw
```

FruitClaw itself only chooses between visible roots.  It does not run ad-hoc
raw block-device mounts from owner tools or MCP.  The RP23xx board bring-up
owns `/dev/mmcsd0` registration and `/mnt/sd0` mounting; if SD is absent,
unformatted, or not writable, FruitClaw falls back to `/data/fruitclaw` so the
operator shell and network services still come up.

Generic `device.read` and `device.write` calls intentionally reject block
devices such as `/dev/mmcsd0`, `/dev/ram*`, `/dev/mtd*`, and any path that
reports `S_ISBLK`.  Use them for bounded character-style device access only.
SD/media recovery needs a dedicated guarded storage command, not raw model or
MCP reads from the block device.

Wi-Fi credentials, Telegram/DeepSeek secrets, memory, schedules, and sessions
survive resets only when the active data root is persistent storage.

Install a CA bundle at:

```text
/mnt/sd0/fruitclaw/certs/roots.pem
```

For bring-up only, `CONFIG_FRUITCLAW_TLS_ALLOW_UNVERIFIED=y` lets HTTPS run
without certificate verification.  Leave it disabled for normal use.

## Runtime Files

FruitClaw creates this layout under the active data root if it is missing.  In
this `esp-hosted` image, the active root is `/mnt/sd0/fruitclaw` when SD is
mounted and writable, otherwise `/data/fruitclaw`.

```text
<active-fruitclaw-data-root>/
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
  services/telnetd.disabled
  services/ftpd.disabled
  notes/
```

Do not store credentials in source code, configs, README examples, or logs.
Put live secrets only in the `secrets/` files:

```sh
fruitclaw config set-secret telegram
fruitclaw config set-secret deepseek
```

The command prompts for the secret with terminal echo disabled when the console
supports it, then writes `secrets/telegram_token` or
`secrets/deepseek_api_key` under the active data root.  FruitClaw trims
whitespace and common serial-shell `\n` / `\r` suffixes when reading these
one-line secret files.

The current USB CDC console is configured for default `tio` use.  The terminal
stays canonical and echo-enabled for applications; NSH readline temporarily
disables driver echo while it owns command-line editing and restores the saved
mode before launching a command.  FruitClaw's own prompt reader accepts both CR
and LF, so interactive commands such as `fruitclaw config set-wifi` work when a
terminal sends carriage return.

For unattended provisioning over CDC serial, the same commands also accept a
bounded value argument and still avoid printing the secret back:

```sh
fruitclaw config set-secret telegram "<bot-token>"
fruitclaw config set-secret deepseek "<api-key>"
```

The current bring-up config seeds the known owner chat `6216681418` into
`telegram_allowed_chats.txt` on first data-root initialization.  To inspect or
replace the allowlist:

```sh
fruitclaw telegram-discover
cat /mnt/sd0/fruitclaw/telegram_allowed_chats.txt
echo "<numeric-chat-id>" > /mnt/sd0/fruitclaw/telegram_allowed_chats.txt
```

For unattended boot, store Wi-Fi settings in runtime storage, not in source.
The current guarded alpha profile reads the explicit
`FRUITCLAW_WIFI_CONFIG_PATH` first, then `wifi.conf` from the active data
root, then `/mnt/sd0/fruitclaw/wifi.conf`, then `/data/fruitclaw/wifi.conf`.
The checked-in `esp-hosted` path is:

```text
/mnt/sd0/fruitclaw_wifi.conf
```

```sh
fruitclaw config set-wifi
```

For automation, pass the SSID and password as arguments:

```sh
fruitclaw config set-wifi "<ssid>" "<password>"
```

`fruitclaw boot` reads this file, joins `CONFIG_FRUITCLAW_WIFI_IFNAME`
(`wlan0` on the ESP-Hosted profile), renews DHCP, starts NTP and HTTPD when
configured, then enters the normal FruitClaw runtime.

ESP-Hosted can expose `wlan0` before every WAPI command is ready.  FruitClaw
therefore retries the `ifup`/WAPI/DHCP sequence with
`CONFIG_FRUITCLAW_WIFI_COMMAND_RETRIES` and
`CONFIG_FRUITCLAW_WIFI_COMMAND_RETRY_DELAY_SEC`; failures are logged by stage
without printing the Wi-Fi password.

The current ESP-Hosted reliability fix and validation commands are recorded in
`docs/FRUITCLAW_WIFI_RELIABILITY.md`.  Keep that note nearby when touching
NuttX TCP write buffers, `NETDEV_RX_THREAD` wakeups, HTTP/MCP response sends,
or `fruitclaw wifi-probe`.

The default HTTP allowlist is:

```text
api.deepseek.com
api.telegram.org
```

## Commands

```sh
fruitclaw boot
fruitclaw wifi-up
fruitclaw selftest
fruitclaw config
fruitclaw status
fruitclaw tools
fruitclaw guard-test
fruitclaw deepseek-test
fruitclaw telegram-discover
fruitclaw telegram-test
fruitclaw telegram-inject "what time is it? use the time tool"
fruitclaw mcp status
fruitclaw service status
fruitclaw once "what time is it?"
fruitclaw start
```

Owner-mode board tools:

```sh
fruitclaw terminal-run uname -a
fruitclaw terminal-run ls /dev
fruitclaw neopixels blue
fruitclaw neopixels rainbow blue 2
fruitclaw neopixels off
fruitclaw service start ftpd
fruitclaw service stop ftpd
fruitclaw service restart ftpd
fruitclaw service restart telnetd
fruitclaw service disable telnetd
fruitclaw service enable telnetd
```

The NeoPixel tools also accept `duration_ms` for natural requests such as
`{"effect":"rainbow","duration_ms":3000}`; FruitClaw maps it to bounded
effect cycles internally.

Telnet and FTP service maintenance is exposed through the same controller used
by MCP `service.status` and `service.control`.  Disable writes a persistent
marker under `services/` in the active FruitClaw data root, preventing
autostart on later boots without changing the firmware image.  FTP supports
start, stop, restart, enable, and disable through the compiled `ftpd_start` and
`ftpd_stop` commands.  Telnet supports the same lifecycle through
`telnetd -k`, which stops the daemon recorded in the configured Telnet PID
file before FruitClaw starts it again.

Telnet NSH is intended to feel like serial NSH.  The Telnet driver requests
Suppress Go Ahead and keeps the client in remote-echo mode.  Readline mirrors
the terminal's saved `ECHO` setting while it owns interactive input.  Password
prompts can therefore suppress output without inviting the client to echo the
hidden value locally.

Schedules:

```sh
fruitclaw schedule list
fruitclaw schedule add-interval heartbeat 3600 "Run heartbeat"
fruitclaw schedule add-after reminder 60 "Send a reminder"
fruitclaw schedule add-once exact 4102444800 "Run at epoch time"
fruitclaw schedule add-cron morning "0 8 * * *" "Run morning workflow"
fruitclaw schedule add-boot ui "tool:script.run {\"path\":\"generated/ui.be\",\"kind\":\"berry\"}"
fruitclaw schedule remove heartbeat
```

The `scheduler.add` tool accepts the same schedule types, including `boot` for
runtime autostart jobs. For natural Telegram requests such as "remind me in 60
seconds", use
`{"type":"once","after_sec":60,"prompt":"..."}`; for exact one-shot times, use
`at_epoch`.  Plain schedule prompts are delivered directly at fire time so
simple reminders cannot wedge the LLM path.  Prefix the prompt with `agent:`
only when the scheduled job should wake DeepSeek and use tools autonomously.

Berry:

```sh
fruitclaw berry-smoke
fruitclaw berry-run morning.be '{}'
```

Berry scripts are resolved under `scripts/` in the active FruitClaw data root.
The embedded runner exposes a constrained `claw` module with `args()`,
`reply()`, `tool()`, `memory_append()`, `terminal_run()`,
`neopixels_set()`, `neopixels_off()`, `neopixels_effect()`,
`schedule_add()`, `script_run()`, `rtttl_play()`, `service_control()`, and
`telegram_send()`.

The direct Berry VM wrapper is enabled by
`CONFIG_FRUITCLAW_BERRY_EXPERIMENTAL_RUNNER`.  The VM runs on a dedicated
Berry-sized worker stack and is wrapped by the short FruitClaw watchdog guard,
so a wedged script resets the board back into the application instead of
leaving it stuck.  Tools called through the `claw` binding reuse that outer
Berry guard instead of trying to acquire a nested watchdog guard.  This lets
Berry scripts schedule jobs, run bounded terminal commands, or drive NeoPixels
while the whole script remains covered by one recovery guard.

Example script:

```be
import claw
claw.reply(claw.terminal_run("uname -a"))
```

That can be written through MCP with `file.write_limited` under
`scripts/example.be`, then run with `berry.run_script`.

Uploaded Berry/NSH scripts can live directly under `scripts/`, for example
when copied over FTP. Generated scripts created by FruitClaw tools live under
`scripts/generated/` so agent-authored files stay separate from hand-uploaded
files. The script tools provide one lifecycle for both groups:
`script.write`, `script.read`, `script.validate`, `script.run`,
`script.schedule`, and `script.remove` support Berry `.be` scripts and NSH
`.nsh` scripts. `script.write` and `script.remove` are intentionally limited
to `scripts/generated/`; `script.read`, `script.validate`, `script.run`, and
`script.schedule` also accept root-level uploaded paths such as
`scripts/hello.be`. A scheduled script fires `script.run` directly, so the
scheduler does not need another LLM turn to run it. Use `script.schedule` with
`{"type":"boot"}` to autostart a generated or uploaded maintenance script or
LVGL UI every time FruitClaw starts. `script.list` reports both
FruitClaw-generated scripts and uploaded root-level scripts, including a
`source` field, stored script descriptions, last validation metadata, and the
schedule IDs/types/timing that currently invoke each script. MCP clients can
answer what scripts exist, what they do, whether the last validation passed,
and which ones are active without manually joining `scheduler.list`.
`scheduler.list` also returns a structured `jobs[]` array, in addition to the
legacy human-readable `schedules` string. Each job includes its id, type,
enabled state, prompt, owner context, timing fields, and a parsed `script`
object when the schedule was created by `script.schedule` or a direct
`berry.run_script` schedule.

Example generated Berry script for an MCP or Telegram request like "make a
script that turns the NeoPixels off":

```be
import claw
claw.reply(claw.neopixels_off())
```

For a rainbow script:

```be
import claw
claw.reply(claw.neopixels_effect("rainbow"))
```

After `script.write`, use `script.validate` and `script.run` to check the
behavior. Short maintenance scripts can keep the default run validation. For
LVGL UIs or other long-running Berry scripts, pass
`"validate_mode":"syntax"` to `script.write`, or `"mode":"syntax"` to
`script.validate`, so FruitClaw parses the script without executing
`lv.run()` forever. FruitClaw stores a small metadata sidecar for generated
scripts and for uploaded scripts after validation, so `script.list` and
`script.read` show whether the last validation passed. If the user says the
result is not right, read the script with
`script.read`, rewrite it with `script.write`, and validate again.
Schedule it with `script.schedule` using `boot`, `once`, `interval`, or
`cron`.

As of the current hardware slice, `fruitclaw berry-smoke`,
`fruitclaw selftest`, MCP `berry.run_script`, `import claw`, and Berry scripts
calling back into `terminal.run`, `scheduler.add`, and the generated script
workflow have been covered by code or hardware smoke tests. The hardware
selftest also proves that a scheduled `tool:...` event runs directly with the
creator's owner chat/session context, that both `scheduler.add` and
`script.schedule` can persist boot/autostart jobs, that generated scripts can
be listed, read, scheduled, and removed through public tools, and that
FTP-style uploaded Berry scripts under `scripts/` can be listed, read,
syntax-validated, run, and scheduled. More complex Berry/LVGL UI generation
remains active bring-up work.

## Recovery Model

The most important rule is that a failed unattended run must recover without a
human pressing reset.  The current guarded alpha image does that with watchdog
resets that set the Fruit Jam bootguard scratch state and return the RP2350 to
ROM BOOTSEL for reflashing.  Production-style images can later disable BOOTSEL
conversion so watchdog trips reset back into the application.

When `CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD=y`, risky owner operations arm the
RP23xx watchdog before they run.  Berry scripts, captured terminal commands,
script/note file writes, scheduler mutation, NeoPixels, `/dev` writes, LLM
calls, HTTP paths, and MCP owner calls therefore cannot wedge the board
forever.  If the operation does not unwind, the watchdog resets the board.

`CONFIG_FRUITCLAW_GUARD_BOOTSEL_RECOVERY` is enabled in the guarded alpha
profile. FruitClaw writes the Fruit Jam bootguard scratch words before guarded
watchdog paths, so a wedged owner operation returns the board to ROM BOOTSEL
and the host can flash the next UF2 without physical button babysitting.

When `CONFIG_FRUITCLAW_ENABLE_SESSION_GUARD=y`, `fruitclaw start` also starts a
continuous progress watchdog.  Workers heartbeat from the runtime loop, agent,
Telegram poller, scheduler, webserver/MCP handler, and network-recovery path.
The watchdog is kept alive only while those heartbeats remain fresh.  If
operator-visible progress is stale for the configured timeout, FruitClaw stops
feeding the watchdog and the board resets to the configured target.

The session feeder uses
`CONFIG_FRUITCLAW_SESSION_GUARD_HW_TIMEOUT_MS`, which is intentionally longer
than the fast `CONFIG_FRUITCLAW_GUARD_TIMEOUT_MS` used by risky tools.  Short
Berry, terminal, device, NeoPixel, MCP owner, and file-write operations still
recover quickly; ordinary Wi-Fi, TLS, SD, and scheduler jitter get a larger
hardware feed window before the session guard declares the runtime wedged.
`fruitclaw boot` uses the longer operator guard around bootstrap so first-time
SD data-root creation does not false-trip the short 12 second tool watchdog.
`fruitclaw wifi-up` uses the operator/HTTP/CLI guard timeout directly; it must
not be stretched to the full session timeout, because a wedged ESP-hosted
association or DHCP command would otherwise leave CDC and MCP stale for the
whole operator recovery window.

Only one short/long risky-operation guard may own the RP2350 watchdog at a
time.  If a Berry/terminal/device/MCP path is already guarded, a second risky
guard waits only up to `CONFIG_FRUITCLAW_GUARD_ACQUIRE_TIMEOUT_MS` before it
returns a busy error instead of stealing or disarming the active watchdog.  The
interactive value is short on purpose: foreground MCP should fail fast behind a
background Telegram notification rather than look hung to Hermes or Codex.

The Fruit Jam profile leaves the full-turn agent guard disabled with
`CONFIG_FRUITCLAW_AGENT_GUARD_TIMEOUT_MS=0`.  That is intentional: when the
outer agent guard owns the watchdog, the narrower DeepSeek and Telegram guards
are same-thread re-entrant no-ops.  For this profile, responsiveness matters
more than one coarse full-turn deadline, so DeepSeek is bounded by
`CONFIG_FRUITCLAW_DEEPSEEK_HTTP_TIMEOUT_SEC=15` plus
`CONFIG_FRUITCLAW_LLM_GUARD_TIMEOUT_MS=60000`, and replies/notifications are
bounded by the HTTP guards.

HTTP is serialized through one native webclient lane.  The HTTP watchdog guard
starts only after that shared HTTP mutex is acquired, so Telegram polling,
Telegram sends, DeepSeek, MCP `http.request`, and agent replies do not reset
the board merely because they are waiting behind another normal HTTPS request.
If the actual HTTP/TLS operation wedges after it owns the lane, the long guard
uses an immediate watchdog trigger with the FruitClaw stage word recorded.

The Fruit Jam guarded alpha profile sets
`CONFIG_FRUITCLAW_TELEGRAM_HTTP_TIMEOUT_SEC=8` and
`CONFIG_FRUITCLAW_TELEGRAM_HTTP_GUARD_TIMEOUT_MS=0` for Telegram polls and
sends.  Telegram is frequent background traffic, so this profile uses
short-poll mode, a short webclient timeout, and an isolated unlocked HTTP path
instead of letting a Telegram HTTPS problem own the global HTTP guard.  A
half-up Telegram transport is treated as degraded and logged; it should not
force MCP/webserver recovery by itself.  MCP tool-call notifications are
attempted only after Telegram has had a recent successful transport, so a
half-up Wi-Fi stack does not let notifications become the first expensive HTTPS
operation.

The Telegram poller deliberately uses short-poll mode
(`CONFIG_FRUITCLAW_TELEGRAM_POLL_TIMEOUT_SEC=0`) plus
`CONFIG_FRUITCLAW_TELEGRAM_POLL_IDLE_MS=5000` between successful polls, so
Telegram does not hold the HTTP lane open long enough to starve MCP.  In the
current profile, Telegram waits 30 seconds after runtime start and
recent MCP activity makes the Telegram poller yield until the board has been
MCP-idle for 60 seconds, capped by
`CONFIG_FRUITCLAW_TELEGRAM_MCP_MAX_YIELD_MS`.  This
keeps MCP and the webserver responsive during long Hermes/Codex test runs
without starving owner notifications when the test harness polls MCP status.
MCP tool-call Telegram notifications are queued asynchronously and yield to
active Telegram polling before sending; if Telegram has not yet had a recent
successful transport, notification batches fail fast instead of becoming the
first HTTPS operation on a half-up network stack.
Queued or in-flight notifications can take priority over polling only up to
`CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_MAX_YIELD_MS`, so a slow `sendMessage` path
does not stop inbound Telegram polling indefinitely.
Successful MCP `system.status` notifications are rate-limited by
`CONFIG_FRUITCLAW_MCP_NOTIFY_STATUS_INTERVAL_MS`, while errors still notify
immediately; this keeps host soaks and health checks from flooding the owner
chat.

Foreground diagnostic commands such as `fruitclaw status` and
`fruitclaw mcp status` are wrapped by the CLI guard, but `fruitclaw status` is
kept to a bounded health summary and does not dump the full tool inventory or
schedule list.  Use `fruitclaw tools`, `fruitclaw schedule list`, and
`fruitclaw mcp status` for the longer views.  Timeouts are reported with their
stage words such as `FCLI`, `FCLM`, `FCTG`, `FCHU`, `FCHT`, `FCNR`, `FCTM`, or
`FCBE`.  `FCTG` is
Telegram HTTP, `FCHU` is the owner-exposed `http.request` tool, `FCNR` is stale
network recovery, and `FCHT` is reserved for shared/generic HTTP paths that have
not been narrowed further.

Network recovery is deliberately supervised.  Telegram/network staleness first
starts a detached Wi-Fi/service recovery worker; while that worker is active,
`fruitclaw status-net` skips live netlib probes so serial NSH stays responsive.
If recovery runs longer than the operator recovery budget, the operator guard
records `FCNR` and hands off to the configured watchdog recovery target instead
of leaving the board half-alive with MCP unreachable.

The webserver supervisor also watches HTTPD response-send health.  A half-up
ESP-Hosted path can still accept TCP connections and parse MCP requests while
every response `send()` times out.  HTTPD therefore records
`send_fail_streak`, and three consecutive transport failures such as
`ETIMEDOUT` cause the service supervisor to request network recovery with the
`http-send-fail` reason instead of reporting the MCP/webserver path as healthy
just because the listener is still bound.

Read-only diagnostics must remain available while recovery owns the watchdog.
`fruitclaw status`, `fruitclaw status-net`, and `fruitclaw service status ...`
therefore fall back to unguarded status printing if the CLI guard is busy,
instead of hiding the recovery state behind `guard start failed`.

This guarded alpha profile sets:

```text
CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS=600000
CONFIG_FRUITCLAW_GUARD_BOOTSEL_RECOVERY=y
```

The session progress watchdog and per-operation guards recover real stalls.
The temporary max-uptime fuse returns the board to ROM BOOTSEL after about
10 minutes, and guarded hangs also target ROM BOOTSEL.  Disable both behaviors
only for final production-style images after the alpha is stable.

Use this before long hardware runs:

```sh
fruitclaw guard-test
fruitclaw status
python3 apps/system/fruitclaw/tools/fruitclaw_soak.py \
  --duration-sec 30 --interval-sec 5 --telnet-cr-smoke --no-recover
```

`guard-test` intentionally trips the short watchdog path.  `status` reports the
guard state, whether BOOTSEL conversion is enabled, the active recovery target,
whether the continuous session watchdog is armed, the active and last guard
stage words, last heartbeat source, and stale timeout.  It also reports queue
depths, router delivery counters, agent event/tool/reply counters, Telegram
poll/update counters, scheduler tick/fire counters, DeepSeek call counters,
Berry runner/call counters, network-recovery counters, webserver
supervisor/listener counters, telnet/FTP service start counters, MCP
request/tool counters, visible tool count, and the last tool error.  During
unattended work, a healthy idle board should show empty queues, recent Telegram
or MCP/runtime heartbeat ages, `telegram_status` with successful HTTP 200
polls, `agent_status` with no new failures, `scheduler_status` with recent
ticks, `berry_status` showing the real runner, `network_recovery` with no
failures, `webserver` with `supervisor=started listening=yes`, `services` with
telnetd/FTPD started when configured, `mcp_status` with no request/tool
failures, `deepseek_status` with HTTP 200 after a model call, and
`last_tool_error: none`.

MCP and Telegram-agent tool calls can use `system.status` for the same
non-secret runtime health counters without shelling out to
`terminal.run fruitclaw status`.

For host-side overnight/day-long runs, use the soak harness from the checkout:

```sh
apps/system/fruitclaw/tools/fruitclaw_soak.py \
  --device-ip DEVICE_IP \
  --duration-sec 0 \
  --interval-sec 60 \
  --require-serial \
  --check-docs \
  --min-mcp-tools 20 \
  --scheduler-smoke \
  --reset-smoke \
  --recovery-attempts 0 \
  --recovery-retry-delay-sec 10 \
  --manual-bootsel-wait-sec 900 \
  --flash-uf2 /path/to/nuttx.uf2 \
  --continue-after-flash \
  --log /tmp/fruitclaw-soak.jsonl
```

The harness treats progress as proven only when MCP `/mcp` answers, MCP
`tools/list` exposes the expected tool surface, the static docs root answers,
a one-shot schedule can be added, fired, routed through the agent, and removed,
a persistent marker survives a controlled `fruitclaw reboot`, MCP and CDC come
back after that reboot, the data root is still the configured FruitClaw root,
a captured `fruitclaw status` has healthy queues, no router delivery failures,
no agent event/tool/reply failures beyond the configured limits, no scheduler
publish failures or stale scheduler tick, no new network-recovery failures, a
listening webserver supervisor, no MCP request/tool failures, fresh Telegram
telemetry, and the CDC serial prompt answers.  Cumulative failure counters are
baselined when the harness starts, so a recovered outage that happened before
the soak does not poison the whole run; new counter deltas still fail the run.
If MCP/status/CDC progress goes stale, the harness first tries non-invasive
checks before it escalates: it retries the current MCP endpoint, asks the board
over CDC for the current `ifconfig wlan0` address, updates the default MCP URL
when DHCP moved, and then retries the failed HTTP/MCP check. If MCP is still
down but CDC is alive, it runs `fruitclaw wifi-up` over serial and retries MCP
again before declaring the app-side network path unrecovered. Only after that
does it use reset/recovery paths and then the explicit BOOTSEL paths needed for
reflashing: `picotool reboot -u -f`, MCP `terminal.run bootsel`, optional legacy HTTP
BOOTSEL URLs, telnet `fruitclaw recover`, serial `fruitclaw recover`, direct
`bootsel` fallbacks, then a 1200-baud CDC touch.  The FruitClaw recovery
command intentionally uses the watchdog/bootguard scratch path instead of
turning bootguard off before BOOTSEL; a stale USB/network runtime may be too
unhealthy for direct ROM BOOTSEL reboot to complete.
It uses `picotool info -a` as the authoritative BOOTSEL gate for flashing,
never a stale `/dev/cu.usbmodem*` node.  Without `--flash-uf2`, the harness
exits after recovering to BOOTSEL so a failed run stops in a flashable state.
With `--flash-uf2 nuttx/nuttx.uf2 --continue-after-flash`, it can reflash and
continue the soak loop.  `--recovery-attempts 0` means keep retrying recovery
forever; use a positive value for a bounded smoke test.  If a temporary image
has the max-uptime fuse and BOOTSEL conversion enabled, add
`--manual-bootsel-wait-sec 900` so the host keeps polling `picotool info -a`
after active recovery methods fail and flashes as soon as the delayed BOOTSEL
window appears.

Default host recovery probes are intentionally broad and bounded:

```text
picotool reboot -u -f
MCP POST /mcp tools/call terminal.run bootsel
GET http://DEVICE_IP/bootsel
GET http://DEVICE_IP/cgi-bin/bootsel
GET http://DEVICE_IP/fruitjamctl/bootsel
telnet DEVICE_IP 23: fruitclaw recover; bootsel fallback
serial: fruitclaw recover; bootsel fallback
serial: 1200-baud touch
picotool info -a
```

The HTTP and telnet probes are safe to leave enabled during bring-up because
they have short host-side timeouts.  Disable them with `--no-http-recovery` or
`--no-telnet-recovery` when testing a narrower path.  Disable the direct
picotool forced reboot probe with `--no-picotool-force-reboot`.

The serial checks run in short-lived child processes.  This is intentional:
macOS can block inside tty setup when a stale NuttX CDC node remains after a
wedged board.  The parent harness kills that child after the serial timeout, so
stale CDC is treated as failed progress rather than freezing the overnight run.
Because the board can still be healthy over MCP while one CDC open races USB
reattachment, `--max-serial-failures` defaults to three consecutive prompt
failures before serial alone can force recovery.  Use `--max-serial-failures 0`
when deliberately testing CDC/NSH recovery.
Likewise, MCP/web reachability must fail across `--max-mcp-health-failures`
consecutive checks before host recovery starts.  The default of three gives
the board-side network supervisor and TCP state a short window to settle while
still preserving the BOOTSEL escape for true hangs.

## MCP YOLO Endpoint

FruitClaw can expose a minimal MCP Streamable HTTP JSON-RPC endpoint at:

```text
POST http://DEVICE_IP/mcp
```

This endpoint is intentionally YOLO mode during bring-up:

- no bearer token
- no token file
- no origin check
- no read-only MCP policy layer
- MCP tool calls run with `owner_mode=true`

MCP reuses the existing `fc_cap` registry.  Tool names are native FruitClaw dot
names such as `time.now`, `system.status`, `service.status`,
`service.control`, `terminal.run`, `berry.run_script`, `script.run`,
`scheduler.add`, `device.list`, and `neopixels.set`. DeepSeek still receives the
OpenAI-compatible underscore mapping internally.

Start the runtime:

```sh
fruitclaw boot &
```

With `CONFIG_FRUITCLAW_BOOT_AUTOSTART=y`, the board `rcS` script runs that
same command automatically after reset.  `fruitclaw start &` also starts the
same FruitClaw-owned webserver supervisor when
`CONFIG_FRUITCLAW_BOOT_START_WEBSERVER=y`, so Telegram and MCP can run together
from the ordinary foreground runtime path.  If you prefer very manual bring-up,
leave autostart disabled and run `fruitclaw wifi-up` before `fruitclaw start &`.

`fruitclaw boot` registers the MCP route, starts the FruitClaw webserver
supervisor, launches the runtime workers, and keeps ESP-Hosted/Wi-Fi/NTP
bring-up in a detached retry thread.  The supervisor calls the existing uIP
`httpd_listen()` loop directly and restarts it if the listener exits, so MCP
can come back after a Wi-Fi/interface recovery without a separate `webserver &`
task.  Slow or failed Wi-Fi therefore shows up in `fruitclaw status` instead
of preventing the MCP route, Telegram worker, scheduler, and agent worker from
existing.  The Fruit Jam profile leaves board-level ESP-Hosted autostart off;
FruitClaw starts it from `wifi-up` so the agent owns the boot order.

The FruitClaw profile keeps the uIP webserver bounded with
`CONFIG_NETUTILS_HTTPD_TIMEOUT=3`.  FruitClaw also applies that timeout to
accepted socket receive and send paths, because a single idle or slow HTTP
client must not monopolize the small embedded webserver and starve MCP or the
docs.  Telegram gets a separate first-success grace window before network
recovery is attempted, so early Telegram HTTPS/backoff during boot does not
tear down Wi-Fi while MCP is coming online.

Curl smoke tests:

```sh
curl -i http://DEVICE_IP/mcp \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-11-25' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'

curl -i http://DEVICE_IP/mcp \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'

curl -i http://DEVICE_IP/mcp \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json' \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"time.now","arguments":{}}}'
```

`GET /mcp` returns `405` with `Allow: POST, OPTIONS`.  `OPTIONS /mcp` returns a
no-body allow response.  There is no WebSocket or SSE server in this slice.
The Fruit Jam operator profile sets a small HTTP receive timeout so one stalled
client cannot monopolize a webserver worker indefinitely, and keeps the MCP
POST body limit at 16 KiB for larger tool arguments.

## YOLO Owner Model

- MCP is currently trusted YOLO mode when enabled.  It has no auth layer, no
  bearer token, no token file, and no read-only policy layer.  MCP tool calls
  run with `owner_mode=true` for local bring-up.
- Local CLI and allowlisted Telegram chats are owner-capable by design:
  terminal, scheduler, Berry, device, file-write, and NeoPixel tools may run
  without per-action confirmation.
- Telegram messages are still ignored unless the chat ID is allowlisted, so the
  internet-facing bot is not open to every Telegram user by accident.
- File tools are jailed under the active FruitClaw data root and reject `../`.
- The `secrets/` directory is never readable through LLM-visible file tools.
- Generic device tools reject block devices before opening them; this prevents
  owner-mode MCP/Telegram calls from wedging the board by reading SD media
  paths directly.
- API keys, bot tokens, and Authorization headers are never logged or sent to
  the LLM.
- Tool loops stop at `CONFIG_FRUITCLAW_MAX_TOOL_ITERATIONS`.
- Risky local execution paths such as Berry scripts, captured terminal
  commands, and long LLM calls are wrapped by the RP23xx watchdog guard when
  `CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD=y`.  In this guarded alpha profile, a
  wedged operation returns the board to BOOTSEL.
  `fruitclaw guard-test` intentionally trips this path.
- Secrets remain unreadable to tools even in owner mode.

## Current Limitations

- Non-streaming DeepSeek only.
- Telegram text messages only.
- Telegram outbound sending, long polling, and a real inbound allowlisted chat
  message have been verified on hardware.  The local
  `fruitclaw telegram-inject <message>` hook remains useful for repeatable
  agent tests without touching the Telegram desktop account.
- Plain scheduled reminders now bypass DeepSeek at fire time; explicit
  `agent:` scheduled jobs still use the full LLM/tool path and need longer soak
  coverage.
- Router rules currently use the built-in default route:
  Telegram/CLI/scheduler events go to the agent.
- GPIO tools are stubs.
- HTTPS needs a CA bundle unless the explicit unverified-TLS bring-up option is
  enabled.
- The max-uptime BOOTSEL fuse is enabled in the Fruit Jam `esp-hosted`
  guarded alpha profile so unattended tests are always recoverable. Disable it
  only for final production-style images.
