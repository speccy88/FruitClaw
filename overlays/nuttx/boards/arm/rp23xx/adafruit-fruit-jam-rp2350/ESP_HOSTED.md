# Fruit Jam ESP-Hosted Wi-Fi Bring-Up

This note tracks the raw-frame ESP-Hosted path for the Adafruit Fruit Jam
RP2350 board. The goal is a real NuttX `wlan0` interface backed by the onboard
ESP32-C6 coprocessor. This is not the Adafruit NINA/AirLift socket-command
firmware path.

## Target Shape

- RP2350 runs NuttX.
- ESP32-C6 runs ESP-Hosted-MCU slave/coproc firmware.
- NuttX owns IP addressing, DHCP, DNS, sockets, and services.
- ESP32-C6 handles Wi-Fi radio work and exchanges control/data frames with the
  RP2350 host.
- User-space sees normal NuttX networking: `ifconfig wlan0`, WAPI/wireless
  ioctls, DHCP, DNS, ping, TCP/UDP sockets, telnet, HTTP, FTP, MQTT, and NTP.

## Current Code State

The current tree contains an ESP-Hosted SPI scaffold:

- `include/nuttx/wireless/esp_hosted.h`
- `drivers/wireless/esp_hosted.c`
- `drivers/wireless/Kconfig`
- `drivers/wireless/Make.defs`
- `drivers/wireless/CMakeLists.txt`
- `boards/arm/rp23xx/adafruit-fruit-jam-rp2350/Kconfig`
- `boards/arm/rp23xx/adafruit-fruit-jam-rp2350/src/rp23xx_bringup.c`

The board option is `CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED`. It depends
on `CONFIG_ESP_HOSTED_SPI` and `CONFIG_RP23XX_SPI1`, and selects the board's
onboard ESP32-C6 pin setup.

The scaffold validates the configured SPI/GPIO callbacks, attaches the
data-ready IRQ when the board provides one, resets the coprocessor, configures
SPI, and uses 1600-byte ESP-Hosted full-duplex frames. The host now sends a
valid dummy header (`ESP_MAX_IF`) instead of an all-zero buffer, schedules
data-ready service on `HPWORK`, validates incoming hosted headers and
checksums, counts private/control/station/AP frames, and recognizes
`ESP_PRIV_EVENT_INIT`.

After `ESP_PRIV_EVENT_INIT`, the driver now sends two minimal ESP-Hosted RPC
startup probes over the serial/control interface:

- `Req_GetCoprocessorFwVersion` (`350`) with an empty request payload.
- `Req_GetMACAddress` (`257`) with station Wi-Fi interface mode `0`.

The parser recognizes `Resp_GetCoprocessorFwVersion` (`606`) and
`Resp_GetMACAddress` (`513`), records RPC counters, and logs the firmware
version / IDF target / chip ID and station MAC when the coprocessor returns
valid responses.

The driver now contains the first real NuttX lower-half plumbing for the
station data path. It allocates a `netdev_lowerhalf_s`, registers it as
`NET_LL_IEEE80211` only after both identity RPCs succeed, queues incoming
`ESP_STA_IF` frames into NuttX `netpkt` RX buffers, and wraps NuttX TX packets
as `ESP_STA_IF` frames for the full-duplex SPI transport.

The station control path is now partially mapped to ESP-Hosted RPCs. After the
version/MAC identity probe succeeds, the driver sends:

- `Req_WifiInit` (`278`) with ESP32-C6/ESP-IDF 5.5-style init defaults.
- `Req_SetWifiMode` (`260`) for station mode.
- `Req_WifiStart` (`280`).

WAPI/wireless ioctl state for ESSID, passphrase, and WPA/cipher preferences is
kept in driver RAM. A connect request sends `Req_WifiSetConfig` (`284`) with
the configured station SSID/passphrase and then `Req_WifiConnect` (`282`).
Disconnect sends `Req_WifiDisconnect` (`283`). Scan start sends
`Req_WifiScanStart` (`286`). `Event_StaScanDone` (`774`) is decoded, and the
first `SIOCGIWSCAN` after completion fetches up to 12 AP records with
`Req_WifiScanGetApRecords` (`289`) and formats them as NuttX `iw_event`
records for WAPI. Carrier is turned on/off only from real
`Event_StaConnected` (`775`) and `Event_StaDisconnected` (`776`)
notifications from the coprocessor.

