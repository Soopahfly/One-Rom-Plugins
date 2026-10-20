# One ROM 0.8.0 RGB candidate

Hardware feedback (20 October 2026): firmware 0.8.0 with r107sl's menu 0.1.2
and friendly labels was flashed with verification. User reports the colours
work perfectly. Orange JiffyDOS and yellow JaffyDOS were too similar, so the
current port changes JaffyDOS to magenta (equal red and blue). Purple was
considered too close to DesTest's blue. All other active colours are retained.
The magenta image has not yet been hardware-tested.

`make_jaffy_magenta.py` creates the magenta variant from the verified F8B062...
menu-0.1.2 image plus the rebuilt plugin. It checks that exactly two palette
bytes differ and retains the metadata, ROMs and settings page byte for byte.

Target: the existing Fire 24 E RP2350, 2MB, C64 2364 setup.

This is a hardware-test candidate, not a hardware-verified release.

## Inputs

- One ROM v0.8.0 at 3d8249b1e209a7b9353ced05bf3bd93b454f73c1.
- RGB reference at 09c2293e686affbbdbd74f086faa252ba3a8a536.
- CLI v0.5.0; pinned USB plugin v0.3.2.
- ARM GNU compiler 15.2.1 from the existing Pico SDK.
- Working full backup SHA256:
  48B89CD73CD025EEEEF947A47A5A3071EFC5D041E3448E73DDC57D9B174D8189.

All seven local ROM files were decoded from the working backup and matched
byte for byte, including the existing C64 bootloader. The installed plugin
matches the local binary except for NV byte 0 (saved selection 5).

## Changes

The base is the current host-control v0.1.6 with its RBCP 0.1.3 implementation,
not the old command decoder. Custom plugin version is 0.1.6 build 1, requiring
firmware 0.8.0. Preserves both SEL outputs (GPIO24 and GPIO25), RGB byte order,
brightness 32/256, and palette. The existing level shifter remains in place.

The old code paused hard-coded PIO0 state machines and attempted to mask IRQs.
Those operations are removed: 0.8.0 allocates the address monitor in the serving
PIO blocks, and unprivileged IRQ masking cannot be relied on. The SRAM sender
is retained; hardware must verify colour timing, especially with USB connected.

RAM-slot identities are tracked for all 64 possible 8KB C64 slots, rather than
eight. The actual count is lower because firmware also occupies RAM. This
extension is specifically for 8KB 2364 C64 images; it is not a generic plugin for
other ROM sizes. Unknown identities show dim white.

The capture ring is reduced from upstream's 64 entries (old RGB: 32) to 16 to
retain the original 420-byte static-memory ceiling. Compiled static placement
uses 404 bytes, leaving 620 bytes of the 1KB plugin area for the stack. The
SRAM sender is optimized for size and uses 100 bytes. Neither linker RAM nor
stack limits were relaxed. The reduced ring and changed timing need hardware
testing; compilation alone does not establish reliability.

LOAD_SLOT, SWITCH_SLOT, SWITCH_AND_EXIT and LOAD_AND_EXIT update colour tracking.
Upstream auxiliary commands that switch slots directly are not LED-hooked;
the existing bootloader does not use those commands.

## Build

The local preparation scripts expect this layout:

```text
programming-folder/
  onerom-new.exe
  onerom-working-0.6.13-full-backup.bin
  [the seven original ROM files]
  upgrade-work/
    One-Rom-Plugins/
    one-rom-v0.8.0/  (checkout of upstream tag v0.8.0)
```

`build_v080.ps1` uses ARM GNU tools on PATH, or the current user's Pico SDK
15_2_Rel1 toolchain. An explicit location can be supplied with
`-ToolchainDirectory`. ROM files and full firmware images are not distributed.

From the programming folder:

```powershell
python .\upgrade-work\One-Rom-Plugins\port_v080.py
& .\upgrade-work\One-Rom-Plugins\build_v080.ps1
& .\upgrade-work\One-Rom-Plugins\prepare_candidate_v080.ps1 -UpdateMenu
```

The last step requires internet to download released firmware and the USB
plugin. `-UpdateMenu` downloads r107sl's 8KB menu 0.1.2 and checks its published
SHA256. All entries receive friendly labels. It builds a complete candidate on
disk, preserving the working plugin's
4KB NV page. No script flashes, reboots, commissions or changes OTP.

## Hardware acceptance

After reviewing and programming the candidate, test:

1. C= boot menu and steady white LED; all six KERNAL entries selectable (the
   seventh ROM slot is the menu itself and is hidden).
2. Stock red, JiffyDOS orange, JaffyDOS magenta, EXOS green, Jiffy/Dolphin cyan,
   DesTest blue. Test each ROM actually starts, not just its LED.
3. Repeated changes and cold starts, both with and without USB connected.
4. Save a selection and verify it survives power off. Existing saved selection
   should remain Jiffy/Dolphin. Test normal keyboard and disk access.

The menu has been observed running on firmware 0.8.0. The broader acceptance
checks above should be recorded separately; a working menu and correct colours
alone do not establish every disk, cartridge or power-cycle scenario.

## Rollback

The verified original full-flash backup is retained in the programming folder.
The following is a manual recovery command, not run by these scripts:

```powershell
.\onerom-new.exe --serial 69A27E2CABC8CE00 program --firmware .\onerom-working-0.6.13-full-backup.bin --verify
```

If normal USB access stops working, BOOTSEL recovery may be required before
the backup can be restored. Do not replace the original backup with a new dump.
