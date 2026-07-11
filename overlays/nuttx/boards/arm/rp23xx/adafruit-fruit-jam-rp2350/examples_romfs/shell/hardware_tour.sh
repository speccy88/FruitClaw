# Run the non-interactive hardware demonstrations in sequence.

echo "=== Fruit Jam hardware tour ==="
source /examples/shell/hardware_info.sh
source /examples/shell/neopixel_show.sh
source /examples/shell/sound_show.sh
source /examples/shell/display_show.sh
echo "=== Hardware tour complete ==="
echo "Source /examples/shell/buttons.sh or ir_receiver.sh for input demos."
