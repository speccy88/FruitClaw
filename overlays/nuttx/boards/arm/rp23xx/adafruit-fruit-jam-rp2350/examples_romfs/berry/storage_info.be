import os

print("Fruit Jam filesystems")
for path : ["/examples", "/examples/shell", "/examples/berry", "/mnt/sd0"]
    if os.path.isdir(path)
        print(path + ":")
        for name : os.listdir(path)
            print("  " + name)
        end
    else
        print(path + ": not mounted")
    end
end

if os.path.isfile("/proc/meminfo")
    print("Memory:")
    file = open("/proc/meminfo", "r")
    print(file.read())
    file.close()
end
