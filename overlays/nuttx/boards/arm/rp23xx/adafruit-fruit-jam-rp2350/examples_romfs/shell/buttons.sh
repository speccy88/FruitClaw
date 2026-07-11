# Print button changes for ten seconds (100 samples at 100 ms).

echo "Fruit Jam buttons: BUTTON1, BUTTON2, BUTTON3"
buttonctl supported
buttonctl state
echo "Press and release buttons during the next ten seconds"
buttonctl watch 100 100
