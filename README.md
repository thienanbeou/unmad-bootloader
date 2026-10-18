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
strings say "Unmad (unofficial)" / "Unmad bootloader".

## What it does

- Power-on: starts the firmware at 0x08020000 straight from reset.
- The firmware asks for the bootloader (RAM flag 0x96969696 at 0x2001FFFC +
  reset or jump, as the stock firmware does): USB HID mode, 64-byte packets
  `AA 01` / `AA 02` / `AA 03` / `AA 96`, same as the vendor bootloader.
- Writes only the firmware area (sectors 5-7), reads back every word, writes
  the firmware's vector table last so a half-written firmware never starts.
- A replug always starts the firmware again.

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

## Install

With the Unmad firmware (v1.0 or later) running, which has the install
commands, and the release files `hmk_bootloader.py` and
`unmad-bootloader-mad60he-v1.0.bin` (from the
[unmad-firmware releases](https://github.com/thienanbeou/unmad-firmware/releases);
needs Python 3 and `pip install hidapi`):

    python hmk_bootloader.py install unmad-bootloader-mad60he-v1.0.bin --yes

It first saves your current bootloader (the vendor one) as
`sector0-before-install-<time>.bin` in the current folder, then writes the
new one and reads it back. Keep that file: installing it the same way is how
you go back to the vendor bootloader.

**Risk:** losing power during the install (about a second) leaves the board
without boot code; recovering then needs SWD. Do not unplug while it runs.

## License

GPL-3.0-or-later (see the header of `src/main.c`). TinyUSB is MIT, ST's CMSIS
files are Apache-2.0 / BSD-3-Clause.
