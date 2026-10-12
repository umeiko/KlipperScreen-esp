import serial, time, sys
try:
    s = serial.Serial('COM5', 115200, timeout=1)
except Exception as e:
    print("open fail:", e); sys.exit(1)
t = time.time()
with open('serial_capture.log', 'w', encoding='utf-8', errors='replace') as f:
    while time.time() - t < 20:
        d = s.readline()
        if d:
            f.write(d.decode('utf-8', 'replace'))
            f.flush()
s.close()
print("done")
