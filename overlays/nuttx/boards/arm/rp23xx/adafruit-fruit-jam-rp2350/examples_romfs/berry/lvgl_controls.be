import lv
import os

lv.start()

scr = lv.scr_act()
scr.set_style_bg_color(lv.color(0x111827), lv.PART_MAIN)

title = lv.label(scr)
title.set_text("Berry LVGL hardware controls")
title.set_style_text_color(lv.color(0xffffff), lv.PART_MAIN)
title.align(lv.ALIGN_TOP_MID, 0, 12)

status = lv.label(scr)
status.set_text("Choose an action")
status.set_style_text_color(lv.color(0xb8c4d8), lv.PART_MAIN)
status.align(lv.ALIGN_BOTTOM_MID, 0, -14)

red_button = lv.btn(scr)
red_button.set_pos(15, 62)
red_button.set_size(90, 55)
red_button.set_style_bg_color(lv.color(0xc62828), lv.PART_MAIN)
red_label = lv.label(red_button)
red_label.set_text("Red")
red_label.center()

rainbow_button = lv.btn(scr)
rainbow_button.set_pos(115, 62)
rainbow_button.set_size(90, 55)
rainbow_button.set_style_bg_color(lv.color(0x1565c0), lv.PART_MAIN)
rainbow_label = lv.label(rainbow_button)
rainbow_label.set_text("Rainbow")
rainbow_label.center()

sound_button = lv.btn(scr)
sound_button.set_pos(215, 62)
sound_button.set_size(90, 55)
sound_button.set_style_bg_color(lv.color(0x6a1b9a), lv.PART_MAIN)
sound_label = lv.label(sound_button)
sound_label.set_text("Sound")
sound_label.center()

off_button = lv.btn(scr)
off_button.set_size(150, 48)
off_button.align(lv.ALIGN_CENTER, 0, 44)
off_label = lv.label(off_button)
off_label.set_text("NeoPixels off")
off_label.center()

def red_clicked(obj, code)
    os.system("neopixels", "red", "64")
    status.set_text("NeoPixels: red")
end

def rainbow_clicked(obj, code)
    status.set_text("NeoPixels: rainbow")
    os.system("neopixels", "rainbow", "1", "10", "72")
end

def sound_clicked(obj, code)
    status.set_text("Speaker: short scale")
    os.system("rtttl", "-r", "22050", "-v", "60", "Click:d=16,o=6,b=240:c,e,g,c7")
end

def off_clicked(obj, code)
    os.system("neopixels", "off")
    status.set_text("NeoPixels: off")
end

red_button.add_event_cb(red_clicked, lv.EVENT_CLICKED)
rainbow_button.add_event_cb(rainbow_clicked, lv.EVENT_CLICKED)
sound_button.add_event_cb(sound_clicked, lv.EVENT_CLICKED)
off_button.add_event_cb(off_clicked, lv.EVENT_CLICKED)

lv.run()
