# mad60boot (unmad-bootloader)

Part of **project Unmad**, fully open firmware for the Madlions MAD60 HE:
[unmad-firmware](https://github.com/thienanbeou/unmad-firmware/tree/unmad/keyboards/mad60he)
(start there: quick start, release files), this bootloader, and
[unmad-configurator](https://github.com/thienanbeou/unmad-configurator).
Installing this bootloader is optional; the firmware works with the vendor
one too.

Open bootloader for the Madlions MAD60 HE (STM32F405-class chip; the tested
board has a Geehy APM32F405). It replaces the vendor "MAD60 Bootloader" in
flash sector 0 (16 KB) and runs any firmware the vendor one runs: the stock
firmware and the libhmk port.

**Unofficial.** Not made or supported by Madlions or its manufacturer. It
reuses the vendor bootloader's USB ID 373B:2028 and its flash protocol only so
that existing tools (fwtool.py, the vendor's updater) keep working; the USB
strings say "Unmad (unofficial)" / "Unmad bootloader v5".

Release files: `unmad-bootloader-mad60he-v1.2.1.bin` is v5 (this source),
`unmad-bootloader-mad60he-v1.0.bin` (releases v1.0 to v1.2) is v3, without
the Esc rescue.

## What it does

- Power-on: starts the firmware at 0x08020000 straight from reset.
- The firmware asks for the bootloader (RAM flag 0x96969696 at 0x2001FFFC +
  reset or jump, as the stock firmware does): USB HID mode, 64-byte packets
  `AA 01` / `AA 02` / `AA 03` / `AA 96`, same as the vendor bootloader.
- Writes only the firmware area (sectors 5-7), reads back every word, writes
  the firmware's vector table last so a half-written firmware never starts.
- A replug starts the firmware again. Hold **Esc** while plugging in to stay
  in the bootloader instead (v4): read by the bootloader itself, so it works
  with any firmware, even one that never starts. A firmware started with
  `AA 96` (the end of every flash) starts even with Esc held.
- A command packet with a wrong checksum is answered as failed (v3 could
  answer it as written).
- Bus powered, no remote wakeup in its USB descriptor (v5; the vendor's
  claimed both).
- Known issue: on Windows, after a few idle seconds with no program holding
  it open, the first open fails and the bootloader re-enumerates (Windows
  suspends it and it does not resume cleanly). `fwtool.py` retries the open;
  an older `fwtool.py` works when run again.

## Build

PlatformIO (`platform = ststm32`, `framework = stm32cube` for CMSIS headers
and startup, TinyUSB 0.20.0):

    pio run
    arm-none-eabi-objcopy -O binary .pio/build/mad60boot/firmware.elf raw.bin

Emulator check (Unicorn, models the flash controller), kept with the other
MAD60 HE checks in unmad-firmware (`keyboards/mad60he/tests`):
`check_boot_emu.py firmware.elf APP_CONTAINER.bin [STOCK_CONTAINER.bin]`.
The protocol and the vendor bootloader's behaviour are described in that
fork's `keyboards/mad60he/NOTES.md`.

## Install and switching back

Needs the Unmad firmware (v1.0 or later) running, which has the install
commands, the release files `hmk_bootloader.py` and
`unmad-bootloader-mad60he-v1.2.1.bin` from the
[unmad-firmware releases](https://github.com/thienanbeou/unmad-firmware/releases),
Python 3 and `pip install hidapi`. Close the configurator tab first. Each
install saves what is in the bootloader sector now as
`sector0-before-install-<time>.bin` in the current folder, writes the new
one and reads it back.

| You want | Do this |
|---|---|
| stock bootloader -> Unmad bootloader | with the Unmad firmware running: `python hmk_bootloader.py install unmad-bootloader-mad60he-v1.2.1.bin --yes`. Keep the `sector0-before-install-<time>.bin` it saves: it is your stock bootloader |
| Unmad bootloader -> stock bootloader | with the Unmad firmware running: `python hmk_bootloader.py install sector0-before-install-<time>.bin --yes` (the file from your first install) |
| a newer Unmad bootloader | install the new file the same way. It saves your old Unmad bootloader too: keep the file from the **first** install, the only copy of the stock one |
| everything back to stock | first the stock bootloader (row 2), then the stock firmware (`fwtool.py`, see the firmware guide). Only the Unmad firmware can change the bootloader, so this order |
| which one do I have? | `python hmk_bootloader.py verify FILE` compares without writing |

Lost the stock bootloader file? Nothing breaks: the Unmad bootloader runs the
stock firmware too (switching firmware both ways still works); you just
cannot put the stock bootloader back. Madlions' bootloader is not ours to
publish.

Every install writes the bootloader sector. **Do not unplug while it runs:**
losing power in that second leaves the board without boot code, recoverable
only with an SWD programmer.

## License

GPL-3.0-or-later (see the header of `src/main.c`). TinyUSB is MIT, ST's CMSIS
files are Apache-2.0 / BSD-3-Clause.
