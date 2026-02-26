import serial, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else 'COM8'
print(f"Reading {port}...")

try:
    s = serial.Serial(port, 115200, timeout=1)
except Exception as e:
    print(f"Cannot open {port}: {e}")
    sys.exit(1)

# Toggle DTR/RTS for reset
s.dtr = False
s.rts = True
time.sleep(0.1)
s.rts = False
time.sleep(0.5)

buf = b''
start = time.time()
while time.time() - start < 10:
    avail = s.in_waiting
    if avail:
        buf += s.read(avail)
    time.sleep(0.05)

s.close()
text = buf.decode('utf-8', 'replace')
if text.strip():
    print(text[:5000])
else:
    print(f"No output on {port}")
