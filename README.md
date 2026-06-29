# FruitClaw

Adafruit Fruit Jam RP2350 NuttX bring-up workspace.

This repository is the project wrapper for the two NuttX source repositories
used by the bring-up:

- `nuttx`: board port and RP23xx support changes.
- `apps`: Berry plus NSH helper applications.

The source repositories are tracked here as submodules so their upstream NuttX
history remains intact.

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

From the `nuttx` submodule:

```sh
./tools/configure.sh -E -m -a ../apps adafruit-fruit-jam-rp2350:usbnsh
make -j8
```

The generated UF2 is `nuttx/nuttx.uf2`.