As of the 2026-06-29 hardware run, this path is no longer just a scaffold:
Fruit Jam boots the ESP-Hosted profile, registers `wlan0`, scans, associates,
gets DHCP/DNS, passes ICMP, makes TCP client connections, accepts inbound
telnet/HTTP/FTP, publishes MQTT, syncs NTP, and recovers after an explicit
WAPI disconnect/reconnect cycle.

## RP2350-Side Pins

These pins are the existing Fruit Jam NINA/AirLift-style wiring on the RP2350
side. They are used only as board wiring for the ESP-Hosted path.

| Signal | RP2350 GPIO | NuttX symbol |
| --- | ---: | --- |
| SPI1 SCK | 30 | `BOARD_NINA_SCK_PIN` |
| SPI1 MOSI | 31 | `BOARD_NINA_MOSI_PIN` |
| SPI1 MISO | 28 | `BOARD_NINA_MISO_PIN` |
| SPI1 CS | 46 | `BOARD_NINA_CS_PIN` |
| ESP data-ready IRQ | 23 | `BOARD_NINA_IRQ_PIN` |
| ESP handshake/busy | 3 | `BOARD_NINA_READY_PIN` |
| ESP/peripheral reset | 22 | `BOARD_NINA_RESET_PIN` |

GPIO22 is also used by other Fruit Jam peripheral reset wiring. Keep reset
sequencing conservative while bring-up is in progress.

## ESP32-C6-Side Pins

The ESP32-C6-side pins have now been checked against the official Adafruit
Fruit Jam Eagle schematic from `adafruit/Adafruit-Fruit-Jam-PCB` commit
`252dc09456ee447b3d7eeb4fa99877898503e81a`. The RP2350-side names were also
cross-checked against the CircuitPython Fruit Jam board definition at commit
`44171479b70f0cf71e09a68d89507bfb8657057f`.

| ESP-Hosted signal | RP2350 side | ESP32-C6 side | Schematic net |
| --- | --- | --- | --- |
| SCK | GPIO30 | IO22 | `SCK` |
| MOSI, host to C6 | GPIO31 | IO21 | `MOSI` |
| MISO, C6 to host | GPIO28 | IO6 through `ESP_MISO` buffer | `MISO` |
| CS | GPIO46 | IO7 | `ESP_CS` |
| Handshake/busy | GPIO3 | IO18 | `ESP_BUSY` |
| Data-ready IRQ | GPIO23 | IO9 / BOOT9 | `I2S_ESP_IRQ` |
| Reset | GPIO22 | EN | `PERIPH_RST` |

`I2S_ESP_IRQ` is also the ESP32-C6 BOOT9 strap. Keep the RP2350 side as a
high-impedance input through ESP32-C6 reset/boot; the ESP-Hosted slave firmware
can drive it as data-ready only after startup.

The matching ESP-Hosted-MCU slave Kconfig overlay is:

- `esp-hosted-mcu/sdkconfig.defaults.fruitjam-esp32c6`

## ESP32-C6 Firmware Notes

Preferred upstream starting point:

- `https://github.com/espressif/esp-hosted-mcu`
- `https://components.espressif.com/components/espressif/esp_hosted`

Initial firmware flow:

```sh
git clone --recurse-submodules https://github.com/espressif/esp-hosted-mcu.git
cd esp-hosted-mcu/slave
cp /Users/fred/Documents/FruitClaw/nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/esp-hosted-mcu/sdkconfig.defaults.fruitjam-esp32c6 .
. /Users/fred/esp/v5.5.4/esp-idf/export.sh
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.fruitjam-esp32c6" set-target esp32c6
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.fruitjam-esp32c6" build
idf.py merge-bin
```

The overlay chooses SPI full-duplex transport, mode 3, and the Fruit Jam
ESP32-C6 GPIOs listed above. Start the RP2350 host at a low SPI clock such as
5 MHz and raise it only after error counters and signal behavior are understood.

Flash through the RP2350 serial-passthrough UF2:

```sh
python -m esptool --chip esp32c6 --before no_reset --after no_reset \
  -p /dev/cu.usbmodem<PASSTHROUGH> -b 115200 \
  write_flash 0 build/merged-binary.bin
```

The known local passthrough UF2 is:

```text
/Users/fred/Documents/Code/PebbleRP2350/hardware_tests/fruitjam_esp32c6_hci/tools/SerialESPPassthrough.ino.uf2
```

After flashing the ESP32-C6, reflash the RP2350 NuttX UF2 before testing
`wlan0`.

## NuttX Configuration Notes

