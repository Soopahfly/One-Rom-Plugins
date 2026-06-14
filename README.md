# One-Rom-Plugins

A modified One ROM **host-control (RBCP)** plugin that indicates which kernal is
active on a Commodore 64 by driving a WS2812/SK6812 NeoPixel with a per-slot
colour. The C64 bootloader's slot selection drives the LED, so you can see at a
glance which kernal is running.

Tested on real hardware (One ROM **Fire 24 E**, RP2350): the C64 still boots and
switches kernals normally with the plugin installed.

> **Status:** the flicker problem is fixed and colours are steady and distinct
> per ROM. There is **one known, unresolved limitation** — colours are
> hue-shifted/washed-out while the C64 is actively driving the ROM bus (see
> [Known limitation](#known-limitation-colour-corruption-under-bus-load)). The
> indicator is fully correct when the host is idle.

## Wiring

The NeoPixel data line is driven from a **bank-select (SEL) pad**. These pads are
read by the firmware at boot to pick the image-select jumpers, and are then free
to use as GPIO. On the Fire 24 E:

| SEL pad | GPIO |
|---------|------|
| SEL0    | 25   |
| SEL1    | 24   |

| NeoPixel | One ROM |
|----------|---------|
| DIN      | a SEL pad (the plugin drives both GPIO24 and GPIO25 — see note) |
| VDD      | J1 (3.3V) for an SK6812, or 5V for a WS2812B |
| GND      | J2 / bank-select GND pad (**common ground is required**) |

- An **SK6812** powered from 3.3V needs no level shifter.
- A **WS2812B** runs from the One ROM 5V rail (≈4.3V after the on-board
  protection diode — fine for the LED) and needs a **74AHCT125** level shifter on
  the data line. AHCT is required (not HCT/HC): its TTL input threshold reliably
  recognises the 3.3V GPIO as a logic high.
- **Byte order:** the LED used here is **RGB**, not the usual WS2812 **GRB**. The
  driver sends RGB. If your pixel comes out with red/green swapped, flip the
  order in `np_send_pixel()`.

> **Why drive two pins?** During debugging we could not definitively confirm
> which physical SEL pad the data wire lands on, so the plugin drives **both**
> GPIO24 and GPIO25 with the same data. The unused one toggles harmlessly. If you
> confirm your pad, you can drop the other and keep a single `NEOPIXEL_PIN`.

## Building

Built against the One ROM SDK (`sdrr`) as a user plugin:

```bash
cd plugins/user/host-control
make          # -> build/host_control_plugin.bin
```

Flash alongside the `usb` system plugin, the C64 bootloader, and your kernals:

```bash
onerom program \
  --plugin usb \
  --plugin file=host_control_plugin.bin \
  --slot file=c64_bootloader.bin,type=2364,cs1=active_low \
  --slot file=901227-03.rom,type=2364,cs1=active_low \
  ... (further kernal slots) ...
```

> Use `--plugin file=...` for **this** build. `--plugin host-control` by name
> pulls the *stock* RBCP plugin from the manifest, which has **no** LED support.

The colour palette is indexed by **flash slot, excluding plugin slots**
(`ORA_FLASH_SLOT_FLAG_EXCLUDE_PLUGINS`): slot 0 = bootloader (off), slot 1 =
first kernal (red), and so on — see the palette table in
`host_control_rgb_main.c`.

---

## Debugging journey

This started as "the LED flickers instead of changing colour" and turned into a
long hardware/firmware investigation. Recording it here so the next person
doesn't repeat it.

### 1. Flicker — *fixed*
- **Symptom:** LED flickered, never held a colour.
- **Cause:** the code tri-stated the GPIO between updates. Floating, the pad sat
  at ~1.5 V — right on the 74AHCT125's input threshold — so the shifter output
  oscillated into the LED.
- **Fix:** drive the pin as a permanent output, idle **LOW**, so the shifter
  input always has a defined level. ✅ Flicker gone.

### 2. Wrong GPIO / wrong pad — *fixed*
- **Symptom:** after the flicker fix, still no stable colour; multimeter showed
  the wired pad at ~1.5 V even while "driven".
- **Cause:** the original code drove **GPIO24** and called it "pad A", but the
  data wire wasn't on the pin being driven. On the Fire 24 E the SEL pads are
  GPIO25 (SEL0) and GPIO24 (SEL1), and every GPIO 0–29 is otherwise allocated
  (data/addr/CS/USB/SWD/status-LED), so a SEL pad is the only sensible choice.
- **Fix:** drive **both** GPIO24 and GPIO25. ✅ LED lit, steady, no flicker.

### 3. Byte order — *fixed*
- **Symptom:** "red" rendered as green.
- **Cause:** this LED is **RGB**, the code sent **GRB**.
- **Fix:** send RGB order. ✅ Primary colours map correctly in isolation.

### 4. Single-shot vs interrupts — *partial*
- Continuous-refresh drivers (e.g. the stock `rgb` plugin) hide frame
  corruption because they redraw constantly. This plugin sends the colour
  **once** per switch, so a single corrupted transmission sticks. Added
  `cpsid`/`cpsie` around the transmission to keep ISRs out of the timing.

### 5. SRAM-bus contention — *big improvement*
- **Symptom:** under load the colour scrambled.
- **Insight:** the transmission is **perfect with the C64 powered off** and
  corrupt with it running — i.e. it's bus contention from the ROM-serving DMA,
  not logic.
- **Fix:** rewrote the bit-bang to touch **no SRAM** — preload pin mask + timings
  into registers, inline the per-bit code, use only register ops + single-cycle
  SIO writes in the loop. ✅ Noticeably better (some colours became correct).

### Things tried that did **not** work
- **Exclusive mode** (pause the other core during the transmission): hung /
  broke RBCP (`get flash slot info failed`). The serving core can't be paused
  mid-bus-cycle without crashing the host.
- **Widening the 0/1 pulse-timing margin:** no change — proving the residual
  error isn't a simple threshold-margin issue.
- **RAM-resident bit-bang:** abandoned — it would move instruction fetch *into*
  the contended SRAM, which is the wrong direction (flash/XIP is the
  *uncontended* path here), and the plugin's RAM budget is tiny.
- **PIO output:** not achievable from a plugin here. All three PIO blocks are
  used (PIO1 = address, PIO2 = data, **PIO0 = the RBCP address monitor**), the
  serving/monitor programs are built **dynamically** so there's no
  statically-knowable free instruction space, and the `apio` tooling isn't
  exposed to plugins. Forcing it blind risks breaking C64 boot.

### Decisive diagnostic
Forcing the LED to **pure red for every update** (bypassing the palette
entirely) still produced orangy-yellow on a switch and **white** in the boot
menu. Same data in, different colour out depending on bus activity — this
**proves** the remaining fault is transmission corruption under load, not the
palette/mapping, byte order, or timing margin.

## Known limitation: colour corruption under bus load

The colour is **bit-banged by the CPU**. While the C64 is actively reading the
ROM, the One ROM's serving engine saturates the internal bus and stretches the
bit-bang's pulse timing by tens of nanoseconds. Because the C64 reads on a fixed
~1 MHz cadence, this is **phase-locked and deterministic** — so colours are
*steady* but hue-shifted, and the heavier the bus activity at the instant of the
update, the worse it gets (a kernal switch = mild tint; the boot menu, which
hammers the ROM hardest = near-white).

Everything else is confirmed correct: wiring, pin, byte order, and the bit-bang
itself (flawless with the host idle).

**The only robust fix is hardware-timed (PIO) output**, which is immune to bus
contention — but as noted above, a *plugin* cannot safely claim a PIO state
machine on this firmware. Driving the NeoPixel from **PIO inside a custom One ROM
firmware build** (where PIO allocation is owned and can be reserved at init)
would solve it properly; that's the open path if perfect hues are required.
