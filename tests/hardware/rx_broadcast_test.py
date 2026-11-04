"""Send LAN broadcasts of several sizes from the PC and count which ones the W5100 logs.

The firmware needs CONFIG_W5100_DEBUG_RX=y and debug logging: frames are counted by their "rx length" lines.

    python rx_broadcast_test.py PORT LOCAL_IP BROADCAST_IP OUTPUT
"""
import re
import socket
import sys
import threading
import time
from collections import Counter

import serial

PORT, LOCAL_IP, BROADCAST_IP, OUT = sys.argv[1:5]
PAYLOADS = [30, 120, 308, 1000]  # Ethernet frame = payload + 42
ROUNDS = 20
sent = Counter()


def sender():
    time.sleep(8)  # let the firmware start
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind((LOCAL_IP, 0))
    for _ in range(ROUNDS):
        for size in PAYLOADS:
            sock.sendto(b"W" * size, (BROADCAST_IP, 9))
            sent[size + 42] += 1
            time.sleep(0.7)


with serial.Serial(PORT, 115200, timeout=0.2) as ser, open(OUT, "w", encoding="utf-8") as out:
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    thread = threading.Thread(target=sender)
    thread.start()
    deadline = time.time() + 8 + ROUNDS * len(PAYLOADS) * 0.7 + 5
    while time.time() < deadline:
        out.write(ser.read(4096).decode("utf-8", errors="replace"))
    thread.join()

with open(OUT, encoding="utf-8") as log:
    text = log.read()
got = Counter(int(n) for n in re.findall(r"rx length = (\d+)", text))
for frame_len in sorted(sent):
    print(f"frame {frame_len:5d} bytes: sent {sent[frame_len]:3d}, received {got[frame_len]:3d}")
print("all received sizes:", sorted(got.items()))
print("RX ring warnings:", text.count("RX ring out of sync"))
