"""Save the console without resetting the board. The file can be read while this runs.

    python serial_passive.py PORT OUTPUT SECONDS
"""
import sys
import time

import serial

ser = serial.Serial()
ser.port, ser.baudrate, ser.timeout = sys.argv[1], 115200, 0.2
ser.dtr = False
ser.rts = False
ser.open()
deadline = time.time() + float(sys.argv[3])
with open(sys.argv[2], "w", encoding="utf-8") as out:
    while time.time() < deadline:
        chunk = ser.read(4096).decode("utf-8", errors="replace")
        if chunk:
            out.write(chunk)
            out.flush()
ser.close()
