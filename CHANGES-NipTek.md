# NipTek M1 — changes from upstream Monstatek/M1

This file lists everything the NipTek flavor changes relative to the upstream
[Monstatek/M1](https://github.com/Monstatek/M1) firmware, so the differences are
always clear (and to honor the GPL-3.0 requirement of stating what was modified).

## v0800 branding (based on upstream v0.8.0.2)

- **About screen** (`m1_csrc/m1_settings.c`): the company page now shows
  `NipTek M1` / `Nipahc Technologies`, with `Base: MonstaTek Inc.` credited below.
- **Build artifact name** (`CMakeLists.txt`): output binaries are named
  `NipTek_M1_v0800.*` instead of `MonstaTek_M1_v0800.*`.
- **Windows build helper** (`build_windows.ps1`): a convenience script that puts the
  ARM toolchain, CMake, Ninja, and srec_cat on PATH for a one-command build.

The on-device firmware **version number is intentionally unchanged** (0.8.0.2). It is
stored in a CRC-protected config block the bootloader uses for update/bank decisions,
so it is left alone until there is a reason to bump it.

## Planned

- Port of a passive 13.56 MHz field-detector tool (Specter-style) using the M1's
  ST25R3916 / RFAL amplitude measurement.
