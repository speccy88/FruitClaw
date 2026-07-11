Fruit Jam Berry examples
========================

  berry /examples/berry/board_info.be
  berry /examples/berry/storage_info.be
  berry /examples/berry/buttons.be
  berry /examples/berry/neopixel_show.be
  berry /examples/berry/sound_show.be

Start DVI before the LVGL examples:

  dvictrl start
  berry /examples/berry/lvgl_dashboard.be
  berry /examples/berry/lvgl_controls.be

The hardware examples use Berry's os module to run the same NuttX commands
available at the NSH prompt. The LVGL examples additionally use the native lv
module, /dev/fb0, and optional USB mouse input from /dev/mouse0.
