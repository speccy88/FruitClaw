# FruitClaw

Adafruit Fruit Jam RP2350 NuttX bring-up workspace.

This repository is the project wrapper for the two NuttX source repositories
used by the bring-up:

- `nuttx`: board port and RP23xx support changes.
- `apps`: Berry plus NSH helper applications.

The source repositories are tracked here as submodules so their upstream NuttX
history remains intact.

## Current ESP-Hosted Release

The current ESP-Hosted `wlan0` release is:

| File | Purpose | SHA-256 |
| --- | --- | --- |
| `artifacts/fruitclaw-esp32c6-esp-hosted-mcu-20260629.bin` | ESP32-C6 ESP-Hosted-MCU merged flash image. This replaces the stock NINA/AirLift firmware on the Fruit Jam coprocessor. | `1a1b35659dd62f44fa8c91b3e03f3fec80886072d04aeb6f192823eb28921c08` |
| `artifacts/fruitclaw-esp-hosted-wlan0-20260629.uf2` | RP2350 NuttX host image with the Fruit Jam ESP-Hosted `wlan0` profile. | `c2ccb00bed4b264fd60c389ee6c3125827ea4ef4188a120b233a195a7d8ce615` |

The ESP32-C6 image was built from `espressif/esp-hosted-mcu` commit
`8f0770d39065c2a9ff6828268709c3502e0d5349` plus the Fruit Jam overlay at
`nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/esp-hosted-mcu/`.
The RP2350 UF2 was built from the checked-in `nuttx` submodule commit
`ea72397d346e04a7e8ac015df026c02192accb54` and `apps` submodule commit
`6a225eded40334823f39e23e4dbb01d2b3094142`.

## Why Replace NINA/AirLift

The stock Fruit Jam ESP32-C6 firmware follows the NINA/AirLift model: the
RP2350 sends high-level socket commands to the coprocessor and the coprocessor
owns most of the Wi-Fi/IP behavior. That is useful for CircuitPython-style
network offload, but it is the wrong shape for operating systems such as NuttX
or Linux when they should own DHCP, DNS, routing, sockets, services, packet
timing, and diagnostics themselves.

This ESP-Hosted path makes the ESP32-C6 a radio coprocessor instead. The
ESP32-C6 handles Wi-Fi radio work and exchanges ESP-Hosted control/data frames
over the existing Fruit Jam SPI wiring. NuttX owns the network stack and sees a
normal `wlan0` interface, so normal tools such as `ifconfig`, `wapi`, `renew`,
`ping`, `wget`, `telnetd`, `webserver`, `ftpd_start`, MQTT-C, and NTP can run
against a real NuttX network device.

## Flash ESP-Hosted

You need to flash both chips:

- Flash `fruitclaw-esp32c6-esp-hosted-mcu-20260629.bin` to the ESP32-C6.
- Flash `fruitclaw-esp-hosted-wlan0-20260629.uf2` to the RP2350.

The ESP32-C6 `.bin` is not a UF2. Use Adafruit's Fruit Jam serial ESP32-C6
passthrough UF2 as a temporary programmer bridge. The bridge UF2 is not the
ESP-Hosted firmware; it only turns the RP2350 USB port into an ESP32-C6 serial
flashing path.

1. Download the ESP32-C6 `.bin` and RP2350 `.uf2` from this repository or the
   matching GitHub release.
2. Download Adafruit's Fruit Jam serial ESP32-C6 passthrough UF2 from the
   official guide:
   `https://learn.adafruit.com/adafruit-fruit-jam/upgrading-airlift-firmware`
3. Put the Fruit Jam RP2350 into BOOTSEL and copy Adafruit's
   `SerialESPPassthrough.ino.uf2` to the mounted `RP2350` drive.
4. After the board reboots, find the passthrough serial port:

```sh
ls /dev/cu.usbmodem*
```

5. Flash the ESP32-C6:

```sh
python3 -m esptool --chip esp32c6 --before no_reset --after no_reset \
  -p /dev/cu.usbmodem<PASSTHROUGH> -b 115200 \
  write_flash 0 fruitclaw-esp32c6-esp-hosted-mcu-20260629.bin
```

