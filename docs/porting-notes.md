# Porting notes: KaeLL/w5100_esp32 → ESP-IDF 6.x

The upstream driver, last updated for IDF 5.2.1 at commit `2820651`, is a correct design: an `esp_eth` MAC/PHY
pair on the W5100 MACRAW socket. It needed two kinds of change:

1. **Interface changes in IDF 6.x.** The Ethernet driver calls some hooks unconditionally, and those hooks did
   not exist before.
2. **Fixes found while porting.** These are race conditions and unbounded waits that IDF 5 did not expose either.

Each change below is checked against the IDF 6.1 sources (`components/esp_eth`) and the
**W5100 datasheet v1.2.8**.

## 1. IDF 6.x interface

| IDF 6.1 member | How IDF uses it | Port |
|---|---|---|
| `esp_eth_phy_t.set_link` | Called unconditionally by `esp_eth_stop()` (`esp_eth.c`) | **Was NULL, so stopping the driver crashed.** Implemented: it caches the link state and reports changes |
| `esp_eth_mac_t.transmit_ctrl_vargs` | Called unconditionally by `esp_eth_transmit_ctrl_vargs()` | **Was NULL.** Implemented: the `(buf, len)` pairs are written back to back into the TX buffer and sent as one frame. `ctrl` (timestamping) is ignored |
| `add_mac_filter` / `rm_mac_filter` | Called by lwIP on every IGMP/MLD join through the netif glue. NULL makes `esp_eth_ioctl` fail, and lwIP logs an error each time | Accept the request and warn once: the W5100 has no multicast table |
| `set_all_multicast`, `custom_ioctl` | Checked for NULL by IDF | Left NULL (returns `ESP_ERR_NOT_SUPPORTED`) |
| `transmit_vargs` | Deprecated | Left NULL |
| W5500 / DM9051 / KSZ8851 drivers | Removed from IDF in 6.0 (migration guide, networking) | Not used; no change |
| `VSPI_HOST` | Only an alias of `SPI3_HOST` on the ESP32 | `SPI3_HOST` |

## 2. Fixes

| Problem in upstream | Effect | Fix |
|---|---|---|
| The RX task read `S0_IR` and wrote all set bits back to clear them | The bits included `SEND_OK`, which the TX path (lwIP task) was waiting for, so TX could wait forever | RX now polls `S0_RX_RSR`, the alternative the datasheet gives in its receive pseudo code (§5.2). TX clears only its own `SEND_OK` bit |
| A frame larger than the free TX space was sent in chunks, one `SEND` each | In MACRAW each `SEND` is one Ethernet frame, so this produced two broken frames | Wait (bounded) until the whole frame fits, then issue a single `SEND` |
| The MAC filter bit `S0_MR.MF` was written after `OPEN` | The datasheet only describes writing `Sn_MR` before `OPEN` | Filter and promiscuous changes reopen the socket |
| Busy waits (`while (reg) ;`) on reset, commands and `SEND_OK` had no bound | A missing chip or bad wiring hung the calling task, or tripped the watchdog | Every wait is bounded by `CONFIG_W5100_CMD_TIMEOUT_MS` and returns an error |
| No check that the chip answers | A floating MISO looked like a working chip | After reset, `RMSR`/`TMSR` are read back. On mismatch `eth_init()` returns `ESP_ERR_INVALID_RESPONSE` |
| RX frame length not validated | A desynchronised ring caused huge `malloc`s or garbage frames | Length must be within `[14, 1522]` and ≤ `S0_RX_RSR`, otherwise the socket is reopened |
| Allocation failure returned without consuming the frame | RX stalled on that frame | The frame is consumed (dropped) anyway |
| `free()` after a failed `stack_input` | The IDF stack owns the buffer on error too (IDF EMAC does not free it), so this was a double free | Removed |
| The TX and RX paths interleaved multi-register sequences | Both use `S0_CR`, so a command could be lost | A mutex in the MAC layer serializes socket 0 |
| The RX task was created in `new()`, and deinit did not wait for it to exit | Use after free on `del()`. A second `install` after `uninstall` had no RX task | The task is created in `init()`, and `deinit()` waits for it to exit |
| PHY link kept in a `static bool` | After stop/start the link was never reported up again | The link state lives per instance. `ETH_LINK_UP` is 0, so it is initialised to DOWN explicitly |
| RX task pinned to core 1 by default | Competes with application code on the APP CPU | Default core 0, the same as lwIP, configurable |
| `EMAC_RX_TASK_YIELD_TICKS` allowed 0 | The task never blocks and starves its core | `W5100_RX_POLL_TICKS` ≥ 1 |
| `eth_init()` returned `void` and aborted on any error | No way to report a missing shield | Returns `esp_err_t` and cleans up on failure |
| `eth_deinit()` was only compiled with `CONFIG_TEST_DEINIT` | Declared in the public header, but a link error if used | Always available. It also deletes the netif glue and the event handler |
| An empty hostname registered a handler with `NULL` | `esp_netif_set_hostname(NULL)` aborted | The handler is registered only when a hostname is given |
| Missing `<string.h>`/`<stdlib.h>` | Implicit declarations, which are errors on GCC 14 | Added |
| A chip reset while running was not noticed | After a power loss on the shield the socket stayed closed until the ESP32 restarted | The RX task reads `S0_SR` every 2 s. If the socket is not in MACRAW, it resets the chip, restores the MAC address and reopens the socket |
| The lower layer (SPI, pins) was only in the author's demo project, with board-specific pins (RESET on strapping pin GPIO12, SEN on GPIO22) | Had to be rewritten for every board | `eth-w5100-spi.h`: a ready-made lower layer with configurable host, CS, RESET, SEN and clock, using polling transactions |

