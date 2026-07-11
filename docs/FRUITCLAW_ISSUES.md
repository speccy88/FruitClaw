# FruitClaw Issue List

- RESOLVED 2026-07-07 shared ESP32-C6/audio reset regression: the Fruit Jam
  NINA reset line (`BOARD_NINA_RESET_PIN`, GPIO22) is also the codec reset
  path, so `board_fruitjam_esp_hosted_start()` reset the DAC during normal
  first Wi-Fi bring-up.  The recovery is now in the board layer: every
  successful ESP-Hosted start or recover immediately calls
  `board_fruitjam_shared_peripherals_recover()`, reinitializing the audio codec
  and reopening `/dev/leds0`.  UF2
  `76181981c6afb95535737088d1a0f2bf3f261694814f11cc69ee5d8ffbe6ac09`
  verifies `rtttl -v 35 simpsons` before Wi-Fi, `fruitclaw wifi-up`, the same
  RTTTL command after Wi-Fi, `fruitclaw neopixels blue/off`, and DVI
  `dvictrl info` all return cleanly.
- VERIFIED 2026-07-07 Wi-Fi after shared-reset boardfix: `fruitclaw wifi-up`
  completed on the first DHCP attempt and reported `gateway_probe=3
  internet_probe=3`; `ping 192.168.1.1` returned 10/10 with 0% loss and
  10-130 ms RTT.  `esphostedctl` still showed a very high data-ready interrupt
  and `rx_work_busy` count with no checksum/malformed/SPI/TX errors, so this
  is a remaining optimization/watch item, not a blocker for this smoke pass.
- RESOLVED 2026-07-07 manual baseline/status side effect: `fruitclaw status`
  and `fruitclaw mcp status` used to call full `fc_bootstrap()`, which
  registered web/MCP endpoints and could trigger Wi-Fi/HTTP recovery while the
  user only asked for diagnostics.  UF2
  `36d45147f21804c1d75e20cf71d76f39ab9a82e89a6480d991e49aa8aa7e277b`
  verifies read-only status stays passive: bootstrap remains `not ready`,
  MCP route stays `registered=no`, and DVI/serial stay live.
- RESOLVED 2026-07-07 NeoPixels wedged board with BOOTSEL marker `FCNP`:
  `/dev/leds0` could choose the same PIO block as PIO USB-host or I2S, then
  reinitialize that PIO while those services were live.  The WS2812 lower half
  now skips `CONFIG_RP23XX_PIO_USBHOST_PIO` and `CONFIG_RP23XX_I2S_PIO`, uses
  bounded direct PIO FIFO writes for the five onboard LEDs, and avoids holding
  the LED device mutex forever if the PIO stalls.  UF2
  `2744d74aa35b259479fbf1a777ab193dbfe381dbb685d59990b33268216dc1ef`
  verifies `fruitclaw neopixels blue`, `echo still-here`,
  `fruitclaw neopixels off`, and `echo after-off` all return without BOOTSEL.
- RESOLVED 2026-07-07 RTTTL chirp/timeout after NeoPixel failure: isolated
  `rtttl -v 45 simpsons` initially chirped then failed with
  `audio playback failed: -110`; after moving WS2812 away from the USB-host and
  I2S PIO blocks, `rtttl -v 45 simpsons` returned cleanly and the user
  confirmed the tune played well.  Treat future RTTTL regressions after LED or
  USB activity as shared PIO/peripheral ownership problems first.
