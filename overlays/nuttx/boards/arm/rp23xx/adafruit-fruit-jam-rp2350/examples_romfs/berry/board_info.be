import os

print("Fruit Jam hardware inventory")
print("----------------------------")
os.system("uname", "-a")
os.system("dvictrl", "info")
os.system("buttonctl", "supported")
os.system("piousbhost", "hidstatus")
os.system("i2c", "bus")
os.system("spi", "bus")

print("Device nodes:")
for name : os.listdir("/dev")
    print("  /dev/" + name)
end