6. Put the RP2350 back into BOOTSEL and copy
   `fruitclaw-esp-hosted-wlan0-20260629.uf2` to the mounted `RP2350` drive.
7. Open the NuttX USB console and check `wlan0`:

```sh
ifup wlan0
wapi psk wlan0 <passphrase> 3 2
wapi essid wlan0 <ssid> 1
renew wlan0
ifconfig wlan0
wapi scan wlan0
ping -c 3 -I wlan0 <gateway-ip>
esphostedctl
```

## Revert To Adafruit NINA/AirLift

Reverting means replacing the ESP32-C6 firmware with Adafruit's Fruit Jam NINA
firmware and then flashing whichever RP2350 UF2 you want to run afterward.

1. Follow Adafruit's official Fruit Jam AirLift firmware guide:
   `https://learn.adafruit.com/adafruit-fruit-jam/upgrading-airlift-firmware`
2. Download the latest NINA firmware from:
   `https://github.com/adafruit/nina-fw/releases/latest`
3. Choose the Fruit Jam ESP32-C6 file. Adafruit names it like
   `NINA_ADAFRUIT-fruitjam_c6-<version>.bin`.
4. Use Adafruit's updater flow from the guide, or use the same passthrough
   esptool method:

```sh
python3 -m esptool --chip esp32c6 --before no_reset --after no_reset \
  -p /dev/cu.usbmodem<PASSTHROUGH> -b 115200 \
  write_flash 0 NINA_ADAFRUIT-fruitjam_c6-<version>.bin
```

5. Put the RP2350 back into BOOTSEL and flash the desired Adafruit/CircuitPython
   or Fruit Jam OS UF2. After this, the ESP32-C6 is back on the stock
   NINA/AirLift socket-command path instead of ESP-Hosted raw-frame transport.

## Current Snapshot

The current hardware-tested image is:

```text
artifacts/fruitjam-usbnsh-berry-ws2812-userled-buttons-examples-i2c-spi-sd-nina-20260629.uf2
```

Verified on hardware:

- USB CDC NSH on `/dev/ttyACM0` from the board side.
- Berry built into the image.
- GPIO29 active-low user LED driver and helper scripts.
- Buttons on GPIO0, GPIO4, and GPIO5.
- Five WS2812 NeoPixels on GPIO32 with example scripts under `/examples`.
- STEMMA QT I2C0 on GPIO20/GPIO21.
- SPI0 microSD card support with FAT mount at `/mnt/sd0`.
- SPI1 low-level NINA/AirLift wiring enabled for future networking work.

## Clone

```sh
git clone --recurse-submodules https://github.com/speccy88/FruitClaw.git
```

If already cloned without submodules:

```sh
git submodule update --init --recursive
```

## Build

Baseline USB NSH build from the `nuttx` submodule:

```sh
./tools/configure.sh -E -m -a ../apps adafruit-fruit-jam-rp2350:usbnsh
make -j8
```

The generated UF2 is `nuttx/nuttx.uf2`.

ESP-Hosted `wlan0` build:

```sh
export PATH="$HOME/.local/nuttx-tools/kconfig-frontends/bin:$PATH"
cd nuttx
./tools/configure.sh -E -m -a ../apps adafruit-fruit-jam-rp2350:esp-hosted
make -j8
```

ESP32-C6 ESP-Hosted-MCU build:

```sh
git clone --recurse-submodules https://github.com/espressif/esp-hosted-mcu.git
cd esp-hosted-mcu
git checkout 8f0770d39065c2a9ff6828268709c3502e0d5349
git submodule update --init --recursive
cd slave
cp /path/to/FruitClaw/nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/esp-hosted-mcu/sdkconfig.defaults.fruitjam-esp32c6 .
. /Users/fred/esp/v5.5.4/esp-idf/export.sh
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.fruitjam-esp32c6" set-target esp32c6
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.fruitjam-esp32c6" build
idf.py merge-bin
```

The generated ESP32-C6 image is `build/merged-binary.bin`.
