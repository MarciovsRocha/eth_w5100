# Hardware tests

These tools were used to validate the driver on a real shield. They need a board on a serial port, a PC on the same
network and a Python with `pyserial` (the ESP-IDF environment has it). They are not part of the component and CI does
not build them.

| Tool | What it does |
|---|---|
| `capture_serial.py` | Resets the board through RTS and saves the console until a marker text or a timeout |
| `serial_passive.py` | Saves the console without resetting the board |
| `rx_broadcast_test.py` | Sends LAN broadcasts of several sizes and counts the ones the board logs |
| `phy_diag/` | Firmware without the driver, plus `phy_diag_host.py`, to tell a hardware fault from a driver fault |

## Reading the serial console

```bash
python capture_serial.py COM5 boot.log 40 "Interface stays up"
python serial_passive.py COM5 console.log 900
```

The port can have only one reader, so close the serial monitor first.

## Reception by frame size

`rx_broadcast_test.py` needs firmware built with `CONFIG_W5100_DEBUG_RX=y` and debug logging, because it counts the
`rx length` lines. For the promiscuous test, also turn the MAC filter off with `ETH_CMD_S_PROMISCUOUS`.

```bash
python rx_broadcast_test.py COM5 192.168.18.174 192.168.18.255 rx.log
```

## Hardware or driver?

`phy_diag` talks to the chip directly, and has three phases:

| Phase | What runs | What it isolates |
|---|---|---|
| A | The chip's own TCP/IP answers ping, with no SPI traffic | The chip and its power, with nothing from the ESP32 involved |
| B | MACRAW, with no SPI traffic during the window, then the ring is read | Reception, without polling |
| C | MACRAW, read every 10 ms like the driver | Reception with polling |

Set a free address on your network in `phy_diag/main/main.c`, then:

```bash
cd phy_diag
idf.py -p COM5 flash
python phy_diag_host.py COM5 192.168.18.174 192.168.18.250 192.168.18.255 phy.log
```

The last two arguments are the address set in the firmware and the network's broadcast address.

If phases A to C lose frames as well, the fault is not in the driver. On the tested shield this is how the power
problem in [docs/wiring.md](../../docs/wiring.md#power) was found.

The firmware in this folder waits for the chip after each reset. The version used in the 2026-11-03 test did not:
phase A was invalid in the run with the separate 5 V source, because the configuration was not applied. This version
has been built, but not run on hardware.
