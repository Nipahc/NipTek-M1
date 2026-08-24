# NipTek M1 — changes from upstream Monstatek/M1

This file lists everything the NipTek flavor changes relative to the upstream
[Monstatek/M1](https://github.com/Monstatek/M1) firmware, so the differences are
always clear (and to honor the GPL-3.0 requirement of stating what was modified).

Base: upstream **v0.8.0.2**. The on-device firmware **version number is intentionally
unchanged** — it lives in a CRC-protected config block the bootloader uses for
update/bank decisions, so it is left alone until there is a reason to bump it.

---

## Branding

- **Power-on screen** (`m1_csrc/m1_system.c`, `startup_info_screen_display`): shows the
  Nipahc emblem centered on top, **"NipTek M1"** and **"Nipahc Technologies"** centered
  below, and the version top-right. Text is centered via `u8g2_GetStrWidth()` so it never
  clips. NOTE: the wordmark uses `M1_DISP_LARGE_FONT_1B` (profont17) because it has
  lowercase glyphs — the original `M1_POWERUP_LOGO_FONT` (tenthinnerguys_tu) is
  UPPERCASE-ONLY and rendered "NipTek" as "NT".
- **Power-on emblem** (`m1_csrc/m1_display_data.c`, `m1_logo_40x32`): the Nipahc emblem
  reduced to 1-bit, with a crisp drawn ring over the downscaled interior (a photo-scaled
  thin ring fragments at 40 px).
- **Welcome/idle screen** (`m1_csrc/m1_display_data.c`, `menu_m1_icon_M1_logo_1`, 128x64):
  Nipahc emblem + "NipTek" wordmark (this is the idle animation, not the boot screen).
- **Main-menu header** (`m1_csrc/m1_display.c`): "NipTek M1" text replaces the old M-mark
  + "M1". The old `m1_logo_26x14` bitmap is now unused.
- **About screen** (`m1_csrc/m1_settings.c`): company page shows `NipTek M1` /
  `Nipahc Technologies`, crediting `Base: MonstaTek Inc.`.
- **Build artifact name** (`CMakeLists.txt`): output is `NipTek_M1_v0800.*`.
- **Windows build helper** (`build_windows.ps1`): puts the ARM toolchain, CMake, Ninja,
  and srec_cat on PATH for a one-command build.

## Feature: Field Detector (Specter-style)

New menu item **NFC -> Field Detector** (`m1_csrc/m1_specter.c` / `.h`, registered in
`m1_csrc/m1_menu.c`, built via `cmake/m1_01/CMakeLists.txt`). Inspired by the Flipper
Zero "Specter" app.

- **Passive** — the M1 never transmits in this mode; it only listens for an external HF
  reader's carrier.
- Brought up via `NFC_Polling_Init()` (registers the ST25R3916 IRQ, enables the EN_EXT_5V
  rail, runs `rfalNfcInitialize`); `rfalNfcInitialize()` alone is not enough.
- The meter is the **hardware external-field detector** (`rfalIsExtFieldOn()`) sampled
  ~40x/frame and shown as a **duty-cycle %** — this tracks pulsed reader fields (e.g. a
  game console's amiibo reader). (An earlier amplitude-based approach was dropped because
  `rfalChipMeasureAmplitude()` barely moves on an external field — it rests ~53/255.)
- UI: big % number, bar gauge, a latched "FIELD DETECTED" banner, a running **Hits**
  counter, and **Peak %**. Buzzes on each new detection. BACK exits; LEFT clears peak.
- Tunables at the top of `m1_specter.c`: `SPECTER_EFD_SAMPLES`, `SPECTER_DUTY_THRESHOLD`,
  `SPECTER_DETECT_HOLD`, `SPECTER_LEVEL_DECAY`.

## Feature: Battery indicator

- **Main menu** (`m1_csrc/m1_display.c`, `draw_main_menu_battery`): a battery glyph
  (outline + proportional fill + percent below) with a charging bolt, in the free
  top-left area. Reads the cached `power_status` via `battery_power_status_get()` — no
  I2C on the draw path.

---

## Known issues / future work

### Amiibo emulation — Flipper file-format mismatch (investigated, not yet fixed)

The M1 **does** have a full Type-2 (NTAG/Ultralight) tag emulator — the listener
(`NFC/NFC_drv/legacy/nfc_listener.c`) answers READ / FAST_READ / GET_VERSION / WRITE from
a loaded dump (`ceT2T_FromDump`, `CeBuildT2TReadResp`; pages come from
`nfc_ctx_get_t2t_page`). So amiibo emulation is feasible in principle.

The reason a user's amiibo dumps don't emulate: they are **Flipper NFC format**, which the
M1 loader rejects before loading any data. In `NFC/NFC_drv/common/nfc_storage.c`:
- `isValidHeaderField(..., "M1 NFC device", "4", ...)` requires `Filetype: M1 NFC device`
  + `Version: 4`; Flipper files say `Filetype: Flipper NFC device` / `Version: 2`.
- device-type parsing requires `Ultralight/NTAG`; Flipper files say `NTAG215`.
- The per-line data loop only acts on `Page N:` / `Block N:` lines and ignores the rest,
  so Flipper's extra fields (Signature, Counter, Mifare version, Pages total) are harmless.
  The `Page N: xx xx xx xx` data lines are byte-for-byte compatible.

**Fix plan:** teach the loader to accept Flipper-format files — recognize
`Flipper NFC device` and map `NTAG215` / `NTAG216` / `Mifare Ultralight` to the Ultralight
family. Once the file loads, the existing emulator should serve the pages. Switch
acceptance (GET_VERSION handshake / amiibo signature) still needs on-hardware testing.

### Other

- Field Detector detection threshold / decay may want tuning per-unit after testing.
