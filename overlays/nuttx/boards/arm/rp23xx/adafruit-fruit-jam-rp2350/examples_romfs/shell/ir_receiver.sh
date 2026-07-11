# Capture one IR frame; point a remote at the onboard IR receiver.

echo "Press an IR remote button within fifteen seconds"
irdump -n 256 -q 120 -t 15000
