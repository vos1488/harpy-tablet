import serial, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else 'COM5'
duration = int(sys.argv[2]) if len(sys.argv) > 2 else 20

print(f"Waiting for data on {port} for {duration}s...")
print(">>> Press RESET button on the board NOW <<<")

try:
    s = serial.Serial(port, 115200, timeout=1)
except Exception as e:
    print(f"Cannot open {port}: {e}")
    sys.exit(1)

buf = b''
start = time.time()
while time.time() - start < duration:
    avail = s.in_waiting
    if avail:
        buf += s.read(avail)
    time.sleep(0.05)

s.close()
text = buf.decode('utf-8', 'replace')
if text.strip():
    print("=== SERIAL OUTPUT ===")
    print(text[:8000])
else:
    print(f"No output on {port} in {duration}s")
