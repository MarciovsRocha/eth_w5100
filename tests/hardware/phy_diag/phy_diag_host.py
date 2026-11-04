"""Drive the phy_diag firmware: ping in phase A, send sized broadcasts in phases B and C.

    python phy_diag_host.py PORT LOCAL_IP TARGET_IP BROADCAST_IP OUTPUT
"""
import socket
import subprocess
import sys
import threading
import time

import serial

PORT, LOCAL_IP, TARGET, BROADCAST_IP, OUT = sys.argv[1:6]
SIZES = [30, 120, 308, 558]  # frames of 72, 162, 350 and 600 bytes
ROUNDS = 5
res = {}


def ping():
    # Windows ping options
    r = subprocess.run(["ping", "-n", "25", "-w", "500", TARGET], capture_output=True, text=True, errors="replace")
    res["ping_replies"] = r.stdout.count("TTL=")


def blast():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.bind((LOCAL_IP, 0))
    for _ in range(ROUNDS):
        for z in SIZES:
            s.sendto(b"W" * z, (BROADCAST_IP, 9))
            time.sleep(0.6)


with serial.Serial(PORT, 115200, timeout=0.2) as ser, open(OUT, "w", encoding="utf-8") as out:
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    buf = ""
    started = set()
    t0 = time.time()
    while time.time() - t0 < 110:
        c = ser.read(4096).decode("utf-8", errors="replace")
        out.write(c)
        out.flush()
        buf += c
        if "PHASE_A" in buf and "A" not in started:
            started.add("A")
            threading.Thread(target=ping, daemon=True).start()
        for p in "BC":
            if f"PHASE_{p}" in buf and p not in started:
                started.add(p)
                threading.Thread(target=blast, daemon=True).start()
        if "DONE" in buf:
            break

print("ping replies (of 25):", res.get("ping_replies"))
for line in buf.splitlines():
    if line.startswith(("B ", "C ", "SR=", "RMSR", "BAD")) or "frames" in line:
        print(line[:400])
