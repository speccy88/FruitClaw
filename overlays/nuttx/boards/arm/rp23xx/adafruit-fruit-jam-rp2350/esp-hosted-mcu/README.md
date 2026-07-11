# Fruit Jam ESP32-C6 ESP-Hosted-MCU Defaults

This directory contains Fruit Jam-specific configuration notes for the upstream
ESP-Hosted-MCU slave firmware. It is not a vendored copy of Espressif's
repository.

Use the defaults file with `esp-hosted-mcu/slave`:

```sh
git clone --recurse-submodules https://github.com/espressif/esp-hosted-mcu.git
cd esp-hosted-mcu/slave
cp /Users/fred/Documents/FruitClaw/nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/esp-hosted-mcu/sdkconfig.defaults.fruitjam-esp32c6 .
. /Users/fred/esp/v5.5.4/esp-idf/export.sh
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.fruitjam-esp32c6" set-target esp32c6
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.fruitjam-esp32c6" build
idf.py merge-bin
```

The initial transport is ESP-Hosted full-duplex SPI because the NuttX host
driver currently implements the standard 1600-byte full-duplex frame format.

## Verified Nets

These mappings were checked against the official Adafruit Fruit Jam schematic
from `adafruit/Adafruit-Fruit-Jam-PCB` commit
`252dc09456ee447b3d7eeb4fa99877898503e81a`, plus the CircuitPython Fruit Jam
board definition at commit `44171479b70f0cf71e09a68d89507bfb8657057f`.

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
high-impedance input during ESP32-C6 reset/boot and only let the ESP-Hosted
slave firmware drive it as data-ready after startup.

## Flashing The C6

The board can be flashed through Adafruit's RP2350 serial-passthrough UF2. The
validated local copy from the earlier Fruit Jam work is:

```text
/Users/fred/Documents/Code/PebbleRP2350/hardware_tests/fruitjam_esp32c6_hci/tools/SerialESPPassthrough.ino.uf2
```

Typical flow:

```sh
# 1. Put the RP2350 into BOOTSEL and copy SerialESPPassthrough.ino.uf2.

# 2. Find the passthrough CDC port.
ls /dev/cu.usbmodem*

# 3. Flash the ESP-Hosted-MCU merged image.
python -m esptool --chip esp32c6 --before no_reset --after no_reset \
  -p /dev/cu.usbmodem<PASSTHROUGH> -b 115200 \
  write_flash 0 build/merged-binary.bin

# 4. Reflash the RP2350 NuttX UF2.
```

Once the RP2350 NuttX side also has the ESP-Hosted profile flashed, the first
runtime proof should be `ESP_PRIV_EVENT_INIT`, followed by the firmware-version
and station-MAC RPC responses in the NuttX log.
