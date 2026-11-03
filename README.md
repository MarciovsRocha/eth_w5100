# eth_w5100: WIZnet W5100 Ethernet for ESP-IDF 6.x

[![build](https://github.com/MarciovsRocha/eth_w5100/actions/workflows/build.yml/badge.svg)](https://github.com/MarciovsRocha/eth_w5100/actions/workflows/build.yml)

*Leia em português: [README.pt-BR.md](README.pt-BR.md)*

An `esp_eth` MAC/PHY driver for the WIZnet **W5100**, the chip on the classic Arduino Ethernet Shield, for
**ESP-IDF 6.x**. It plugs into `esp_netif`, so everything built on lwIP works over the cable unchanged:
esp-mqtt, esp_http_client, SNTP, DNS, sockets.

> **Status: 0.1.0.** It builds on ESP-IDF 6.1, checked by CI on every push. **It has not been tested on
> hardware yet.** The first test on a real Arduino Ethernet Shield is pending, and results will be recorded in
> [CHANGELOG.md](CHANGELOG.md).

## Why a driver for the W5100?

Espressif maintains drivers for the W5500, DM9051, KSZ8851SNL and other chips, in
[esp-eth-drivers](https://github.com/espressif/esp-eth-drivers). They were moved out of ESP-IDF in 6.0. There is
no driver for the W5100, which is still the cheapest and most common shield around.

The W5100 has a TCP/IP stack in hardware, but **this driver does not use it**. Socket 0 is opened in
**MACRAW** mode, where the chip only exchanges raw Ethernet frames, and lwIP on the ESP32 does all the TCP/IP.
This is the same approach as Espressif's W5500 driver. The result is a normal network interface, with no
4-socket limit and no second TCP/IP stack to keep in sync.

## Origin

This is a port of [KaeLL/w5100_esp32](https://github.com/KaeLL/w5100_esp32), written for IDF 5.2.1 and released
into the public domain. Its full git history is kept in this repository. The changes for IDF 6.x and the
fixes found along the way are listed in [docs/porting-notes.md](docs/porting-notes.md). That document also
cross-checks every register, bit and timing against the W5100 datasheet v1.2.8 and the Arduino Ethernet Shield
schematic.

## Quick start

1. Add the component to your project's `main/idf_component.yml`:

   ```yaml
   dependencies:
     eth_w5100:
       git: https://github.com/MarciovsRocha/eth_w5100.git
       version: "main"   # a v0.1.0 tag follows the first hardware validation
   ```

2. Bring up the interface:

   ```c
   #include "eth-w5100.h"
   #include "eth-w5100-spi.h"

   ESP_ERROR_CHECK( esp_netif_init() );
   ESP_ERROR_CHECK( esp_event_loop_create_default() );

   // MISO/MOSI/SCLK; each W5100 access is a 4-byte frame, so no DMA
   ESP_ERROR_CHECK( spi_bus_initialize( SPI3_HOST, &( spi_bus_config_t ) {
       .miso_io_num = 19, .mosi_io_num = 23, .sclk_io_num = 18,
       .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4 }, SPI_DMA_DISABLED ) );

   struct w5100_spi_config spi = W5100_SPI_DEFAULT_CONFIG( SPI3_HOST, 17 );  // CS = GPIO17
   struct eth_ifconfig ifcfg = { .hostname = "my-device" };                 // zeroed sip = DHCP
   w5100_spi_get_ops( &spi, &ifcfg.w5100_cfg );
   ESP_ERROR_CHECK( eth_init( &ifcfg ) );
   // wait for IP_EVENT_ETH_GOT_IP, then use any lwIP based API
   ```

The network configures itself: by default the interface gets its address, gateway and DNS server by DHCP,
as soon as the cable is plugged into a network with a DHCP server. A static address can be set through
`ifcfg.sip`.

The complete program is [examples/ethernet_basic](examples/ethernet_basic), a self-test in six stages. Each stage
prints PASS, FAIL or SKIP, and a summary comes at the end:

| Stage | What it proves |
|---|---|
| SPI | The W5100 answers on the bus |
| DHCP | Address, gateway and DNS were obtained from the network |
| PING | The gateway answers ICMP, so TX and RX both work |
| DNS | A public name resolves (default `example.com`) |
| TCP | An HTTP `HEAD` to that host gets an answer over the internet |
| MQTT | A message published to the broker comes back on its own subscription (default `mqtt://test.mosquitto.org`) |

```bash
cd examples/ethernet_basic
idf.py set-target esp32
idf.py -p PORT flash monitor
```

## Wiring (Arduino Ethernet Shield → ESP32)

| Signal | Shield | ESP32 |
|---|---|---|
| SCLK | ICSP pin 3 | GPIO18 |
| MISO | ICSP pin 1 | GPIO19 |
| MOSI | ICSP pin 4 | GPIO23 |
| W5100 CS | D10 | GPIO17 |
| SD card CS | D4 | 3V3 (keeps the SD card off the bus) |
| 5 V | 5V | VIN / 5V |
| GND | GND | GND |
| RESET | — | **not connected** (it is a 5 V supervisor input, see below) |

All of these ESP32 pins are free of boot strapping functions. The full explanation is in
[docs/wiring.md](docs/wiring.md), which covers the voltages, why RESET stays unconnected, and the strapping
pins to avoid.

## Configuration (`idf.py menuconfig` → W5100 Ethernet (SPI))

| Option | Default | Meaning |
|---|---|---|
| `W5100_RX_TASK_CORE` | core 0 | Core of the RX polling task. Keep it on lwIP's core so the other core stays free for time-critical code |
| `W5100_RX_TASK_PRIORITY` | 15 | Priority of the RX polling task (the IDF drivers use 15) |
| `W5100_RX_TASK_STACK_SIZE` | 4096 | Stack of the RX polling task |
| `W5100_RX_POLL_TICKS` | 1 | Sleep between polls when no frame is waiting |
| `W5100_CMD_TIMEOUT_MS` | 100 | Upper bound for every busy wait on the chip |
| `W5100_DEBUG_TX` / `_RX` | off | Hex dump of every frame |

## Limitations

- **No multicast filter.** The W5100 MAC filter passes only its own MAC and broadcast, so mDNS and IPv6
  neighbour discovery do not work unless promiscuous mode is enabled. IPv4 unicast (TCP, UDP, DHCP, DNS, ICMP)
  is unaffected. The driver warns once at startup.
- **Link is always reported up.** The W5100 has no PHY status register, so a pulled cable is not detected by the
  driver. Use application-level timeouts, for example the MQTT keepalive.
- **Polling, no interrupt.** The INT pin is not used. An idle poll costs a few SPI transactions per tick on the
  RX task's core.
- **Throughput.** The W5100 SPI protocol moves one byte per 32-bit frame. At the default 4 MHz that is an
  estimated few tens of kB/s, which suits telemetry and MQTT but not bulk transfers. This has not been measured
  yet.
- One W5100 per firmware; tested target: `esp32`.

## License

Public domain ([The Unlicense](LICENSE)), the same terms as the upstream project. Its author asks that
improvements be contributed back, and this port intends to offer them upstream once it is validated on
hardware.
