Fruit Jam NuttX ROMFS examples
==============================

The examples are split into exactly two families:

  /examples/shell/   NuttShell hardware demonstrations
  /examples/berry/   Berry hardware and LVGL demonstrations

Quick start
-----------

  source /examples/shell/hardware_info.sh
  source /examples/shell/neopixel_show.sh
  source /examples/shell/sound_show.sh
  source /examples/shell/hardware_tour.sh

  berry /examples/berry/board_info.be
  berry /examples/berry/neopixel_show.be
  berry /examples/berry/sound_show.be

For Berry LVGL, start DVI first:

  dvictrl start
  berry /examples/berry/lvgl_dashboard.be
  berry /examples/berry/lvgl_controls.be

Shell examples
--------------

  hardware_info.sh  Kernel, devices, filesystems, display, buttons,
                    I2C, SPI, and USB-host status.
  neopixel_show.sh  Colors and animated effects on all five NeoPixels.
  sound_show.sh     A short scale and the built-in RTTTL tunes.
  display_show.sh   DVI information, color bars, and RGB565 fills.
  buttons.sh        Read the three buttons for ten seconds.
  ir_receiver.sh    Wait up to fifteen seconds for an IR remote frame.
  hardware_tour.sh  Run the non-interactive visual/audio demonstrations.

Berry examples
--------------

  board_info.be       Run hardware status commands and list /dev.
  storage_info.be     Explore ROMFS, SD, and /proc with Berry file APIs.
  buttons.be          Read the three buttons for ten seconds.
  neopixel_show.be    Drive NeoPixels through Berry and os.system().
  sound_show.be       Play several RTTTL sounds from Berry.
  lvgl_dashboard.be   Live LVGL uptime display with theme/NeoPixel control.
  lvgl_controls.be    Clickable LVGL controls for NeoPixels and sound.

Notes
-----

Shell files are sourced because they are NSH command scripts. Berry files run
with the normal berry command; they do not require FruitClaw's generated-script
runner. The LVGL examples use /dev/fb0 at 320x240 and accept USB mouse input
from /dev/mouse0. Press Ctrl-C to stop a long-running Berry LVGL example.

The complete set is intended for the Fruit Jam lvgl and esp-hosted profiles.
Smaller profiles can still mount this ROMFS but may not enable every command.

This /examples filesystem is baked into the firmware and is read-only. Put
editable scripts on the SD card or in FruitClaw's writable scripts directory.
