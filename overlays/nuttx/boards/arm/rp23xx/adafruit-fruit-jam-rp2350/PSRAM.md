# Fruit Jam PSRAM Bring-up Notes

This note records the current PSRAM bring-up state for the Adafruit Fruit Jam
RP2350 NuttX port.

## Configuration

The `esp-hosted` configuration enables the RP2350 QMI CS1 PSRAM path:

```text
CONFIG_RP23XX_PSRAM=y
CONFIG_RP23XX_PSRAM_HEAP_USER=y
CONFIG_RP23XX_PSRAM_SIZE=8388608
CONFIG_RP23XX_PSRAM_MAX_SCK_HZ=63000000
CONFIG_RP23XX_PSRAM_WAIT_TIMEOUT=1000000
CONFIG_RP23XX_PSRAM_CS1_GPIO=47
CONFIG_MM_KERNEL_HEAP=y
```

The heap mode keeps the NuttX kernel heap in internal SRAM and uses the 8 MiB
PSRAM aperture as the user heap. The PSRAM heap uses the RP2350 no-cache CS1
alias at `0x15000000`.

## Implementation Notes

The PSRAM setup code lives in `arch/arm/src/rp23xx/rp23xx_psram.c` and is
included when `CONFIG_RP23XX_PSRAM=y`.

The setup sequence is based on the RP2350 QMI flow used by the Arduino-Pico
Fruit Jam variant:

- Configure GPIO47 for XIP CS1.
- Leave RP2350 ACCESSCTRL alone. A diagnostic image that tried to update the
  XIP access-control registers watchdog-reset at stage `0x15` before any PSRAM
  traffic. The working setup follows the Pico SDK/Arduino-Pico pattern and
  only configures the CS1 GPIO before using QMI.
- Use QMI direct mode to exit QPI, read the PSRAM ID, and enter QPI.
- Program the QMI M0 format/timing registers for memory-mapped PSRAM.
- Probe the first and last words through the no-cache PSRAM alias.

The Fruit Jam variant in Arduino-Pico identifies PSRAM CS as GPIO47 and uses
external PSRAM on QMI CS1.

## Recovery Guard

PSRAM bring-up can fail before USB CDC is available, so the board port arms a
watchdog boot guard very early in `rp23xx_boardearlyinitialize()`. If that
guard expires on the next boot, the board reboots into BOOTSEL through the
RP2350 ROM reboot function.

The RP23xx watchdog lower-half now preserves an already-running watchdog during
driver registration. This prevents the common watchdog device initialization
from accidentally disabling the Fruit Jam boot guard during early bring-up.

The guard also enables the RP2350 watchdog tick source explicitly and clears
the watchdog debug/JTAG pause bits. The first PSRAM diagnostic flash after
adding the guard still left the board invisible because the register update
changed the desired enable bit but did not include the pause bits in the update
mask; stale pause bits could therefore survive. The current image clears those
bits every time the guard is armed.

The guard stores its arming state in RP2350 POWMAN scratch registers as well as
watchdog scratch registers. This avoids relying only on watchdog scratch across
the RP2350 boot ROM path. The BOOTSEL ROM reboot call uses parameter 0 for
BOOTSEL flags and parameter 1 for the optional GPIO/diagnostic value; the guard
therefore passes the PSRAM diagnostic word in reboot parameter 1.

## Diagnostic Breadcrumbs

The PSRAM driver writes diagnostic breadcrumbs to watchdog scratch registers
and POWMAN scratch registers:

- Scratch 2: packed value `(stage << 24) | (value & 0x00ffffff)`.
- Scratch 3: full diagnostic value.
- POWMAN scratch 2: same packed value.
- POWMAN scratch 3: same full diagnostic value.

When the boot guard sends the board to BOOTSEL, the packed diagnostic value is
passed as BOOTSEL reboot parameter 1. `picotool info -a` can then be used to decode where the
boot stopped.

Useful stages:

| Stage | Meaning |
| ----- | ------- |
| `0x10` | Entered `rp23xx_psramconfig()` |
| `0x11` | GPIO/access-control setup completed |
| `0x12` | IRQs disabled before PSRAM init |
| `0x13` | PSRAM init returned |
| `0x15` | GPIO setup started |
| `0x16` | ACCESSCTRL skipped; GPIO setup continuing |
| `0x17` | CS1 pad configured |
| `0x18` | CS1 function select configured |
| `0x20` | PSRAM detect started |
| `0x21` | PSRAM ID bytes read |
| `0x30` | PSRAM size detected |
| `0x31` | QPI entry completed |
| `0x32` | QMI timing calculated |
| `0x33` | Before memory-mapped probe |
| `0x40` | Before first-word probe write |
| `0x41` | After first-word probe read |
| `0x42` | Before last-word probe write |
| `0x43` | After last-word probe read |

For example, reboot parameter `0x43005a5a` means the code reached stage `0x43`
and the low 24 bits of the observed value were `0x005a5a`.

The failing ACCESSCTRL image produced reboot parameter 1 `0x1500002f`, meaning
stage `0x15` with GPIO47 (`0x2f`). Removing the ACCESSCTRL writes allowed the
same board to boot to NSH with the 8 MiB PSRAM user heap.

## Local Artifacts

UF2 images are kept under the local build artifacts directory:

