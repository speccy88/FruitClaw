# FruitClaw Wi-Fi Reliability Notes

Last verified: 2026-07-07 on `adafruit-fruit-jam-rp2350:esp-hosted`.

## Failure Mode

FruitClaw could associate to Wi-Fi, get DHCP, and accept inbound TCP on
`192.168.1.7`, but HTTP/MCP, Telnet, and FTP sometimes returned no payload.
The symptom was not bad credentials or bad DHCP: `/mcp` and `/docs/index.html`
connections reached the board, HTTP parsed requests, and the ESP-Hosted SPI
framing counters stayed clean.

The key failed state was:

- TCP connect succeeded from the host.
- HTTP handler counters showed parsed requests but no sent bytes.
- Telnet and FTP ports opened but did not produce banners.
- `/proc/iobinfo` and TCP write-buffer counters were healthy.
- ESP-Hosted showed no checksum, malformed-frame, SPI TX, or parse errors.

## Root Cause

The response path was stuck between NuttX buffered TCP send and the netdev
upper-half transmit poll. SYN/SYN-ACK traffic could still work because it was
handled during RX processing, but application payloads queued by `send()` need
an explicit TX wakeup.

For `NETDEV_RX_THREAD` devices, FruitClaw's ESP-Hosted lower half uses a
dedicated netdev thread. The generic upper-half worker previously serviced RX
before TX for all wakeups. Under inbound service traffic and ESP-Hosted idle
polling, a TX wake from user-space TCP could be delayed behind RX work, leaving
HTTP/Telnet/FTP/MCP payloads queued even though the socket was accepted.

The fix is to preserve queued-thread safety but mark explicit TX wakeups with
`txrequested`. The netdev thread now performs a TX-first pass for that wake,
then RX, then a second TX pass in case RX released quota or created replies.

## Related Probe Bug

FruitClaw also misreported Wi-Fi health after the transport fix. Two mistakes
made `fruitclaw wifi-probe` look worse than real packet flow:

- `netlib_check_*connectivity()` returns a positive reply count on success;
  FruitClaw treated a positive gateway reply as a failure in one branch.
- The ping helper's timeout value is used as milliseconds by `icmp_ping()`.
  Passing `1` meant a 1 ms timeout, not a 1 second timeout.

The active probe now uses three 1500 ms samples for the gateway and internet,
stores positive reply counts, and refreshes `last_ok_ms` on success.

## Coexistence Fixes

Telegram polling and MCP owner notifications had a startup deadlock: queued
MCP notification messages made the Telegram poller yield to the notification
sender, while the notification sender waited for Telegram to have a recent
successful transport before sending. The fixed poller always allows the first
`getUpdates` request to establish transport before notifications get priority.

Notification priority is now bounded by
`CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_MAX_YIELD_MS`. This lets MCP tool-call
notices go out over Telegram, but prevents queued or in-flight notifications
from stopping inbound Telegram polling indefinitely.

Telnet autostart also needed hardening. A boot-time `telnetd -4 &` command can
return success before the listener is actually ready, and the FruitClaw service
probe may then mark Telnet stopped. Boot now starts Telnet with a bounded
start/probe/retry loop, and the service supervisor restarts enabled Telnet/FTP
services that are later found stopped before escalating to full network
recovery.

## Current Transport Model

- ESP-Hosted RX/TX progress remains task-context safe.
- Socket services use small send slices and activity hooks so FruitClaw can
  pump ESP-Hosted progress while waiting for TCP progress.
- HTTP status includes TCP send diagnostics:
  `tcp_send_calls`, `tcp_send_ok`, `tcp_send_eagain`,
  `tcp_send_cb_fail`, `tcp_send_wrb_fail`, `tcp_send_iob_fail`,
  `tcp_send_txnotify`, and `tcp_send_queued`.
- `esphostedctl` should stay clean under service load:
  `checksum_error=0`, `malformed_frame=0`, `tx_spi_error=0`,
  `tx_parse_error=0`, and no growing recovery count.

## Shared Peripheral Recovery

The Fruit Jam ESP32-C6/NINA reset path can disturb other board peripherals.
Do not treat Wi-Fi reset as isolated board state.  On this board GPIO22 is
`BOARD_NINA_RESET_PIN`, and the audio codec setup also uses it as the DAC reset
pin.  Therefore the first normal `board_fruitjam_esp_hosted_start()` is already
a shared peripheral reset event, not only `wifi-up --force` or later recovery.

