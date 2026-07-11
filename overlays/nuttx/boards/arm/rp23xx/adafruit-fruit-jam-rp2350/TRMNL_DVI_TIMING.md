# Fruit Jam TRMNL DVI Timing Bring-up

This note records the 2026-07-02 Fruit Jam RP2350 HSTX/DVI bring-up result
for the future TRMNL-style 800x480 low-bit-depth framebuffer profile.  It is
intentionally detailed because a bad probe baseline made several useful display
modes look broken.  Do not repeat that path without reading this first.

## Result

Native 800x480 output works on the tested HDMI monitor when the Fruit Jam is
built from the official Pico SDK 2.2.0 `adafruit_fruit_jam` board definition and
uses the tight 25.2 MHz timing below.

The working timing is:

```text
horizontal active:      800
horizontal front porch:   8
horizontal sync:         16
horizontal back porch:   16
horizontal total:       840

vertical active:        480
vertical front porch:     5
vertical sync:            5
vertical back porch:     10
vertical total:         500

pixel clock:     25.200 MHz
refresh:         60.000 Hz  (25.2 MHz / 840 / 500)
```

Two Pico SDK 2.2.0 probes were verified live on the monitor:

```text
artifacts/hstx-probe/probe_frame_800x480_tight_rgb565_25m2.uf2
SHA256: 07eb08e6cde0c49fd4d15953bc03aced81a16914c36b8d57912a48fe82cd0c2e
Result: visible color-bar frame, user reported "it works"

artifacts/hstx-probe/probe_frame_800x480_tight_gray4_25m2.uf2
SHA256: f402f0918feedc4bee3a5fdc5b5d16593030cfda5ac9c7c783b491f9f06abb20
Result: visible four-gray frame, user reported "yes it works, its gray"
```

This means the panel does not reject 800x480 in general, and low-bit-depth
TRMNL-style grayscale output is a viable target.

## Baseline That Worked

The working probes were built with:

- Pico SDK 2.2.0
- `PICO_BOARD=adafruit_fruit_jam`
- RP2350B official board definition
- HSTX pins from the SDK board header:
  - CKN 12
  - CKP 13
  - D0N 14
  - D0P 15
  - D1N 16
  - D1P 17
  - D2N 18
  - D2P 19
- fixed Fruit Jam clock plan:
  - VREG 1.30 V
  - `clk_sys` 252 MHz
  - `clk_hstx` 126 MHz

The official board header checked during bring-up was:

```text
$HOME/.pico-sdk/sdk/2.2.0/src/boards/include/boards/adafruit_fruit_jam.h
```

That header also confirms `#define PICO_RP2350A 0`, meaning the board is the
RP2350B variant.

The current NuttX board clock plan already matches this direction in:

```text
nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/include/board.h
```

Important symbols in that file:

```text
BOARD_VREG_VSEL    0x0f      /* 1.30 V */
BOARD_PLL_SYS_FREQ 252 MHz
BOARD_SYS_FREQ     252 MHz
BOARD_USB_FREQ      48 MHz
BOARD_HSTX_FREQ    126 MHz when CONFIG_RP23XX_HSTX_DVI=y
```

Do not try to fix the TRMNL display path by backing away from this clock plan.
The user explicitly wants to keep the 252 MHz / 1.30 V plan because it matches
the known-good Fruit Jam direction and keeps USB work from drifting.

## What Failed

The following Pico SDK 2.2.0 frame probes were flashed and did not produce a
usable image on the tested monitor:

```text
artifacts/hstx-probe/probe_frame_800x480_rgb565_31m5.uf2
SHA256: 94acae84ee405b56856c460dd4da4d0399f8010f2e5b62aac24fe1dd19a64090
Result: blank / no visible output

artifacts/hstx-probe/probe_frame_800x480_lcd_rgb565_31m5.uf2
SHA256: e38c8b54c296690cf7b3866d61b745b8c59dee2c834974653cd750312d8f56b6
Result: blank / no visible output

artifacts/hstx-probe/probe_frame_800x600_window_rgb565_42m.uf2
SHA256: 0d897c7cabe128cea178441834b174af0cb069fe9e95092730e4fca2a177cfcb
Result: blank / no visible output

artifacts/hstx-probe/probe_frame_1280x720_window_rgb565_63m.uf2
SHA256: a4649e321c26be33e404e4394e0c88b5b92bcf21b71bff9460409ee00dbd02d0
Result: blank / no visible output
```

