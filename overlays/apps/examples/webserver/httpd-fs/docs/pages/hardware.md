# Hardware Notes

FruitClaw targets the Adafruit Fruit Jam RP2350 profile in this checkout.

## What Is On The Board

The important pieces for this build are:

| Part | Role in this system |
| --- | --- |
| RP2350 | Main CPU running NuttX and FruitClaw. |
| ESP32-C6 | ESP-Hosted Wi-Fi coprocessor. |
| microSD | Persistent FruitClaw data root and runtime files. |
| DVI output | Framebuffer display through RP2350 HSTX DVI. |
| NeoPixels | Onboard WS2812 LEDs exposed as `/dev/leds0`. |
| USB host | Keyboard, mouse, hub, and Xbox controller support. |
| PSRAM | Extra user heap and DVI scanout buffer support. |

## Board Bring-Up Docs

The board directory contains supporting notes:

- `PSRAM.md`: QMI CS1 PSRAM setup, recovery guard notes, UF2 artifacts, and
  RAM speed measurements.
- `CONSOLE.md`: USB CDC/NSH readline notes.
- `ESP_HOSTED.md`: ESP32-C6 ESP-Hosted Wi-Fi architecture, pin mapping, and
  validated networking/service status.
- `SDIO.md`: microSD SPI/MMC baseline and future native PIO SDIO notes.

## ESP-Hosted Wi-Fi

`ESP_HOSTED.md` describes this shape:

- RP2350 runs NuttX.
- ESP32-C6 runs ESP-Hosted-MCU slave/coprocessor firmware.
- NuttX owns IP addressing, DHCP, DNS, sockets, and services.
- User space sees normal NuttX networking through `wlan0`.

The board notes say the profile has been validated for scan, association,
DHCP/DNS, ICMP, TCP client connections, inbound telnet, HTTP, FTP, MQTT, NTP,
and disconnect/reconnect.

## RP2350 ESP-Hosted Pins

| Signal | RP2350 GPIO | NuttX symbol |
| --- | ---: | --- |
| SPI1 SCK | 30 | `BOARD_NINA_SCK_PIN` |
| SPI1 MOSI | 31 | `BOARD_NINA_MOSI_PIN` |
| SPI1 MISO | 28 | `BOARD_NINA_MISO_PIN` |
| SPI1 CS | 46 | `BOARD_NINA_CS_PIN` |
| ESP data-ready IRQ | 23 | `BOARD_NINA_IRQ_PIN` |
| ESP handshake/busy | 3 | `BOARD_NINA_READY_PIN` |
| ESP/peripheral reset | 22 | `BOARD_NINA_RESET_PIN` |

GPIO22 is shared with other Fruit Jam peripheral reset wiring, so reset
sequencing should stay conservative during bring-up.

## DVI And Framebuffer

The current `esp-hosted` profile enables:

```text
CONFIG_RP23XX_HSTX_DVI=y
CONFIG_RP23XX_HSTX_DVI_FB_RGB565_320X240=y
CONFIG_RP23XX_HSTX_DVI_OUTPUT_SCALING=1
CONFIG_RP23XX_HSTX_DVI_PIXEL_CLOCK=25200000
CONFIG_RP23XX_HSTX_DVI_SCANOUT_PSRAM=y
CONFIG_RP23XX_HSTX_DVI_SINGLE_SCANOUT_INTERNAL=y
CONFIG_VIDEO_FB=y
CONFIG_SYSTEM_DVICTRL=y
CONFIG_SYSTEM_DVICTRL_START_GUARD=y
```

Use:

```sh
dvictrl start
dvictrl info
```

The live framebuffer validated for this image is:

```text
/dev/fb0
320x240 RGB565
640-byte stride
1:1 DVI output centered inside 640x480 timing
```

FruitClaw uses split framebuffer storage for this mode: the application
framebuffer lives in PSRAM, and the live HSTX scanout frame lives in internal
SRAM. This was validated after a PSRAM-only scanout build produced clean
software counters but no monitor signal. The current profile keeps 1024-byte
command lines and reduces shell history depth to 8 to preserve enough internal
SRAM for the scanout frame without shrinking Wi-Fi/TCP buffers.

