import serial, time, sys
try:
    s = serial.Serial()
    s.port = 'COM5'
    s.baudrate = 115200
    s.timeout = 1
    s.open()
except Exception as e:
    print("open fail:", e); sys.exit(1)
# 不复位，保持当前运行状态
s.setDTR(False); s.setRTS(False)
t = time.time()
with open('serial_capture.log', 'w', encoding='utf-8', errors='replace') as f:
    while time.time() - t < 40:
        d = s.readline()
        if d:
            f.write(d.decode('utf-8','replace')); f.flush()
s.close(); print("done")
