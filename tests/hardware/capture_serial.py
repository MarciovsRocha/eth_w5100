"""Reset the ESP32 through RTS and save its console until a marker text or a timeout.

    python capture_serial.py PORT OUTPUT SECONDS [MARKER]
"""
import sys
import time

import serial

port, out_path, seconds = sys.argv[1], sys.argv[2], float(sys.argv[3])
stop_marker = sys.argv[4] if len(sys.argv) > 4 else None

with serial.Serial(port, 115200, timeout=0.2) as ser, open(out_path, "w", encoding="utf-8") as out:
    # EN is on RTS, IO0 on DTR: keep IO0 high and pulse EN low
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    deadline = time.time() + seconds
    buf = ""
    while time.time() < deadline:
        chunk = ser.read(4096).decode("utf-8", errors="replace")
        if chunk:
            out.write(chunk)
            out.flush()
            buf = (buf + chunk)[-8192:]
            if stop_marker and stop_marker in buf:
                time.sleep(1.0)
                out.write(ser.read(8192).decode("utf-8", errors="replace"))
                break