```text
/Users/fred/Documents/FruitClaw/nuttx/build-artifacts/fruitjam-readline-enter-lf.uf2
/Users/fred/Documents/FruitClaw/nuttx/build-artifacts/fruitjam-ramspeed-kmem-umem.uf2
/Users/fred/Documents/FruitClaw/nuttx/build-artifacts/fruitjam-psram-no-accessctrl.uf2
/Users/fred/Documents/FruitClaw/nuttx/build-artifacts/fruitjam-psram-powman-guard.uf2
/Users/fred/Documents/FruitClaw/nuttx/build-artifacts/fruitjam-psram-diag-guard.uf2
/Users/fred/Documents/FruitClaw/nuttx/build-artifacts/fruitjam-recovery-nopsram.uf2
```

Known hashes:

```text
561dab885bc4d7ee81e277d79bf0c63224780c8e4e5872be3f688bae74a46147  fruitjam-readline-enter-lf.uf2
0c7ace8db8f8279fe56646add1faae90585f6256199c412d3060f4433e77a802  fruitjam-ramspeed-kmem-umem.uf2
f5c9a681b023f326efb3d5ddfedbc665c94f3bf097b0f5a73db87a6d935967e2  fruitjam-psram-no-accessctrl.uf2
1161310b24fe9c999f8786d8da8e30c3636d8988c0424b6df8e08b009f174442  fruitjam-psram-powman-guard.uf2
487d719b155753cf433378bde50c79be64a70c3809c0c5a3cb915cdc193c9df5  fruitjam-psram-diag-guard.uf2
625417c6a9fca821f48dc01eaf4486e09aafd21ae5bfeaa23a59a006d4f12db9  fruitjam-recovery-nopsram.uf2
```

The recovery image is built from the same board feature set with
`CONFIG_RP23XX_PSRAM` disabled. It is intended to restore CDC/NSH if a PSRAM
test image leaves the board unreachable.

## Current Hardware State

As of 2026-06-29 13:31 EDT, the Fruit Jam boots the
`fruitjam-readline-enter-lf.uf2` image and enumerates as USB CDC on
`/dev/cu.usbmodem01`.

Verified NSH output:

```text
NuttX  3.6.1 ea72397d34-dirty Jun 29 2026 13:31:54 arm adafruit-fruit-jam-rp2350
Kmem total: 402984
Umem total: 8388608
Umem free: 8377992
bootguard: disarmed
```

Flash the current working image from BOOTSEL with:

```sh
cp -X /Users/fred/Documents/FruitClaw/nuttx/build-artifacts/fruitjam-readline-enter-lf.uf2 /Volumes/RP2350/ && sync
```

A no-PSRAM recovery image is also available if the goal is just to restore
CDC/NSH before continuing diagnostics.

## RAM Speed Benchmark

The local `apps/benchmarks/ramspeed` command was extended with `-k` so the same
test can allocate buffers from the internal SRAM kernel heap (`kmm_malloc`) or
from the PSRAM-backed user heap (`malloc`). The `esp-hosted` defconfig enables
`CONFIG_BENCHMARK_RAMSPEED=y`.

Benchmark image:

```text
/Users/fred/Documents/FruitClaw/nuttx/build-artifacts/fruitjam-ramspeed-kmem-umem.uf2
SHA256: 0c7ace8db8f8279fe56646add1faae90585f6256199c412d3060f4433e77a802
```

The commands below were run from NSH over USB CDC on `/dev/cu.usbmodem01`.
Do not add `-i` for this benchmark on the Fruit Jam profile: disabling
interrupts also stops the tick source used by `clock_gettime()`, so every
measurement reports as too short.

```sh
ramspeed -k -s 65536 -n 1000
ramspeed -a -s 65536 -n 1000
ramspeed -a -s 1048576 -n 100
```

Representative largest-row results from 2026-06-29 13:21 EDT:

| Heap | Command | Row | system memcpy | internal memcpy | system memset | internal memset |
| ---- | ------- | ---: | ------------: | --------------: | ------------: | --------------: |
| Kmem, internal SRAM | `ramspeed -k -s 65536 -n 1000` | 64 KiB | 164102.564 KB/s | 188235.294 KB/s | 20915.033 KB/s | 304761.905 KB/s |
| Umem, PSRAM | `ramspeed -a -s 65536 -n 1000` | 64 KiB | 4545.455 KB/s | 4571.429 KB/s | 11449.016 KB/s | 22222.222 KB/s |
| Umem, PSRAM | `ramspeed -a -s 1048576 -n 100` | 1024 KiB | 4547.069 KB/s | 4573.470 KB/s | 11466.965 KB/s | 22212.581 KB/s |

The 64 KiB like-for-like row shows PSRAM `memcpy` at about 2.8 percent of the
internal SRAM `memcpy` rate for this uncached CS1 mapping. PSRAM `memset` is
less extreme: the system `memset` path is about 55 percent of the internal SRAM
system `memset` rate, while the internal `ramspeed` memset loop is about 7.3
percent of its internal-SRAM rate.

Raw logs:

```text
/Users/fred/Documents/FruitClaw/nuttx/build-artifacts/fruitjam-ramspeed-kmem-umem-noirq.log
/Users/fred/Documents/FruitClaw/nuttx/build-artifacts/fruitjam-ramspeed-umem-1m-n100.log
```

If a PSRAM diagnostic image fails, read the BOOTSEL reboot parameters:

```sh
picotool info -a
```

For current guard images, the high byte of reboot parameter 1 is the stage
number from the table above.
