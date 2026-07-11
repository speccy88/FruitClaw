import lv
import os

lv.start()

scr = lv.scr_act()
scr.set_style_bg_color(lv.color(0x101820), lv.PART_MAIN)

title = lv.label(scr)
title.set_text("Fruit Jam hardware dashboard")
title.set_style_text_color(lv.color(0xffffff), lv.PART_MAIN)
title.align(lv.ALIGN_TOP_MID, 0, 16)

resolution = lv.label(scr)
resolution.set_text("Framebuffer " + str(lv.get_hor_res()) + " x " + str(lv.get_ver_res()))
resolution.set_style_text_color(lv.color(0x80d8ff), lv.PART_MAIN)
resolution.align(lv.ALIGN_TOP_MID, 0, 48)

uptime = lv.label(scr)
uptime.set_style_text_color(lv.color(0xffffff), lv.PART_MAIN)
uptime.center()

theme_button = lv.btn(scr)
theme_button.set_size(180, 52)
theme_button.align(lv.ALIGN_BOTTOM_MID, 0, -22)

theme_label = lv.label(theme_button)
theme_label.set_text("Change theme")
theme_label.center()

backgrounds = [0x101820, 0x19324d, 0x3b1f47, 0x173b2c]
pixels = ["blue", "cyan", "purple", "green"]
theme = [0]

def change_theme(obj, code)
    theme[0] = (theme[0] + 1) % backgrounds.size()
    scr.set_style_bg_color(lv.color(backgrounds[theme[0]]), lv.PART_MAIN)
    os.system("neopixels", pixels[theme[0]], "56")
end

theme_button.add_event_cb(change_theme, lv.EVENT_CLICKED)

started = lv.millis()
while true
    seconds = (lv.millis() - started) / 1000
    uptime.set_text("UI uptime " + str(seconds) + " s")
    uptime.center()
    lv.run(250)
end