## 3. Datasheet cross-check (W5100 v1.2.8)

| Item | Datasheet | Code |
|---|---|---|
| SPI mode | Mode 0, MSB first (§6.3.1) | `.mode = 0`; the ESP32 SPI master is MSB first |
| SPI frame | 32-bit unit: op-code, address (2 bytes), data. Write `0xF0`, read `0x0F` (§6.3.2) | `W_PCK`/`R_PCK`, byte-swapped so the bytes go out in that order; read data is the 4th byte |
| /SS per frame | /SS high after each unit (§6.3.3) | One SPI transaction per byte, with hardware CS |
| SCLK | Minimum period 70 ns, so ≤ 14.28 MHz (§7, SPI timing) | Example default 4 MHz, Kconfig range up to 14 MHz |
| SEN | High = SPI mode enable (§1.1) | Optional `spi_en_gpio`. Arduino shields derive it from /SCS |
| /RESET | Low ≥ 2 µs; PLL lock up to 10 ms (§7, reset timing) | 2 ms low, 10 ms wait |
| MR | `0x0000`; bit 7 RST, auto-cleared (§4.1) | `MR0 0x0`, `MR_RST 0x80`, polled with a timeout |
| SHAR | `0x0009–0x000E` | `SHAR0 0x9`, 6 bytes |
| RMSR/TMSR | `0x001A`/`0x001B`; bits 1:0 = socket 0; `11` = 8 KB (§4.1) | Value `3`: all 8 KB to socket 0 |
| Memory | TX base `0x4000`, RX base `0x6000` (memory map) | `SBUFBASEADDRESS`, `RBUFBASEADDRESS`; mask `0x1FFF` for 8 KB |
| S0 registers | MR `0x400`, CR `0x401`, IR `0x402`, SR `0x403`, TX_FSR `0x420`, TX_RR `0x422`, TX_WR `0x424`, RX_RSR `0x426`, RX_RD `0x428` | Same. `0x042A` is "Reserved" in v1.2.8, so the upstream `S0_RX_WR0` define was removed |
| 16-bit registers | Read the upper byte first (§4.2) | `wiz_r_cont(addr, 2)` reads addr, then addr+1, big endian |
| Sn_MR | MACRAW = `0100`; MF = bit 6, S0 only (§4.2) | `S0_MR_MACRAW 0x04`, `S0_MR_MF 0x40` |
| Sn_CR | OPEN `0x01`, CLOSE `0x10`, SEND `0x20`, RECV `0x40`; auto-cleared | Same; `exec_cmd()` waits for 0 |
| Sn_IR | SEND_OK bit 4; cleared by writing 1 (§4.2) | `S0_IR_SEND_OK 0x10`, written alone |
| Sn_SR | SOCK_MACRAW `0x42` | Checked after OPEN, as in the MACRAW pseudo code (§5.2.4) |
| TX procedure | Write at `TX_WR & mask`, wrap at the boundary, `TX_WR += len`, SEND (§4.2) | `write_tx_buf()` and `w5100_socket_send()` |
| RX procedure | Read at `RX_RD & mask`, wrap, `RX_RD += len` (never more than `RX_RSR`), RECV (§4.2) | `read_rx_buf()` and `w5100_socket_rx_consume()`; `rx_peek()` checks the length against `RX_RSR` |
| MACRAW header | A 2-byte size header precedes each frame (§5.2.4) | The header counts its own 2 bytes, as WIZnet's ioLibrary does; the datasheet shows it only as a figure |
| PHY status | No PHY status register in the register map. Speed/duplex come from the OPMODE2–0 pins (§1.2) | The PHY reports the link up when started |