- RESOLVED 2026-07-07 Wi-Fi/MCP/Telnet/FTP no-payload and coexistence failure: TX-requested netdev wakeups, corrected 1500 ms x3 Wi-Fi probe, bounded Telegram notification yield, Telegram first-poll startup fix, and Telnet boot start/probe/retry pass in UF2 `af4e14af83df77bc6b14316be6d6ca7509f54cded353e36245a8be9cfad9abbb`; see `docs/FRUITCLAW_WIFI_RELIABILITY.md`.
- VERIFIED 2026-07-07 on `192.168.1.7`: HTTP root/docs return payloads, MCP has 32 tools and `requests=11 failures=0`, Telnet banner plus `uname -a` works, FTP returns `220 NuttX FTP Server`, `fruitclaw wifi-probe` returns `gateway_probe=3 internet_probe=3`, Telegram reaches `polls=9 fails=0 http=200`, and `esphostedctl` shows `pump_error=0 control_timeout=0 malformed_frame=0 checksum_error=0`.
- OPEN 2026-07-07 combined boot regression: working Wi-Fi/RTTTL/NeoPixels/Telnet/DVI pieces regressed when `rcS` launched `fruitclaw boot &` before DVI/USB-host startup and before FruitClaw owned the session watchdog. Symptom: stale `/dev/cu.usbmodem01` blocks host opens, no Wi-Fi at `192.168.1.7`, and no automatic return to BOOTSEL. Fix in next alpha is staged boot: `dvictrl start`, `piousbhost start-guarded`, `bootguard off`, then `fruitclaw boot &`, plus session guard starts before Wi-Fi/services. Ready UF2 hash is `c4b9c3f24df83c2287c4260a793e70c4106809a2a4c796559b2da3cc8d53dbba`.
- SUPERSEDED 2026-07-07 RTTTL regressed again after Wi-Fi/NeoPixel/DVI
  bring-up: this was traced to the shared NINA/audio reset line during
  ESP-Hosted start/recovery and is resolved by the board-layer shared
  peripheral recovery entry above.
