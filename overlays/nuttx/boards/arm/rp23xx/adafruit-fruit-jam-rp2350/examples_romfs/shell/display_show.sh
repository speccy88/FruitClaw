# Exercise the 320x240 RGB565 DVI framebuffer.

echo "Fruit Jam DVI display demo"
dvictrl start
dvictrl info

echo "Color bars"
dvictrl pattern colorbars
sleep 2

echo "Red, green, and blue RGB565 fills"
dvictrl solid 0xf800
sleep 1
dvictrl solid 0x07e0
sleep 1
dvictrl solid 0x001f
sleep 1

echo "Restoring color bars"
dvictrl pattern colorbars