Keep the existing `adafruit-fruit-jam-rp2350:usbnsh` profile stable while the
driver is still being validated. The named
`adafruit-fruit-jam-rp2350:esp-hosted` profile carries the current experimental
network configuration. If rebuilding it manually, keep at least:

```text
CONFIG_NET=y
CONFIG_NET_ETHERNET=y
CONFIG_NET_IPv4=y
CONFIG_NET_TCP=y
CONFIG_NET_UDP=y
CONFIG_NET_ICMP=y
CONFIG_NET_ICMP_SOCKET=y
CONFIG_NET_ARP=y
CONFIG_ARP_SEND_MAXTRIES=10
CONFIG_ARP_SEND_DELAYMSEC=100
CONFIG_NETDEVICES=y
CONFIG_NETDEV_LATEINIT=y
CONFIG_DRIVERS_WIRELESS=y
CONFIG_ESP_HOSTED=y
CONFIG_ESP_HOSTED_SPI=y
CONFIG_ESP_HOSTED_WLAN=y
CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED=y
CONFIG_NETDEV_WIRELESS_IOCTL=y
CONFIG_NETDEV_WIRELESS_HANDLER=y
CONFIG_DRIVERS_IEEE80211=y
CONFIG_NETUTILS_DHCPC=y
CONFIG_NETDB_DNSCLIENT=y
CONFIG_SYSTEM_PING=y
CONFIG_NETUTILS_WEBCLIENT=y
CONFIG_EXAMPLES_WGET=y
CONFIG_NETUTILS_WEBSERVER=y
CONFIG_NETUTILS_HTTPD_SINGLECONNECT=y
CONFIG_EXAMPLES_WEBSERVER=y
CONFIG_NETUTILS_FTPD=y
CONFIG_EXAMPLES_FTPD=y
CONFIG_NETUTILS_MQTTC=y
CONFIG_EXAMPLES_MQTTC=y
CONFIG_NETUTILS_NTPCLIENT=y
CONFIG_SYSTEM_NTPC=y
CONFIG_SYSTEM_VI=y
```

The profile also includes WAPI tooling, DHCP, DNS, `ping`, `wget`, `telnetd`,
`webserver`, `ftpd_start`/`ftpd_stop`, `mqttc_pub`, `ntpcstart`/`ntpcstatus`/
`ntpcstop`, and `vi` so link and service tests can run from NSH.

The ARP retry window is intentionally longer than the NuttX default. With the
default 5 tries at 20 ms, a first same-LAN TCP connection could fail with
`-ENETUNREACH` before the Wi-Fi ARP reply arrived. The current profile uses 10
tries at 100 ms and cold local `wget` succeeds without a manual warm-up ping.

## Build Evidence

Built locally on 2026-06-29:

| Profile | Result | FLASH | RAM | Notes |
| --- | --- | ---: | ---: | --- |
| `adafruit-fruit-jam-rp2350:usbnsh` | Pass | 406576 B | 51060 B | Baseline image after returning from hosted config |
| `adafruit-fruit-jam-rp2350:esp-hosted` | Pass | 530896 B | 72208 B | Named defconfig with `CONFIG_ESP_HOSTED_WLAN=y`, WAPI, DHCP, DNS, ICMP sockets, ARP retry tuning, `wget`, `telnetd`, `webserver`, `ftpd_start`, `mqttc_pub`, `ntpc*`, and `vi` compiled |
| ESP32-C6 ESP-Hosted-MCU slave | Pass | 1026464 B | n/a | Upstream `esp-hosted-mcu` commit `8f0770d39065c2a9ff6828268709c3502e0d5349`, ESP-IDF 5.5.4, Fruit Jam defaults overlay, `merged-binary.bin` SHA256 `a7e78def271dc2a21c6983ce2dd5883b7e95a7c6576fae263edda82c5dceec6c` |

## Validation Milestones

