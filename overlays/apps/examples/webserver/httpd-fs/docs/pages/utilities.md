# Installed Utilities

This page lists the user-facing utilities compiled into the Fruit Jam
`esp-hosted` guarded alpha release. The live board is still the source of truth:
run `help` from NSH and `fruitclaw tools` for the current command and MCP tool
surface.

## FruitClaw

The main operator command is:

```sh
fruitclaw
```

Useful subcommands:

| Command | Use |
| --- | --- |
| `fruitclaw status` | Non-secret runtime health, queues, guard state, MCP, Telegram, scheduler, Berry, services, and data root. |
| `fruitclaw config` | Show non-secret configuration state. |
| `fruitclaw config set-wifi` | Store Wi-Fi SSID/password in the active data root. |
| `fruitclaw config set-secret telegram` | Store the Telegram bot token without printing it. |
| `fruitclaw config set-secret deepseek` | Store the DeepSeek API key without printing it. |
| `fruitclaw tools` | List registered capability tools. |
| `fruitclaw selftest` | Run offline sanity tests for JSON, tools, queues, cron, memory, paths, scheduled direct tools, boot schedules, and mapping logic. |
| `fruitclaw telegram-test` | Send a test Telegram message to the allowed chat. |
| `fruitclaw deepseek-test` | Test outbound DeepSeek chat/completions. |
| `fruitclaw terminal-run <cmd...>` | Run a bounded NSH command and capture output. |
| `fruitclaw service status` | Show Telnet and FTP service state. |
| `fruitclaw service <start|stop|restart|enable|disable> <service>` | Manage Telnet/FTP where the underlying daemon supports the action. |
| `fruitclaw berry-run <path> [json-args]` | Run a Berry script below `scripts/`. |
| `fruitclaw berry-smoke` | Verify the real Berry runner and `claw` binding. |
| `fruitclaw schedule ...` | Add/list/remove boot, interval, once, after, and cron schedules. |
| `fruitclaw neopixels ...` | Drive `/dev/leds0`. |
| `fruitclaw device ...` | List/read/write bounded `/dev` paths. |
| `fruitclaw mcp status` | Show MCP endpoint counters. |

## Network Utilities

Compiled network/user-space utilities include:

| Utility | Purpose |
| --- | --- |
| `ifconfig` | Inspect interfaces such as `wlan0`. |
| `wapi` | Wireless association/configuration command used by FruitClaw Wi-Fi setup. |
| `renew` | DHCP renew command for `wlan0`. |
| `ping` | ICMP connectivity test. |
| `wget` | Simple HTTP client example. |
| `ntpc` | NTP client. |
| `telnetd` | NuttX Telnet NSH service on port 23. |
| `ftpd_start` / `ftpd_stop` | FTP daemon lifecycle commands. |
| `webserver` | uIP webserver serving `/`, `/docs/index.html`, `/site/home.md`, and `/mcp`; `/doc/` redirects to the docs shell. |
| `mqttc` | MQTT-C example support. |
| `esphostedctl` | ESP-Hosted control/diagnostic utility. |

FruitClaw autostarts the webserver, NTP, Telnet, and FTP when the profile and
runtime service settings allow it.

## Display, Input, And Hardware Utilities

| Utility | Purpose |
| --- | --- |
| `dvictrl` | Start/inspect the RP2350 HSTX DVI framebuffer. |
| `cgol` | Conway's Game of Life on `/dev/fb0`. |
| `neopixels` | Direct NeoPixel command for `/dev/leds0`. |
| `rtttl` | Play RTTTL tunes through the audio path. |
| `piousbhost` | USB-host bring-up and diagnostics. |
| `xbc_test` | Xbox controller test app. |
| `hidkbd`, `keyboard`, `touchscreen` examples | USB HID/input examples compiled in this profile. |
| `fb` | Framebuffer example support. |
| `buttons` and `buttonctl` | Button input tools. |
| `i2c` | I2C bus tool. |
| `spi` | SPI bus tool. |
| `irdump` | IR dump utility. |
| `nxplayer` | NuttX audio player utility. |

## Shell And File Utilities

| Utility | Purpose |
| --- | --- |
| `nsh` | NuttX shell over USB CDC and Telnet. |
| `vi` | Text editor configured for an 80x24 terminal. |
| `mount`, `umount`, `ls`, `cat`, `cp`, `rm`, `mkdir` | Standard NSH file and filesystem operations where enabled by NuttX. |
| `ps`, `free`, `uname`, `help` | Runtime inspection commands. |
| `bootsel` | Manual reboot to ROM BOOTSEL for flashing. The current alpha also uses BOOTSEL automatically for guarded recovery and the 10 minute max-uptime fuse. |
| `bootguard` | Board recovery scratch/control helper. |

Sources: `nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/configs/esp-hosted/defconfig`,
`apps/system/fruitclaw/fruitclaw_main.c`,
`apps/system/fruitclaw/caps/fc_caps_builtin.c`,
`apps/system/fruitclaw/README.md`.
