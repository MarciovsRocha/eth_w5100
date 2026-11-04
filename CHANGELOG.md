# Changelog

## 0.2.0 (planned)

The first release validated on hardware. It is tagged `v0.2.0` once the remaining acceptance tests in section 5
of [docs/porting-notes.md](docs/porting-notes.md) pass.

- First hardware results, on a NodeMCU-32S with a HanRun clone: the six-stage self-test passes, and MQTT QoS 1
  delivered 67 stored records with no duplicates or gaps.
- The wiring guide now says to power the shield directly from 5 V. Fed through the ESP32 board's VIN pin, the
  tested shield received almost no frames.
- The driver checks the chip every 2 s and restores it if it lost its configuration, for example after a power
  loss on the shield. This has not been tested on hardware yet.
- `w5100_init()` waits 10 ms after the software reset for the PLL to lock.

## 0.1.0 (not tagged)

The first release of the ESP-IDF 6.x port of [KaeLL/w5100_esp32](https://github.com/KaeLL/w5100_esp32) at
commit `2820651`.

- Builds on ESP-IDF 6.1, with no warnings in the component.
- Implements the hooks that IDF 6.x calls unconditionally (`phy.set_link`, `mac.transmit_ctrl_vargs`) and the
  MAC filter hooks.
- Fixes the TX/RX race on `S0_IR`, the split MACRAW frames, unbounded busy waits, RX ring validation, a double
  free on RX, and the task lifetime on deinit. Details are in [docs/porting-notes.md](docs/porting-notes.md).
- Adds a ready-made SPI lower layer (`eth-w5100-spi.h`), the `ethernet_basic` example, Kconfig options, the
  wiring guide and CI.
- API: `eth_init()` now returns `esp_err_t`. `eth_deinit()`, `eth_get_netif()` and `eth_get_handle()` are always
  available. The Kconfig options were renamed from `EMAC_*` to `W5100_*`.