| ID | Milestone | Current state | 2026-06-29 evidence |
| --- | --- | --- | --- |
| A | ESP32-C6 reset works from RP2350 | Pass | Fresh RP2350 flash/reboot reset the C6 path; `esphostedctl` reported `reset=1`, `init_event=1`, and successful identity RPCs |
| B | SPI exchanges valid ESP-Hosted control frames | Pass | `esphostedctl` reported control frames and RPC responses with `control_timeout=0`, `malformed_frame=0`, `checksum_error=0`, and `tlv_error=0` |
| C | NuttX gets coprocessor version/MAC | Pass | `esphostedctl` reported `fwversion=1`, `mac=1`; `ifconfig wlan0` showed `HWaddr 58:e6:c5:f5:a3:58` |
| D | Scan returns AP records | Pass | `wapi scan wlan0` returned 12 AP records, including the target SSID |
| E | Connect and carrier event | Pass | `wapi essid wlan0 <ssid> 1` associated; `ifconfig wlan0` showed `RUNNING`; `esphostedctl` reported `link_up=2` after reconnect test |
| F | `wlan0` appears | Pass | `ifconfig wlan0` showed a registered `NET_LL_IEEE80211` netdev after version/MAC RPCs |
| G | DHCP through NuttX | Pass | `renew wlan0` assigned `192.168.1.7`, default router `192.168.1.1`, mask `255.255.255.0` |
| H | Ping works | Pass | `ping -c 2 -I wlan0 192.168.1.1` and `ping -c 1 -I wlan0 example.com` both returned 0% packet loss |
| I | TCP client works | Pass | Cold `wget http://192.168.1.234:18080/` returned the local test page; `wget http://example.com/` returned the Example Domain page |
| J | Inbound service works | Pass | `telnetd` accepted a host connection to `192.168.1.7:23` and returned the `NuttShell (NSH)` banner and prompt |
| K | Reconnect/reset recovery works | Pass | `wapi disconnect wlan0` dropped carrier; re-applying PSK/ESSID plus `renew wlan0` restored `RUNNING` and gateway ping; `esphostedctl` reported `disconnect=1`, `link_down=1`, `connect=2`, `link_up=2` |

The same flashed image also passed a USB CDC / NSH editing smoke test:
up-arrow history recall re-ran the previous command, and left-arrow insertion
produced `echo abcXYZdef` without hanging the shell.

## Network App Validation

Validated on the 2026-06-29 flashed ESP-Hosted image:

| App/service | Result | Evidence |
| --- | --- | --- |
| Webclient / `wget` | Pass | `wget http://example.com/` returned the Example Domain HTML through `wlan0` |
| Webserver | Pass | `webserver &` started the embedded sample site; a Mac host HTTP GET to `http://192.168.1.7/` returned HTTP 200 and the sample page |
| FTPD | Pass | `ftpd_start -4` started `NuttX FTP Server`; a Mac host logged in as `root`, listed `/`, uploaded `/tmp/fruitclaw-ftp.txt`, and read it back |
| MQTT-C example | Pass | `mqttc_pub -h 192.168.1.234 -p 1883 -t fruitclaw/test -m fruitclaw -n 1` connected to a local test broker and published the message |
| NTP client | Pass | `ntpcstart` launched the daemon; after the retry/sample window, `ntpcstatus` reported 5 samples from pool servers; `ntpcstop` stopped it |
| vi | Pass | `vi -h` printed usage with the configured 80x24 defaults |

## Bring-Up Commands

Baseline known-good build:

```sh
cd /Users/fred/Documents/FruitClaw/nuttx
./tools/configure.sh -E -m -a ../apps adafruit-fruit-jam-rp2350:usbnsh
make -j8
```

Experimental ESP-Hosted build:

```sh
cd /Users/fred/Documents/FruitClaw/nuttx
./tools/configure.sh -E -m -a ../apps adafruit-fruit-jam-rp2350:esp-hosted
make -j8
```

Runtime validation commands:

```sh
ifup wlan0
wapi psk wlan0 <passphrase> 3 2
wapi essid wlan0 <ssid> 1
renew wlan0
ifconfig wlan0
wapi scan wlan0
ping -c 3 -I wlan0 <gateway-ip>
ping -c 1 -I wlan0 example.com
wget http://<same-lan-test-host>:18080/
wget http://example.com/
telnetd
webserver &
ftpd_start -4
mqttc_pub -h <mqtt-broker-ip> -p 1883 -t fruitclaw/test -m fruitclaw -n 1
ntpcstart
ntpcstatus
ntpcstop
vi -h
wapi disconnect wlan0
wapi psk wlan0 <passphrase> 3 2
wapi essid wlan0 <ssid> 1
renew wlan0
esphostedctl
```

## Near-Term Driver Tasks

1. Expand AP record fetching beyond the current conservative 12-record
   SPI-frame batch.
2. Add RSSI/link-state/MAC query support where NuttX wireless tooling expects
   it.
3. Add UDP, HTTP-server, and FTP service tests through `wlan0`.
4. Stress longer reboot/AP-loss/reconnect runs and raise SPI clock only after
   the error counters stay clean.
