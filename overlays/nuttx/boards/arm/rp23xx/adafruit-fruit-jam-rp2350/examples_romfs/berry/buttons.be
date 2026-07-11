import os

print("Fruit Jam buttons: BUTTON1, BUTTON2, BUTTON3")
os.system("buttonctl", "supported")
print("Press and release buttons during the next ten seconds")
os.system("buttonctl", "watch", "100", "100")
