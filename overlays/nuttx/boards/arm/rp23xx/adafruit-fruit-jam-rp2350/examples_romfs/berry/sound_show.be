import os

print("Fruit Jam speaker demo")
print("Short scale")
os.system("rtttl", "-r", "22050", "-v", "70", "Scale:d=8,o=5,b=180:c,d,e,f,g,a,b,c6")

for tune : ["gta3", "scratchy", "simpsons", "cantina"]
    print("Playing " + tune)
    if os.system("rtttl", "-r", "22050", "-v", "70", tune) != 0
        print("rtttl command failed")
        break
    end
end