- After any successful ESP-Hosted start or true ESP-Hosted recovery, rerun the
  Fruit Jam shared peripheral recovery path before playing RTTTL or using
  audio demos. Otherwise RTTTL can chirp once or fail with
  `audio playback failed: -110` after Wi-Fi has reset the shared line.
- The verified implementation keeps this invariant in the board layer:
  `board_fruitjam_esp_hosted_start()` and `board_fruitjam_esp_hosted_recover()`
  call `board_fruitjam_shared_peripherals_recover()` only after ESP-Hosted
  reset/start succeeds.  FruitClaw should not separately poke this path after a
  normal Wi-Fi config reapply unless a real ESP-Hosted reset occurred.
- The `rtttl.play` FruitClaw tool performs codec recovery before launching the
  RTTTL app.
- RTTTL reliability also depends on the RP23xx I2S PIO state machine remaining
  reserved for audio.  The FruitClaw profile uses `CONFIG_RP23XX_I2S_PIO=1`
  and `CONFIG_RP23XX_I2S_PIO_SM=0`; the WS2812 lower half must skip that
  configured SM when opening `/dev/leds0`, or NeoPixel activity can steal the
  state machine that RTTTL later assumes is available.
- USB-host reliability depends on the same rule.  The FruitClaw profile uses
  PIO USB-host on `CONFIG_RP23XX_PIO_USBHOST_PIO=0` with SM0/SM1/SM2.  Do not
  let the WS2812 lower half allocate or reconfigure that PIO block while
  `piousbhost` is running.  The verified fix is to keep the five onboard
  NeoPixels on a separate RP2350B PIO block and send the tiny LED frame with a
  bounded direct PIO FIFO write instead of an unbounded DMA completion wait.
  The failure marker for this bug was BOOTSEL reboot parameter `0x46434e50`
  (`FCNP`) after `fruitclaw neopixels blue`.
- Read-only diagnostics must stay read-only.  `fruitclaw status` and
  `fruitclaw mcp status` should not call full `fc_bootstrap()` when FruitClaw
  is not already initialized; otherwise a harmless status check can register
  HTTP/MCP routes and start network recovery while the board is supposed to be
  in a manual hardware-baseline state.
- When rebuilding audio or NeoPixel fixes, force the RP23xx driver objects to
  rebuild or use a helper that syncs them.  A UF2 that only relinks FruitClaw
  app code can leave stale `rp23xx_i2s.o` or `rp23xx_ws2812.o` behavior in
  place.
- FruitClaw `wifi-up --force` should prefer `ifdown` plus Wi-Fi config reapply
  before escalating to a full ESP-Hosted recovery.  Shared peripheral recovery
  is handled by the board ESP-Hosted start/recover path after a real reset.
- NeoPixels on this RP23xx build use the non-SPI PIO WS2812 lower half. Keep
  `CONFIG_WS2812_FREQUENCY=800000`; SPI-style multi-MHz values make the tool
  return success while `/dev/leds0` emits an invalid waveform.
- For the FruitClaw DVI operator profile, keep the documented 320x240 RGB565
  framebuffer and 25.2 MHz pixel clock, but use the proven split-buffer shape:
  the application framebuffer in PSRAM and one live scanout frame in internal
  SRAM (`CONFIG_RP23XX_HSTX_DVI_SCANOUT_PSRAM=y` plus
  `CONFIG_RP23XX_HSTX_DVI_SINGLE_SCANOUT_INTERNAL=y`). A PSRAM-only scanout can
  register `/dev/fb0` and show clean counters while the monitor still reports no
  signal.
- The single internal scanout needs about 150 KiB of internal SRAM. The current
  FruitClaw profile keeps command input length at 1024 bytes but reduces shell
  history depth with `CONFIG_READLINE_CMD_HISTORY_LEN=8` so `/dev/fb0` can
  register without shrinking Wi-Fi/TCP buffers.

## Boot Ordering Note

Do not start the full FruitClaw network/service stack before the basic board
surface is controllable.  A bad early Wi-Fi/service path can otherwise leave
CDC registered but stale, Wi-Fi offline, and no useful BOOTSEL recovery path.

For the FruitClaw alpha profile, the generated `rcS` should stage boot in this
order:

```sh
dvictrl start
piousbhost start-guarded
bootguard off
fruitclaw boot &
```

FruitClaw then starts its own session guard before asynchronous Wi-Fi,
webserver, Telnet, FTP, MCP, Telegram, and scheduler startup.  This makes the
DVI/shell/USB-host baseline independent of the higher-risk networking path and
keeps a watchdog owner active while Wi-Fi is being brought up.

