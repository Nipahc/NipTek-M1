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

## Feature: Specter passive field detector

New menu item **NFC -> Field Detector** (`m1_csrc/m1_specter.c` / `.h`,
registered in `m1_csrc/m1_menu.c`, built via `cmake/m1_01/CMakeLists.txt`).

- Passive 13.56 MHz field sweeper inspired by the Flipper Zero "Specter" app. The
  M1 never transmits in this mode -- it only listens for an external reader's carrier.
- Uses `rfalChipMeasureAmplitude()` for an analog strength gauge (0-255) and
  `rfalIsExtFieldOn()` for the ST25R3916 hardware external-field detector.
- UI: live numeric value, bar gauge, "FIELD DETECTED" banner, peak-hold. BACK exits,
  LEFT clears the peak. Buzzes once on each new detection.
- Costs ~1.3 KB flash. Detection threshold and smoothing are `#define`s at the top of
  `m1_specter.c` for easy tuning.

## Planned

- NipTek boot/main-screen logo swap (`m1_logo_26x14`, `menu_m1_icon_M1_logo_1`).
