# Fruit Jam SD Transports

Fruit Jam can use its onboard microSD socket through either the established
SPI/MMC driver or the RP2350 PIO-backed four-bit SD driver.  The selection is
made at compile time; both paths retain `/dev/mmcsd0`, partition devices such
as `/dev/mmcsd0p0`, VFAT mounting at `/mnt/sd0`, and the `sdmount` command.

## Wiring

| SD signal | GPIO | SPI signal |
| --- | ---: | --- |
| Card detect | 33 | Card detect |
| CLK | 34 | SCK |
| CMD | 35 | MOSI |
| DAT0 | 36 | MISO |
| DAT1 | 37 | — |
| DAT2 | 38 | — |
| DAT3 | 39 | CS |

The Adafruit Learn pinout page currently labels card detect as GPIO34, but the
board schematic and CircuitPython board definition confirm GPIO33.  The detect
contact is active-high after enabling the RP2350 internal pull-up.

## Compile-time selection

The board Kconfig menu offers `Fruit Jam SD transport`:

- `CONFIG_ADAFRUIT_FRUIT_JAM_SD_SPI=y` selects the existing
  `CONFIG_RP23XX_SPISD` compatibility path.
- `CONFIG_ADAFRUIT_FRUIT_JAM_SD_PIO=y` selects
  `CONFIG_RP23XX_PIO_SDIO`, NuttX `MMCSD_SDIO`, and DMA support.

Only one transport may be selected in an image.  Release profiles initially
select SPI.  Development builds can override the profile without editing its
defconfig:

```sh
./scripts/build-profile.sh fruit-jam-full --dev-sd-mode spi
./scripts/build-profile.sh fruit-jam-full --dev-sd-mode pio
```

The resulting UF2 and build-record names include `-sd-spi` or `-sd-pio`.

## PIO host design

The native path implements NuttX `struct sdio_dev_s` with a four-bit-only
host.  It uses:

- PIO2 with GPIOBASE 16.
- Two state machines and 27 instructions.
- Two dynamically allocated DMA channels.
- 400 kHz identification and at most 25 MHz default-speed transfers.
- CRC7 command checking and four-lane CRC16 data checking.
- Single- and multiblock reads/writes plus bounded cancellation and timeout
  handling.

GPIO34 through GPIO39 are PIO-relative pins 18 through 23.  The driver loads
its program before WS2812 registration; together they consume 31 of PIO2's 32
instructions and three of its four state machines.  The driver never clears
shared PIO instruction memory.

The initial implementation intentionally does not support one-bit PIO, MMC or
eMMC devices, arbitrary SDIO I/O functions, 50 MHz CMD6 High-Speed mode, or
1.8 V/UHS signaling.

## Card handling and switching modes

GPIO33 supplies real presence status in both modes.  PIO mode uses a debounced
both-edge interrupt and NuttX media callbacks.  Neither mode automatically
unmounts a live filesystem; run `umount /mnt/sd0` before removing a card.

The socket has no software-controlled power switch.  A card can remain latched
in SPI mode until power is removed, so changing between SPI and PIO firmware
requires a cold power cycle rather than only a soft reset.

## Validation

Use the same card for `sd_bench`, `sd_stress`, and `fstest` comparisons.  The
PIO promotion gate is at least twice the sequential throughput of a validated
20 MHz multiblock SPI baseline, with no CRC errors, timeouts, or hash mismatch
while DVI, USB host, I2S, networking, and WS2812 are active.

References:

- https://learn.adafruit.com/adafruit-fruit-jam/pinout
- https://learn.adafruit.com/adafruit-fruit-jam/sdio-usage
- https://github.com/adafruit/Adafruit-Fruit-Jam-PCB
- https://github.com/adafruit/SdFat/tree/master/src/SdCard/PioSdio
- https://nuttx.apache.org/docs/latest/components/drivers/special/sdio.html
