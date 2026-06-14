# One-Rom-Plugins

A modified One ROM **host-control (RBCP)** plugin that indicates which kernal is
active on a Commodore 64 by driving a WS2812/SK6812 NeoPixel with a per-slot
colour. The C64 bootloader's slot selection drives the LED, so you can see at a
glance which kernal is running.

Tested on real hardware: **One ROM Fire 24 E (RP2350)**. The C64 boots and
switches kernals normally with the plugin installed.

## Status

**Working for normal use.** Flicker is fixed, **stock shows red**, and colours
are **steady and distinct per ROM**.

There is **one residual limitation**: while the C64 is hammering the ROM bus,
the CPU-bit-banged colour can be slightly hue-shifted on a couple of slots. It is
correct with the host idle, and **a power cycle always shows the true colour**.
Best results with **USB unplugged** (i.e. normal C64 use — USB is only needed for
programming). Full reasoning in [The investigation](#the-investigation).

| Kernal slot | Colour |
|---|---|
| bootloader / menu | off |
| stock 901227-03 | 🔴 red |
| JiffyDOS | 🟠 orange |
| JaffyDOS | 🟡 yellow |
| EXOS | 🟢 green |
| jiffy_dolphin | 🩵 cyan |
| destest | 🔵 blue |

## Wiring

The NeoPixel data line is driven from a **bank-select (SEL) pad**. These pads are
read by the firmware at boot for the image-select jumpers, then free as GPIO. On
the Fire 24 E: **SEL0 = GPIO25, SEL1 = GPIO24**.

| NeoPixel | One ROM |
|----------|---------|
| DIN | a SEL pad — the plugin drives **both GPIO24 and GPIO25** (see note) |
| VDD | J1 (3.3V) for an SK6812, or 5V for a WS2812B |
| GND | J2 / bank-select GND pad — **common ground is required** |

- **SK6812 @ 3.3V** needs no level shifter.
- **WS2812B @ 5V** (≈4.3V after the on-board protection diode — fine) needs a
  **74AHCT125** level shifter. AHCT specifically: its TTL input threshold
  recognises the 3.3V GPIO as a logic high.
- **Byte order:** this LED is **RGB**, not the usual WS2812 **GRB**. The driver
  sends RGB. If red/green are swapped on your pixel, flip the order in
  `np_send_pixel()`.
- **Why both pins?** We couldn't definitively confirm which SEL pad the data wire
  lands on, so the driver drives both; the unused one toggles harmlessly. Confirm
  yours and you can drop to a single `NEOPIXEL_PIN`.

## Brightness

`#define NP_BRIGHTNESS` (0–255) in `host_control_rgb_main.c` scales all colours.
Default `32` (~12%). The raw pixel is very bright; lower values are also slightly
kinder to the hue (shorter 1-bit runs → less cross-channel bleed under load).

## Build & flash

```bash
cd plugins/user/host-control
make                       # -> build/host_control_plugin.bin
```

```bash
onerom program \
  --plugin usb \
  --plugin file=host_control_plugin.bin \
  --slot file=c64_bootloader.bin,type=2364,cs1=active_low \
  --slot file=901227-03.rom,type=2364,cs1=active_low \
  ... (further kernal slots, in palette order) ...
```

> Use `--plugin file=...`. `--plugin host-control` by name pulls the **stock**
> RBCP plugin from the manifest, which has **no LED support**.

The palette is indexed by **flash slot excluding plugins**
(`ORA_FLASH_SLOT_FLAG_EXCLUDE_PLUGINS`): slot 0 = bootloader (off), slot 1 =
first kernal (red), etc. — see the table in `host_control_rgb_main.c`.

## Usage notes

- **Run with USB unplugged** for the best colours (USB-stack interrupts are the
  biggest corruptor — see below). The One ROM is powered through the ROM socket.
- **A power cycle always shows the true colour** for every slot.
- Brief colour tint right after a menu selection on some slots is the residual
  bus-contention artifact; it does not affect which kernal boots.

---

## The investigation

This began as "the LED flickers instead of changing colour" and became a deep
hardware/firmware dig. Recorded in full so it isn't lost.

### Fixed along the way
| # | Problem | Cause | Fix | Outcome |
|---|---------|-------|-----|---------|
| 1 | Flicker, never holds a colour | Pin tri-stated between updates; floating pad sat at ~1.5 V, right on the 74AHCT125 input threshold → shifter output oscillated | Drive the pin as a permanent output, idle **LOW** | ✅ Flicker gone |
| 2 | No stable colour; wired pad read ~1.5 V even when "driven" | Code drove GPIO24, but the data wire wasn't on the pin being driven; on Fire 24 E every GPIO is allocated except the SEL pads (24/25) | Drive **both** GPIO24 and GPIO25 | ✅ LED lit, steady |
| 3 | "Red" showed as green | LED is **RGB** order; code sent **GRB** | Send RGB order | ✅ Primaries correct in isolation |
| 4 | Colours scrambled under load | The per-bit `noinline` call + static reloads hit SRAM, which the ROM-serving DMA saturates → stretched bits | Rewrite bit-bang to be **register-only** (no SRAM in the timing loop) | ✅ Big improvement (some colours became correct) |
| 5 | LED far too bright | — | `NP_BRIGHTNESS` scale | ✅ Dimmable; also reduces colour bleed |

### Decisive diagnostics
- **Host powered OFF → perfect** red/green/blue/white. The bit-bang itself is
  flawless; the corruption is purely load-induced.
- **Force pure red for every update** → still came out orangy / white-in-menu.
  Proved it's **transmission corruption**, not the palette/mapping or byte order.
- **USB unplugged → stock is red, colours solid and distinct.** Proved
  **USB-plugin interrupts are the dominant corruptor.**
- **Menu-select tinted, but a power cycle shows the true colour.** Proved the
  *value* is right — it's the *moment* of transmission (bus activity) that's bad.

### Things tried that did **not** work (and why)
| Approach | Result | Why |
|----------|--------|-----|
| `cpsid` (disable interrupts) | No effect | Plugin runs **unprivileged**; `cpsid` is silently ignored |
| Firmware `ora_enable_irq` to mask USB IRQ | **Firmware panic** | Unprivileged plugin writing the NVIC faults |
| Exclusive mode (pause other core) | **Broke RBCP** | Can't pause the serving core mid-bus-cycle |
| Pause the RBCP address monitor (PIO0) during TX | No real change | The monitor isn't the main corruptor |
| Widen 0/1 pulse-timing margin | No change | Corruption isn't a threshold-margin issue |
| RAM-resident bit-bang | Abandoned | Would move code into the *contended* SRAM (wrong direction); flash/XIP is the uncontended path |
| PIO (hardware-timed) output | Not feasible from a plugin | All 3 PIO blocks dynamically allocated (PIO1 addr, PIO2 data, **PIO0 = the RBCP monitor**); no statically-free instruction space; no `apio` access |
| Re-send the colour after the session settles | Couldn't thread it | Too short = still tinted; long enough = **breaks RBCP** (the bootloader switches during menu navigation) |

### Root cause & conclusion
The colour is **bit-banged by the CPU**, and the timing is corrupted whenever
something preempts the CPU / saturates the bus during the ~30 µs transmission —
dominantly **USB-plugin interrupts**, with a milder residual from **ROM-serving
bus contention**. A plugin has **no lever** to stop either: it runs unprivileged
(so no interrupt masking, by any route), and it cannot claim a PIO state machine
(so no hardware timing). The only fully robust fix is **PIO output from inside a
custom One ROM firmware build**, where PIO allocation is owned and can be reserved
at init.

For real-world C64 use (USB unplugged) the indicator works: flicker-free, stock
red, steady distinct per-kernal colours, with a power cycle showing the exact
hue.