## 4. Shield schematic cross-check (Arduino Ethernet Shield rev 3 netlist)

| Net | Connects | Consequence for the driver/wiring |
|---|---|---|
| `SEN` | 74LVC1G14 output (input = `SS` / D10), 10 kΩ pull-up to 3.3 V | `spi_en_gpio = -1` |
| `MISO` | W5100 MISO, ICSP 1 and SD DAT0, unbuffered | 3.3 V logic. The SD card shares the line, so D4 must keep it deselected |
| `SCK`, `MOSI` | W5100 directly; to the SD card through 74LVC1G125 buffers (OE tied low, 3.3 V supply) | — |
| `RESET` | ICSP 5, header RESET and button, going to the `/MR` of a CAT811 powered from 5 V | Do not drive it from the ESP32; `rst_gpio = -1` |
| `RESET_W` | CAT811 `RES` going to the W5100 `/RESET` | Power-on reset is done by the shield |
| `SD_CS` | D4, through a buffer, to SD DAT3/CD | Tie it to 3.3 V |
| `INT` | W5100 /INT, going to an open solder jumper, then D2 | Not used (polling) |
| OPMODE2–0 | Tied to GND (`000`, autonegotiation) | Link reported as 100 Mbps full duplex; the real result shows only on the LEDs |
| `/CS`, `/RD`, `/WR` | 10 kΩ pull-up to 3.3 V; address and data bus to GND | The parallel bus is disabled |
| `+3V3` | MC33269-3.3 LDO from `+5V`. The 3.3 V header pin is not on this net | Power the shield from 5 V only, and not through the ESP32's VIN ([wiring guide](wiring.md#power)) |

## 5. Hardware validation

Hardware: NodeMCU-32S, HanRun W5100 clone with a `511` network on the Ethernet lines, a switch, and the shield on
a separate 5 V source (an Arduino UNO's 5V pin). The test was on 2026-11-03.

| Test | Result |
|---|---|
| Self-test (SPI, DHCP, ping, DNS, TCP, MQTT) with a broker on the local network | All six stages pass, 10 of 10 reboots. An address arrives in about 3 s |
| Ping from a PC | 20 of 20, 9 ms on average |
| LAN broadcasts of 72, 162, 350 and 600 bytes, read every 10 ms by a test firmware without this driver | 20 of 20 |
| Frame header | The length includes its own 2 bytes: frames are parsed intact |
| MQTT QoS 1, a backlog of 67 records in 4 batches, each deleted on its PUBACK | All delivered, with no duplicates or gaps |

Still to do for 0.2.0:

- Behaviour when the cable is unplugged and plugged back in. The link is not detected, so TCP must recover by its
  own timeouts.
- Sustained traffic over many hours.
- The shield powered on after the ESP32, and losing power while running. Both are implemented, but not tested.
- Measured throughput and CPU load of the RX task at 4 and 14 MHz.
- Promiscuous mode on and off at runtime. Turning it on at startup was tested.
