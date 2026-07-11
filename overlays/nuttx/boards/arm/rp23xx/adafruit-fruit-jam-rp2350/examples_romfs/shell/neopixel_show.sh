# Cycle colors and effects on the five onboard NeoPixels.

echo "Fruit Jam NeoPixel show"
neopixels red 56
usleep 300000
neopixels orange 56
usleep 300000
neopixels yellow 56
usleep 300000
neopixels green 56
usleep 300000
neopixels cyan 56
usleep 300000
neopixels blue 56
usleep 300000
neopixels purple 56
usleep 300000
neopixels white 40
usleep 300000

neopixels rainbow 1 8 72
neopixels chase cyan 2 50 72
neopixels pulse purple 2 25 72
neopixels fire 30 25 72
neopixels off
echo "NeoPixels off"