The important failure for NuttX is the 31.5 MHz native 800x480 path.  At the
time this note was written, `esp-hosted/defconfig` still had:

```text
CONFIG_RP23XX_HSTX_DVI_FB_Y2_800X480=y
CONFIG_RP23XX_HSTX_DVI_PIXEL_CLOCK=31500000
CONFIG_RP23XX_HSTX_DVI_Y2_OUTPUT_NATIVE_800X480=y
```

That 31.5 MHz native 800x480 setting should not be treated as hardware-proven.
It is the setting that cost time during TRMNL validation.

## Bad Probe Baseline

Early probes were built with an older Pico SDK 2.0.0 setup and a local/custom
Fruit Jam board shim.  Those probes produced misleading blank results, including
blank output for 640x480.  After switching to Pico SDK 2.2.0 and the official
`adafruit_fruit_jam` board definition, the equivalent 640x480 baseline worked:

```text
artifacts/hstx-probe/probe_frame_640x480_rgb565_25m2.uf2
SHA256: 61c2f665ef4b39aba39edeb3a03ccef8910165f40ca0ef4ce5185d816e01d8f1
Result: visible, user reported "it works now"
```

Treat blank results from the removed older-SDK probes as suspect unless they
are retested under the SDK 2.2.0 official board baseline.

## NuttX Integration Notes

The NuttX TRMNL display profile now uses the validated native 800x480 timing in:

```text
nuttx/arch/arm/src/rp23xx/Kconfig
nuttx/arch/arm/src/rp23xx/rp23xx_dvi.c
nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/configs/trmnl/defconfig
```

The dedicated TRMNL defconfig sets:

```text
CONFIG_RP23XX_HSTX_DVI_FB_Y2_800X480=y
CONFIG_RP23XX_HSTX_DVI_PIXEL_CLOCK=25200000
CONFIG_RP23XX_HSTX_DVI_Y2_OUTPUT_NATIVE_800X480=y
CONFIG_RP23XX_HSTX_DVI_SCANOUT_PSRAM=y
CONFIG_RP23XX_HSTX_DVI_SINGLE_SCANOUT_INTERNAL=y
CONFIG_SYSTEM_TRMNL=y
CONFIG_SYSTEM_TRMNL_BOOT_AUTOSTART=y
```

## Scanout Stability Finding

The first native 800x480 NuttX TRMNL image could draw a good-looking frame, but
the monitor intermittently dropped sync and came back.  `dvictrl info` showed
the live scanout buffers in PSRAM (`0x150...`) and frame gaps much longer than
one 60 Hz frame.  Reworking the DMA command list so the final block rewinds the
command channel in hardware improved the loop, but PSRAM live scanout still
produced occasional HDMI dropouts on the tested monitor.

The stable NuttX result used:

```text
CONFIG_RP23XX_HSTX_DVI_SINGLE_SCANOUT_INTERNAL=y
```

With that option, `/dev/fb0` remains the 800x480 Y2 application framebuffer and
large TRMNL buffers can still live in PSRAM, but the active RGB scanout frame is
allocated from internal SRAM.  The mode is single-buffered and intended for
static or mostly-static screens.  On the tested board, `dvictrl info` reported
the active scanout at `0x200...`, `frontback=0`, `dma_errors=0`, and a stable
maximum frame gap of about 16673 us during a 20 second soak.  A later
`dvictrl pattern colorbars` update copied in about 13 ms and did not disturb the
scanout loop.

The practical rule for TRMNL and future LVGL profiles is:

- Keep bulk image/application framebuffers in PSRAM when needed.
- Do not use PSRAM as the live HSTX/DVI scanout source for picky HDMI panels.
- For static 800x480 grayscale/TRMNL output, prefer one internal SRAM scanout
  frame and update it only when the screen content changes.
- For animated LVGL, use the same 252 MHz / 126 MHz HSTX clock plan and start
  from internal scanout first; add double buffering only after measuring frame
  gaps and DMA errors with `dvictrl info`.

The integration direction remains:

