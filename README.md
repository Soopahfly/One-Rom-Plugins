# One-Rom-Plugins

A modified One ROM **host-control (RBCP)** plugin that indicates which kernal is
active on a Commodore 64 by driving a WS2812/SK6812 NeoPixel with a per-slot
colour. The C64 bootloader's slot selection drives the LED, so you can see at a
glance which kernal is running.

Tested on real hardware: **One ROM Fire 24 E (RP2350)**. The C64 boots and
switches kernals normally with the plugin installed.

## Status

**Working as intended.** Flicker is fixed, the stock kernal shows red, the boot
menu/power state shows steady white, and the selected kernal colours are stable
and distinct.

The key fix for the remaining hue-shift problem was moving the timing-critical
NeoPixel sender into `.ramfunc`, so the 24-bit waveform runs from SRAM instead
of shared XIP flash. This avoids stalls from the system plugin running on the
other core while the user plugin bit-bangs the LED.

| Flash slot | Meaning | LED colour |
|---|---|---|
| 0 | bootloader / menu / power indicator | white |
| 1 | stock 901227-03 | red |
| 2 | JiffyDOS | orange |
| 3 | JaffyDOS | yellow |
| 4 | EXOS | green |
| 5 | jiffy_dolphin | cyan |
| 6 | destest | blue |
| 8 | additional kernal | rose |

## Wiring

The NeoPixel data line is driven from a **bank-select (SEL) pad**. These pads are
read by the firmware at boot for the image-select jumpers, then free as GPIO. On
the Fire 24 E: **SEL0 = GPIO25, SEL1 = GPIO24**.

| NeoPixel | One ROM |
|----------|---------|
| DIN | a SEL pad; this plugin drives both GPIO24 and GPIO25 |
| VDD | J1 (3.3V) for an SK6812, or 5V for a WS2812B |
| GND | J2 / bank-select GND pad; common ground is required |

- **SK6812 at 3.3V** needs no level shifter.
- **WS2812B at 5V** needs a 74AHCT125 level shifter on the data line.
- **Byte order:** this LED is RGB, not the usual WS2812 GRB. If red/green are
  swapped on your pixel, change the packing in `np_send_pixel()`.
- **Why both pins?** During bring-up we could not definitively confirm which SEL
  pad the data wire landed on, so the driver drives both. The unused one toggles
  harmlessly. If you confirm your pad, you can drop to a single pin.

## Brightness

`#define NP_BRIGHTNESS` in `host_control_rgb_main.c` scales all colours.
Default is `32`, roughly 12% brightness. This keeps the replacement power LED
comfortable and also reduces visible colour bleed.

## Build & flash

Build against the One ROM SDK host-control plugin harness:

```bash
cd plugins/user/host-control
make
```

Flash with this plugin binary by file:

```bash
onerom program \
  --plugin usb \
  --plugin file=host_control_plugin.bin \
  --slot file=c64_bootloader.bin,type=2364,cs1=active_low \
  --slot file=901227-03.rom,type=2364,cs1=active_low \
  --slot file=JiffyDOS_C64_6.01.bin,type=2364,cs1=active_low \
  --slot file=jaffydos.bin,type=2364,cs1=active_low \
  --slot file=EXOS_V3.rom,type=2364,cs1=active_low \
  --slot file=jiffy_dolphin.bin,type=2364,cs1=active_low \
  --slot file=destest-kernal.rom,type=2364,cs1=active_low \
  --yes
```

Use `--plugin file=...`. `--plugin host-control` by name pulls the stock RBCP
plugin from the manifest, which has no LED support.

The palette is indexed by **flash slot excluding plugins**
(`ORA_FLASH_SLOT_FLAG_EXCLUDE_PLUGINS`): slot 0 is the bootloader/menu, slot 1
is the first kernal, and so on.

## Investigation notes

This started as an LED flicker problem and became a timing investigation. The
important findings:

| Problem | Cause | Fix |
|---|---|---|
| LED flickered and never held colour | GPIO was left floating between updates; the level shifter input sat near its threshold | Keep the pin as a permanent output and idle it low |
| No colour on the wired pad | The data wire/pad mapping was uncertain | Drive both SEL pads, GPIO24 and GPIO25 |
| Red appeared as green | The LED used RGB byte order, not GRB | Send RGB order |
| Colours were hue-shifted under real use | CPU bit-bang fetched instructions from shared XIP flash while the other core/system plugin could also execute from flash | Put `np_send_pixel()` in `.ramfunc` and copy it to SRAM at startup |
| LED was too bright | Raw NeoPixel output is excessive as a power LED | Add `NP_BRIGHTNESS` scaling |
| Boot menu needed a visible power state | Slot 0 was originally off | Use steady white for flash slot 0 |

Things deliberately not kept:

- A smooth boot-menu colour cycle was tested, but custom polling of the RBCP
  ring buffer made the final colour nondeterministic during boot handoff. The
  stable version uses the firmware's blocking `wait_for_knock()` path and a
  steady white boot/menu colour.
- PIO output would still be the most robust architectural solution, but it is
  not safely claimable from this user plugin because the firmware owns dynamic
  PIO allocation.

## Final behaviour

- Bootloader/menu/power state: steady white.
- Stock kernal: red.
- Other kernals: palette colours by flash slot.
- NeoPixel waveform: sent from SRAM via `.ramfunc`.
- RBCP command handling: original blocking firmware knock wait, no custom ring
  polling.
