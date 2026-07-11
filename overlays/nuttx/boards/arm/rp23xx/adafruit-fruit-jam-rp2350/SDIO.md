# Fruit Jam SD Card Interface Notes

This note records the SD card decision made during the initial Adafruit Fruit
Jam RP2350 NuttX bring-up.

## Current NuttX Support

The board port currently uses NuttX SPI/MMC for the microSD socket. This is SD
card access through SPI mode, not SDIO mode.

Current `usbnsh` status, verified on hardware:

- `/dev/mmcsd0` is created.
- `/mnt/sd0` is mounted as `vfat`.
- `spi bus` reports SPI bus 0 and SPI bus 1 present.
- `i2c dev -b 0 -z 18 18` finds the onboard I2C codec at `0x18`.

The SPI/MMC path is the stable baseline because it uses existing NuttX RP23xx
SPI support plus the common NuttX MMC/SD SPI upper-half.

## Fruit Jam SD Pins

Fruit Jam wires the microSD socket so it can be used either in SPI mode or in
SDIO-style mode.

Current SPI/MMC mapping:

| Signal | GPIO | Notes |
| ------ | ---- | ----- |
| SCK | 34 | SPI0 clock |
| MOSI | 35 | SPI0 TX |
| MISO / DAT0 | 36 | SPI0 RX, also SDIO DAT0 |
| CS / DAT3 | 39 | SPI software chip select, also SDIO DAT3 |
| Card detect | 33 | GPIO input |

Additional SDIO data pins available on the socket:

| Signal | GPIO |
| ------ | ---- |
| DAT1 | 37 |
| DAT2 | 38 |

## SPI/MMC vs Native SDIO

NuttX SPI/MMC means the SD card is accessed in SPI mode over `SCK`, `MOSI`,
`MISO`, and `CS`. NuttX still exposes the card as an MMC/SD block device, so
NSH and filesystems see `/dev/mmcsd0` and `/mnt/sd0` normally.

Native SDIO would use SD command/data signaling instead: `CLK`, `CMD`, and
`DAT0..DAT3`. On Fruit Jam/RP2350 this would not be a simple dedicated SDIO
peripheral enable. It would be an SDIO protocol implementation using RP2350
PIO, then connected to NuttX through an SDIO host lower-half.

## Can Fruit Jam Do Native SDIO?

Yes, the Fruit Jam hardware is suitable for it:

- The microSD socket exposes the extra SDIO data pins.
- `DAT0..DAT3` are on GPIO36, GPIO37, GPIO38, and GPIO39, which is convenient
  for a PIO program.
- The current NuttX RP23xx Fruit Jam port does not yet include a PIO SDIO host
  driver.

So native SDIO is possible, but it is driver work rather than a defconfig
switch.

## Future Work

A future implementation should probably be added behind a separate config, for
example:

```text
CONFIG_RP23XX_PIO_SDIO=y
```

Expected work items:

- Add an RP23xx/RP2350 PIO SDIO host lower-half.
- Plug that lower-half into NuttX `mmcsd_sdio`.
- Implement SD command/response handling.
- Support at least 1-bit mode first, then 4-bit mode.
- Add CRC, timeout, and card error handling.
- Decide whether to use DMA or tightly serviced PIO FIFOs for data movement.
- Keep the existing SPI/MMC path available as the conservative fallback.

Useful references:

- NuttX SPI special driver docs:
  https://nuttx.apache.org/docs/latest/components/drivers/special/spi.html
- NuttX SDIO special driver docs:
  https://nuttx.apache.org/docs/latest/components/drivers/special/sdio.html
- Adafruit Fruit Jam pinout:
  https://learn.adafruit.com/adafruit-fruit-jam/pinout