TRMNL-style 800x480 grayscale work is kept out of the FruitClaw operator
image. The separate TRMNL/HSTX profiles use a different compile-time
framebuffer profile:

```text
CONFIG_RP23XX_HSTX_DVI_FB_Y2_800X480=y
CONFIG_RP23XX_HSTX_DVI_PIXEL_CLOCK=25200000
CONFIG_RP23XX_HSTX_DVI_SCANOUT_PSRAM=y
CONFIG_RP23XX_HSTX_DVI_SINGLE_SCANOUT_INTERNAL=y
CONFIG_RP23XX_HSTX_DVI_Y2_OUTPUT_NATIVE_800X480=y
```

That mode exposes `/dev/fb0` as 800x480 `FB_FMT_Y2` with a 200-byte packed
2-bit grayscale stride. It is intended for future TRMNL-style work, not the
current FruitClaw RGB565 path. Internally the driver expands Y2 into compact
RGB scanout buffers because HSTX DVI still streams pixel words. Use the
documented tight 25.2 MHz timing for this path; the older 31.5 MHz 800x480
setting was a failed bring-up experiment.

## Conway's Game Of Life

Conway's Game of Life is enabled from NuttX `apps/games/cgol`:

```text
CONFIG_GAMES_CGOL=y
CONFIG_GAMES_CGOL_MAPWIDTH=320
CONFIG_GAMES_CGOL_MAPHEIGHT=240
CONFIG_GAMES_CGOL_FRAMEDELAY=50000
CONFIG_GAMES_CGOL_STACKSIZE=24576
```

Run it from NSH:

```sh
dvictrl start
cgol
```

Press Ctrl-C to stop it.

## LVGL And Berry LVGL

The profile compiles LVGL and Berry LVGL bindings:

```text
CONFIG_GRAPHICS_LVGL=y
CONFIG_INTERPRETERS_BERRY=y
CONFIG_INTERPRETERS_BERRY_STACKSIZE=32768
CONFIG_INTERPRETERS_BERRY_LVGL=y
CONFIG_INTERPRETERS_BERRY_LVGL_GLOBAL=y
CONFIG_INTERPRETERS_BERRY_LVGL_MOUSE=y
CONFIG_INTERPRETERS_BERRY_LVGL_MOUSE_DEVPATH="/dev/mouse0"
```

This means the libraries and bindings are present. FruitClaw's primary
operator surfaces are still Telegram, MCP, web docs, and shell; a finished
local GUI is not the current milestone.

## USB Host

The profile enables:

```text
CONFIG_USBHOST=y
CONFIG_USBHOST_HIDKBD=y
CONFIG_USBHOST_HIDMOUSE=y
CONFIG_USBHOST_XBOXCONTROLLER=y
CONFIG_EXAMPLES_XBC_TEST=y
CONFIG_EXAMPLES_TOUCHSCREEN_DEVPATH="/dev/mouse0"
```

Useful apps and nodes include `piousbhost`, `hidkbd`, `kbd`, and `xbc_test`.
The current build keeps the Xbox controller ABI at `/dev/xboxa`.

## PSRAM

The profile enables 8 MB PSRAM as user heap:

```text
CONFIG_RP23XX_PSRAM=y
CONFIG_RP23XX_PSRAM_HEAP_USER=y
CONFIG_RP23XX_PSRAM_SIZE=8388608
```

The board README includes PSRAM benchmark notes and warns not to use
`ramspeed -i` for current comparisons because disabling interrupts affects the
tick source used by `clock_gettime()`.

## NeoPixels

The profile enables WS2812 support and FruitClaw targets `/dev/leds0`:

```text
CONFIG_RP23XX_BOARD_HAS_WS2812=y
CONFIG_SYSTEM_NEOPIXELS=y
```

Try:

```sh
fruitclaw neopixels blue
fruitclaw neopixels off
```

Sources: `nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/README.md`,
`nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/ESP_HOSTED.md`,
`nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/configs/esp-hosted/defconfig`,
`apps/system/fruitclaw/README.md`.
