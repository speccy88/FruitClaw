# Adafruit Fruit Jam RP2350 NuttX Notes

This board directory tracks the current Fruit Jam RP2350 NuttX bring-up work.

## Current Bring-up Docs

- `PSRAM.md`: QMI CS1 PSRAM setup, recovery guard notes, local UF2 artifacts,
  and RAM speed benchmark results.
- `CONSOLE.md`: USB CDC/NSH readline notes, including the Enter-key prompt
  pile-up fix and arrow/history sanity checks.
- `ESP_HOSTED.md`: ESP32-C6 ESP-Hosted Wi-Fi architecture, pin mapping, and
  validated networking/service status.
- `SDIO.md`: microSD SPI/MMC baseline and future native PIO SDIO notes.
- `TRMNL_DVI_TIMING.md`: 2026-07-02 HSTX/DVI display bring-up result, including
  the working native 800x480 tight 25.2 MHz timing, failed 31.5 MHz attempts,
  and the Pico SDK probe baseline that should be used for future TRMNL work.

## Current Flashed Image

```text
/Users/fred/Documents/FruitClaw/nuttx/build-artifacts/fruitjam-readline-enter-lf.uf2
SHA256: 561dab885bc4d7ee81e277d79bf0c63224780c8e4e5872be3f688bae74a46147
```

This image includes the PSRAM/ramspeed work plus the USB CDC readline Enter-key
fix from `CONSOLE.md`.

## PSRAM Benchmark Snapshot

The benchmark results below were collected with:

```text
/Users/fred/Documents/FruitClaw/nuttx/build-artifacts/fruitjam-ramspeed-kmem-umem.uf2
SHA256: 0c7ace8db8f8279fe56646add1faae90585f6256199c412d3060f4433e77a802
```

Largest comparable 64 KiB rows from `ramspeed` on 2026-06-29:

| Heap | Command | system memcpy | internal memcpy | system memset | internal memset |
| ---- | ------- | ------------: | --------------: | ------------: | --------------: |
| Kmem, internal SRAM | `ramspeed -k -s 65536 -n 1000` | 164102.564 KB/s | 188235.294 KB/s | 20915.033 KB/s | 304761.905 KB/s |
| Umem, PSRAM | `ramspeed -a -s 65536 -n 1000` | 4545.455 KB/s | 4571.429 KB/s | 11449.016 KB/s | 22222.222 KB/s |

The larger PSRAM-only run, `ramspeed -a -s 1048576 -n 100`, held the same
plateau at the 1024 KiB row: 4547.069 KB/s system `memcpy`, 4573.470 KB/s
internal `memcpy`, 11466.965 KB/s system `memset`, and 22212.581 KB/s internal
`memset`.

Do not use `ramspeed -i` for these comparisons on the current Fruit Jam profile.
Disabling interrupts stops the tick source used by `clock_gettime()`, so the
benchmark reports the elapsed time as too short.
