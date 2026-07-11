import os

colors = ["red", "orange", "yellow", "green", "cyan", "blue", "purple", "pink", "white"]

print("Cycling the five Fruit Jam NeoPixels")
for color : colors
    print("  " + color)
    if os.system("neopixels", color, "72") != 0
        print("neopixels command failed")
        break
    end
    os.system("usleep", "250000")
end

print("Rainbow")
os.system("neopixels", "rainbow", "1", "12", "80")
os.system("neopixels", "off")