- BUILD HYGIENE 2026-07-07: `scripts/fruitclaw_docker_incremental.sh` did not sync `apps/system/rtttl/`, `rp23xx_i2s.c`, or `rp23xx_ws2812.c`; any audio/NeoPixel UF2 built through the warm Docker helper before this fix could contain stale driver code.
- BOOT GUARD 2026-07-07: boot Wi-Fi used the guarded CLI path and could return to BOOTSEL with `FCWD`. Changing the boot thread to use the un-nested Wi-Fi path helped, but deferring session-guard ownership until after Wi-Fi left a stale-CDC/no-BOOTSEL gap. Current fix starts the FruitClaw session guard before asynchronous Wi-Fi/services.
- BOOT GUARD 2026-07-07: a follow-up half-alive state kept the app running with stale CDC and no Wi-Fi. The operator guard now checks boot-network active age explicitly and forces `FCNR` recovery if the asynchronous boot network worker never finishes.
- READY TO FLASH 2026-07-07: local UF2 `f33dba03e218415c6d381f71001097c80455dc8af93d778f6273f616d27f5e11` includes the force-rebuilt I2S/WS2812 driver objects, deferred boot/session guard fixes, and an independent boot-network deadline guard, but still needs BOOTSEL before it can be flashed and tested.
- CDC serial can remain registered as `/dev/cu.usbmodem01` while blocking host opens, with Wi-Fi/MCP offline; recovery must not depend only on CDC or network.
- Current board evidence: `picotool info -a` reports no BOOTSEL device and `192.168.1.7` has 100% ping loss while stale `/dev/cu.usbmodem01` still exists. A long BOOTSEL watch after flashing UF2 `91d354d06c3e4930d90c0a21b8fc420ef7e27a81589319e87e0359056827802e` did not recover the board, so the current app-running image still needs manual BOOTSEL before the `f33dba03...` boot-network guard image can be installed.
- Telnet parity patch is built but still needs live board verification: visible echo, CR enter, backspace, `help`, `uname -a`, and `ls /dev` should match serial NSH.
- HTTP and Telnet accept TCP connections on `192.168.1.7`, but board-to-host TCP payload sends currently fail/hang: `/site/home.md` parsed then returned curl empty reply, and Telnet accepted input with no banner/output.
- HTTPD now tracks `send_fail_streak`; live MCP smoke produced `send_fail_streak=3` and `last_send_errno=110`, proving the response-send path is the MCP/web failure point.
- Service probes now run the HTTPD passive check even after Wi-Fi recovery clears the last-ok timestamp, and read-only `status`/`service status` can print without a CLI guard when another guard is busy.
- Wi-Fi can report command/DHCP success while real connectivity is dead: board held `192.168.1.7` and ARP resolved, but host ping was 100% loss, board pings returned late/stale ICMP ID warnings, and HTTP activity counters stayed at zero handlers after host curl/telnet attempts.
- `fruitclaw wifi-up --force` now genuinely does `ifdown wlan0`, reapplies Wi-Fi credentials, renews DHCP, and reports probe failure; if this still cannot restore traffic, the next fix likely belongs in ESP-Hosted driver/board reset handling rather than the FruitClaw CLI wrapper.
- ESP-Hosted TX now rejects carrier-off sends and exposes TX diagnostics (`tx_carrier_off`, `tx_spi_error`, `tx_parse_error`, `tx_last_*`); 2026-07-07 smoke showed IP `192.168.1.7`, `wlan_ifup=1`, `wlan_carrier_on=1`, no TX errors, but `wifi-probe` still timed out.
- `status-net`, `wifi-up`, and `wifi-probe` now print interface flags and IP; current failure is specifically IP-up/running/carrier-on but no confirmed gateway/internet connectivity.
- Default router is sane (`192.168.1.1` with `/24` netmask), but gateway ping on 2026-07-07 returned only 2/10 replies with late/out-of-order ICMP warnings, pointing at ESP-Hosted packet latency/loss rather than bad DHCP routing.
- Final guarded flash on 2026-07-07 returned to ROM BOOTSEL with reboot marker `0x46435347` (`FCSG`, session guard) during/after a serial `status-net` retry; the escape path worked, but the trigger still needs root-cause work.
- The 50 ms ESP-Hosted RX poll build still wedged during the old full `status-net` path after printing Wi-Fi/service state, then recovered to ROM BOOTSEL with marker `0x46434c49`; `status-net` is now intentionally shortened and the full subsystem dump stays under plain `status`.
- UF2 `ddc6471b3ece8ab400c832c8de558e1903c04a489c3748a1ec656b6ab53ad880` verifies the shortened `status-net` returns to NSH and `fruitclaw recover` reaches ROM BOOTSEL, but gateway ping remains 2/10 with 540-580 ms late replies; `esphostedctl` shows `wlan_ifup=1`, `wlan_carrier_on=1`, `tx_error=0`, and no SPI/parse TX errors.
- UF2 `701d7872fdf2494654dca8ecb4b63904e25d60af1e0f228117b783116e32cb64` verifies the ESP-Hosted forced RX poll fix: gateway ping improved to 10/10, 0% loss, 10-120 ms RTT, with `rx_forced_poll` increasing and no TX/SPI/parse errors; keep this behavior when tuning Telnet/MCP/Telegram coexistence.
- UF2 `3e7fb469a885af272942fde1fab25f885a9899b69c06490eb44efd445b4e34e2` is the current stronger ESP-Hosted fix: sustained forced burst polling plus no retimering of a queued delayed RX poll; no-`wifi-probe` gateway ping passed 10/10, 0% loss, 10-120 ms RTT, then `fruitclaw recover` returned to BOOTSEL.
- UF2 `6bda77927d8ba638a21669ae1229d721d9759236b5d1ee29457b7c14f51c5ce4` keeps the ESP-Hosted fix and verifies clean split `status-net`: IP `192.168.1.7`, gateway `192.168.1.1`, Telnet/FTP/webserver/MCP up, gateway ping 10/10 with 0% loss and 10-120 ms RTT, then `fruitclaw recover` returned to BOOTSEL.
- Generated Berry/NSH script write/read/list/run/schedule and strict cron validation pass `fruitclaw selftest`; uploaded root-level Berry scripts under `scripts/` now also pass read/validate/run/schedule selftest, but still need live FTP-upload-to-MCP/Hermes verification through HTTP.
- Long CLI diagnostics need explicit session heartbeats while the session guard is enabled; `fruitclaw selftest` now feeds the guard, but other multi-minute CLI paths should be audited the same way.
- RESOLVED 2026-07-07 RTTTL/audio after ESP-Hosted/shared reset activity: Fruit Jam now exposes codec recovery, FruitClaw calls shared peripheral recovery after guarded Wi-Fi bring-up, and `rtttl -v 70 simpsons` returned cleanly; the user confirmed audio output.
- RESOLVED 2026-07-07 NeoPixels returning success but staying dark: RP23xx non-SPI WS2812 timing must use the real WS2812 bit rate, so the FruitClaw profile now uses `CONFIG_WS2812_FREQUENCY=800000`; UF2 `df38475d7540b0c921a3f72991634fed319b330a987dc7d560d0a9f55b0f4f53` was flashed and the user confirmed NeoPixels work.
- RESOLVED 2026-07-07 DVI `/dev/fb0` missing in the full FruitClaw image: the first internal-scanout build could not allocate the ~150 KiB internal scanout frame, so `/dev/fb0` was absent. A PSRAM-scanout-only UF2 `fffd62982d8d2eff774372a7fd8dd41a1e8c18ab5351e1c2aa7841b24d7d804f` registered `/dev/fb0` and reported clean DVI counters, but the monitor still showed no signal. The working fix is UF2 `861368013f140962dd4c39110b86fc092a1548520ddaad8a4e6cf8aacef45fea`, with 320x240 RGB565, 25.2 MHz, PSRAM app framebuffer, single internal SRAM scanout, and `CONFIG_READLINE_CMD_HISTORY_LEN=8` to free internal SRAM while keeping `CONFIG_READLINE_CMD_HISTORY_LINELEN=1024`. User confirmed DVI output after `dvictrl pattern colorbars` and `dvictrl start`.
- DVI still depends on fragile HSTX timing/bootstrap ordering; keep regressions tied back to `nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/TRMNL_DVI_TIMING.md`. For the FruitClaw operator image, the proven shape is 320x240 RGB565 centered 1:1 inside 640x480 timing, `actual_pixel=25200000`, and a single internal SRAM scanout buffer (`scanout [0] ... sram=1`). PSRAM-only scanout can look healthy in `dvictrl info` but fail to produce monitor signal on this panel.
- Audit 2026-07-07 of UF2 `6bda77927d8ba638a21669ae1229d721d9759236b5d1ee29457b7c14f51c5ce4`: serial/local commands stayed responsive with no command timeouts; `status-net`, `config`, `service status`, `mcp status`, `tools`, `terminal-run uname -a`, `device list`, SD listings, `esphostedctl`, and board-side gateway/internet pings all completed before host HTTP testing.
- Audit 2026-07-07: `terminal.run` intentionally refuses to enumerate `ls /dev` and returns a hint to use `device.list`; verify this is acceptable for MCP/operator UX or add a bounded safe implementation for common diagnostic commands.
- Audit 2026-07-07: host HTTP/MCP still fails even though port 80 accepts TCP; `curl` for `/`, `/docs/index.html`, `/docs/index.json`, `/docs/pages/home.md`, `/doc/`, and MCP initialize/tools-list all timed out or returned no payload.
- Audit 2026-07-07: after host HTTP/MCP attempts, board-side diagnostics show the HTTP dispatcher saw MCP initialize internally, but HTTP transport failed to send responses (`send_fail`, `last_send_errno=110`, `last_phase=close-drain`) and the HTTPD restart path hit `last_errno=98`/EADDRINUSE loops.
- Audit 2026-07-07: inbound HTTP/MCP failures can churn ESP-Hosted recovery and degrade Wi-Fi from previously good 10/10 board-side pings to 40% packet loss with `sendto` errno 101 during recovery.
- Audit 2026-07-07: Telnet and FTP ports 23/21 accept TCP from the host on a clean boot, but neither service returned a banner or command response; after these host connections, gateway ping degraded to 2/10 with late replies while serial stayed alive.
- Audit 2026-07-07: local Berry smoke works for `selftest_import_claw.be` (`import claw` plus `claw.reply` returned `ok:true`), but schedule mutation and generated-script MCP flows were not exercised because this audit avoided persistent side effects.
- Audit 2026-07-07: Telegram, DeepSeek, schedule add/remove, script write/remove/schedule, NeoPixels, RTTTL, DVI/LVGL demos, and FTP file transfer were not run in this pass because the user requested no changes/side effects; they still need dedicated live validation.
- Audit 2026-07-07: final recovery command returned the board to ROM BOOTSEL and `picotool info -a` confirmed boot type `bootsel` with reboot marker `0x46434c49`.