1. Keep the board clock plan in `include/board.h`:
   - VREG 1.30 V
   - system clock 252 MHz
   - USB clock 48 MHz
   - HSTX clock 126 MHz
2. Keep the RP23xx HSTX DVI native 800x480 mode on:
   - pixel clock 25.2 MHz
   - horizontal total 840
   - vertical total 500
   - the exact porch/sync values listed above
3. Use `adafruit-fruit-jam-rp2350:trmnl` for the minimal TRMNL image, not the
   broader `esp-hosted` FruitClaw operator image.
4. Use the single internal SRAM scanout option for the real TRMNL image.  PSRAM
   remains useful for the Y2 framebuffer, PNG downloads, decoded images, and
   future LVGL heap pressure, but not as the live scanout source on the tested
   monitor.

## FruitClaw Operator Profile

The broader `adafruit-fruit-jam-rp2350:esp-hosted` FruitClaw operator profile
does not use the TRMNL 800x480 Y2 framebuffer.  It intentionally stays on the
smaller LVGL/game-friendly 320x240 RGB565 framebuffer with 1:1 centered output
inside the standard 640x480 DVI timing:

```text
CONFIG_RP23XX_HSTX_DVI_FB_RGB565_320X240=y
CONFIG_RP23XX_HSTX_DVI_OUTPUT_SCALING=1
CONFIG_RP23XX_HSTX_DVI_PIXEL_CLOCK=25200000
CONFIG_RP23XX_HSTX_DVI_SCANOUT_PSRAM=y
```

The important TRMNL lesson carried into FruitClaw is the clock plan: keep VREG
at 1.30 V, `clk_sys` at 252 MHz, `clk_hstx` at 126 MHz, and the DVI pixel
clock at 25.2 MHz.  Do not drift the operator build toward the failed 31.5 MHz
800x480 experiments.

FruitClaw currently leaves the live 320x240 RGB565 scanout in PSRAM.  That is a
deliberate tradeoff for the large operator image: a single internal RGB565
scanout frame would cost about 150 KiB of internal SRAM, while the tested
operator build keeps a small kernel heap and many concurrent network,
filesystem, Berry, LVGL, USB host, and agent services.  If HDMI dropouts appear
in the operator profile, measure first with `dvictrl info`; only then consider
raising the internal kernel heap and enabling
`CONFIG_RP23XX_HSTX_DVI_SINGLE_SCANOUT_INTERNAL=y`.

The diagnostic NuttX profile currently has:

```text
nuttx/boards/arm/rp23xx/adafruit-fruit-jam-rp2350/configs/hstxdvi/defconfig
CONFIG_RP23XX_HSTX_DVI_PIXEL_CLOCK=25200000
CONFIG_RP23XX_HSTX_DVI_Y2_OUTPUT_VGA_640X480=y
```

That profile is still useful for isolation. The `trmnl` profile is the native
800x480 tight timing path for the TRMNL display client.

## Probe Flash Commands

From the repository root, the preserved probe artifacts can be flashed with:

```sh
./artifacts/hstx-probe/flash_probe.sh probe_frame_800x480_tight_rgb565_25m2.uf2
./artifacts/hstx-probe/flash_probe.sh probe_frame_800x480_tight_gray4_25m2.uf2
```

The SDK 2.2.0 frame probes include an auto-BOOTSEL guard of about 45 seconds.
That guard was for bring-up only; production firmware should use watchdog reset
and bootguard recovery policy appropriate to the image being tested.

## Checklist Before More Display Experiments

- Start from SDK 2.2.0 or newer with the official `adafruit_fruit_jam` board
  definition when making Pico SDK probes.
- Keep the Fruit Jam pin map on HSTX pins 12 through 19.
- Keep VREG 1.30 V, system clock 252 MHz, and HSTX clock 126 MHz unless there is
  a specific clock experiment being documented separately.
- Do not conclude that 800x480 is unsupported if a 31.5 MHz mode is blank.
- Do not conclude that Y2/four-gray output is broken; the gray probe worked.
- Do not use the older custom-board Pico SDK 2.0.0 probe results as final
  evidence.
- For the NuttX TRMNL path, use the dedicated `trmnl` profile before judging the
  TRMNL app or framebuffer renderer.
