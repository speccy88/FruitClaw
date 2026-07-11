# Safe, mostly read-only inventory of the Fruit Jam hardware interfaces.

echo "== Kernel =="
uname -a

echo "== Device nodes =="
ls -l /dev

echo "== Filesystem roots =="
ls -l /

echo "== DVI framebuffer =="
dvictrl info

echo "== Buttons =="
buttonctl supported
buttonctl state

echo "== I2C controllers =="
i2c bus

echo "== SPI controllers =="
spi bus

echo "== PIO USB host =="
piousbhost hidstatus
