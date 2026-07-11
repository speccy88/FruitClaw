# Play a short scale followed by the built-in RTTTL melodies.

echo "Fruit Jam speaker and RTTTL demo"
echo "Built-in tunes:"
rtttl -l

rtttl -r 22050 -v 65 "Scale:d=8,o=5,b=180:c,d,e,f,g,a,b,c6"
rtttl -r 22050 -v 65 gta3
rtttl -r 22050 -v 65 scratchy
rtttl -r 22050 -v 65 simpsons
rtttl -r 22050 -v 65 cantina