## Passing Evidence

### Shared Reset Boardfix Smoke

UF2 hash:

```text
76181981c6afb95535737088d1a0f2bf3f261694814f11cc69ee5d8ffbe6ac09  artifacts/fruitclaw-alpha-shared-reset-boardfix-20260707.uf2
```

Focused serial sequence:

```text
dvictrl info
  video: 320x240 fmt=11
  actual_pixel=25200000
  dma_errors=0

rtttl -v 35 simpsons
  returned to nsh prompt; user confirmed tune played

fruitclaw wifi-up
  Wi-Fi connectivity ok: gateway_probe=3 internet_probe=3
  ip=192.168.1.7 gw=192.168.1.1

rtttl -v 35 simpsons
  returned to nsh prompt after Wi-Fi; user confirmed RTTTL worked

fruitclaw neopixels blue
  {"ok":true,"device":"/dev/leds0","effect":"fill"}

fruitclaw neopixels off
  {"ok":true,"device":"/dev/leds0","effect":"off"}

ping 192.168.1.1
  10 packets transmitted, 10 received, 0% packet loss
  rtt min/avg/max = 10/76/130 ms

esphostedctl
  checksum_error=0 malformed_frame=0 tx_spi_error=0 tx_parse_error=0
  rx_dropped=0 rx_echo_dropped=0
```

Watch item: this pass still showed a very high data-ready IRQ count and
`rx_work_busy` count during a short test, while packet loss was 0% and all
error counters stayed clean.  Keep it as an optimization target, but do not
destabilize the working shared-reset/audio/DVI/NeoPixel path just to reduce the
counter.

### Earlier HTTP/MCP/Telnet/FTP Pass

UF2 hash:

```text
af4e14af83df77bc6b14316be6d6ca7509f54cded353e36245a8be9cfad9abbb  nuttx/nuttx.uf2
```

Board-side:

```text
fruitclaw status-net
  boot_network: started=yes done=yes ok=yes
  wifi_probe_cache: last_ok_age_ms=... gateway=3 internet=3
  telnetd: autostart=yes started=yes listening=yes probe=0
  ftpd: autostart=yes started=yes listening=yes probe=0
  webserver: compiled=yes running=yes

fruitclaw wifi-probe
  Wi-Fi probe: ret=3 gateway_probe=3 internet_probe=3

fruitclaw httpd-status
  sends=74 sent=74 send_fail=0
  tcp_send_calls=... tcp_send_ok=... tcp_send_eagain=0
  tcp_send_cb_fail=0 tcp_send_wrb_fail=0 tcp_send_iob_fail=0
  tcp_send_txnotify=...

system.status
  visible_tools=32
  network_recovery: attempts=0 failures=0
  services: telnetd=enabled started=yes telnet_listening=yes
  services: ftpd=enabled started=yes ftpd_listening=yes
  telegram_status: polls=9 fails=0 http=200 notify_sent=3
  mcp_status: requests=11 failures=0 tools=10 tool_failures=0
```

Host-side:

```text
curl http://192.168.1.7/                         -> 266 bytes
curl http://192.168.1.7/docs/index.html          -> 6731 bytes
fruitclaw wifi-probe over Telnet                  -> gateway=3 internet=3
telnet 192.168.1.7 23                            -> NSH banner, uname works
ftp/nc 192.168.1.7 21                            -> 220 NuttX FTP Server
MCP initialize/tools-list/system.status           -> ok
MCP terminal.run "uname -a"                      -> ok
MCP device.list/scheduler.list/neopixels.off      -> ok
ping 192.168.1.7                                 -> 10/10, 0% loss
```

ESP-Hosted stats during MCP/HTTP/Telnet/FTP load:

```text
pump_error=0
control_timeout=0
malformed_frame=0
checksum_error=0
tx_frame and rx_frame advancing
```

## Build Hygiene Note

During this fix, an old generated object `nuttx/net/tcp_send_unbuffered.o`
remained from an earlier config and was archived together with
`tcp_send_buffered.o`, causing duplicate `psock_tcp_send` symbols at link time.
Deleting the stale generated object and regenerated net archive fixed it:

```sh
rm -f nuttx/net/tcp_send_unbuffered.o nuttx/net/libnet.a nuttx/staging/libnet.a
make -C nuttx -j8
```

Keep this in mind after toggling `CONFIG_NET_TCP_WRITE_BUFFERS`.
