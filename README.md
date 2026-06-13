# One-Rom-Plugins

A modified One ROM host-control (RBCP) plugin that indicates which Kernal is in
use on the Commodore 64, by driving a WS2812/SK6812 NeoPixel with a per-slot
colour. The bootloader's slot selection drives the LED colour, so you can see at
a glance which kernal is active.

The modifications to the host control have been tested on real hardware and
still allow the C64 to boot and choose kernals.

## Wiring

The NeoPixel data line is driven from **bank select pad A (GPIO24)**:

| NeoPixel | One ROM |
|----------|---------|
| DIN      | bank select pad A (GPIO24) |
| VDD      | J1 (3.3V) for SK6812, or 5V for WS2812B |
| GND      | J2 / bank select GND pad |

- An **SK6812** powered from 3.3V needs no level shifter.
- A **WS2812B** runs from the One ROM 5V rail (≈4.3V after the on-board
  protection diode — this is fine for the LED) and needs a **74AHCT125** level
  shifter on the data line. AHCT is required (not HCT/HC) because its TTL input
  threshold reliably recognises the 3.3V GPIO as a logic high.

## Changelog

### Fix: NeoPixel flicker with 74AHCT125 level shifter

**Symptom:** with a WS2812B behind a 74AHCT125, the LED flickered instead of
holding a stable per-kernal colour.

**Cause:** the original code tri-stated GPIO24 between updates (to preserve the
bank-select jumper read on a warm reboot). While tri-stated, pad A floated to
~1.5V — almost exactly the 74AHCT125's input switching threshold — so the
shifter output oscillated and fed noise to the LED's data line.

**Fix:** GPIO24 is now configured as a permanent output, held idle LOW between
updates (`neopixel_init` sets the output enable; `neopixel_show_flash_slot` no
longer clears it). The shifter input now has a defined level, so the LED holds
the colour it was last sent.

> Note: this trades away the old "tri-state to keep the bank-select jumper read"
> behaviour. That's intentional — pad A is repurposed as the NeoPixel data pin
> here, so it is no longer used for jumper-based bank selection.
