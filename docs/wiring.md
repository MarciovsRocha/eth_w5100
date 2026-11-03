# Wiring an Arduino Ethernet Shield (W5100) to an ESP32

These notes were checked against the netlist of the official Arduino Ethernet Shield (rev 3 schematic,
`Shield_Eth06-REV3.sch`, published by Arduino as open hardware). The W5100 HanRun clones copy this design.
Measure your board as described below before connecting it, because a clone may differ.

## Connections

The shield's SPI pins are only on the 2×3 **ICSP** header, not on D11–D13. As seen from the top of the shield:

```
        ICSP
 MISO  1 ■ ● 2  5V
 SCK   3 ● ● 4  MOSI
 RESET 5 ● ● 6  GND
```

| Signal | Shield | ESP32 | Notes |
|---|---|---|---|
| SCLK | ICSP 3 | **GPIO18** | VSPI native pin |
| MISO | ICSP 1 | **GPIO19** | VSPI native pin. Driven directly by the W5100, which is a 3.3 V output |
| MOSI | ICSP 4 | **GPIO23** | VSPI native pin |
| W5100 /SCS | D10 | **GPIO17** | The native VSPI CS is GPIO5, a strapping pin, so GPIO17 is used instead |
| SD card CS | D4 | **3V3** | Keeps the micro-SD slot deselected. Leave the slot empty |
| 5 V | 5V (or ICSP 2) | **VIN / 5V** | Feeds the shield's 3.3 V regulator |
| GND | GND (or ICSP 6) | **GND** | Common ground is required |
| RESET | ICSP 5 / RESET | **not connected** | See below |
| INT | D2 via the `INT` solder jumper | not connected | The jumper is open by default, and the driver polls |
| 3.3V header | 3V3 | not connected | On this shield the header's 3.3 V pin does not reach the shield's 3.3 V rail |

No SEN (SPI enable) wire is needed. The shield generates SEN from /SCS with a 74LVC1G14 inverter, so the W5100
is in SPI mode exactly while D10 is low.

## Why RESET stays unconnected

The shield's RESET pin (ICSP 5, the RESET header pin and the reset button) is **not** the W5100 `/RESET`.
It goes to the manual-reset input `/MR` of a **CAT811** supervisor, which is powered from **5 V** and pulls that
input up to its supply. The supervisor's output drives the W5100 `/RESET`. Two consequences follow:

- The pin sits at about 5 V, and an ESP32 GPIO is not 5 V tolerant.
- It is not needed. The CAT811 already resets the W5100 at power-up, and the driver also does a software reset
  (`MR.RST`) every time it starts.

Keep `rst_gpio = -1`, which is the default. Use `rst_gpio` only on boards that expose the W5100 `/RESET` pin
itself, at 3.3 V.

## Voltage levels

From the W5100 datasheet (v1.2.8, section 7) and the schematic:

- **W5100 inputs** are 5 V tolerant, with VIH ≥ 2.0 V. The ESP32's 3.3 V outputs drive SCLK, MOSI and /SCS
  correctly.
- **W5100 outputs** come from its 3.3 V supply (VOH ≥ 2.4 V), so MISO is safe for the ESP32.
- **The 74LVC1G125 buffers** between the bus and the SD card, and the SEN inverter, are powered from the
  shield's **3.3 V** rail.
- **The 3.3 V rail** comes from an MC33269-3.3 LDO fed by the 5 V pin. Give the shield a real 5 V supply: an ESP32
  board's VIN when powered over USB (about 4.7 V after its diode) is enough. Plan for roughly 150–200 mA for
  the shield. This is a typical figure for W5100 shields: the datasheet does not specify supply current.

## Check before connecting

Power only the shield (5V and GND), then measure against GND:

1. **ICSP 1 (MISO): 3.3 V or less.** On the original design it is the W5100 pin itself.
2. **ICSP 5 (RESET): expect about 5 V.** This confirms it is the supervisor input, and that it must stay
   unconnected.
3. **The small 5-pin buffers (SOT-23/SC-70): 3.3 V on their supply pin.** On a clone, this confirms that the
   buffered lines are at 3.3 V logic.

## ESP32 pins to avoid

These GPIOs are read at boot (strapping pins), or are otherwise busy, so the shield must not drive them:

| GPIO | Level required at boot | If wrong |
|---|---|---|
| 12 (MTDI) | low | Flash powered at 1.8 V: boot loop. The upstream driver used it for RESET |
| 0 | high | Boots into download mode |
| 2 | low or floating | Serial flashing fails |
| 15 (MTDO) | high | Boot log silenced |
| 5 | high | SDIO timing; it also toggles during boot |
| 1, 3 | — | UART0, used for flashing and the monitor |
| 6–11 | — | Internal flash, never usable |
| 34–39 | — | Input only, so they cannot drive SPI |

GPIO17, 18, 19 and 23 have none of these functions.
