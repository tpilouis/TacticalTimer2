# TacticalTimer2 — Release Note v2.1.0

**Release Date:** 2026-04-06
**Hardware:** M5Stack Core2 (ESP32)
**IDE:** Arduino IDE 1.8.19 / ESP32 Core 2.0.17

---

## 🔧 Core2 Display — Bug Fixes

### Badge / Status Indicator Ghost Artifact — Resolved
- `drawModeHeader()` clear height extended from `MODE_HDR_H (18px)` to `MODE_HDR_H + 2px (20px)`, covering FSS9 font descender overflow
- Badge width is now dynamically calculated (`textWidth + 14`, max 90px) so long labels such as `SHOW CLEAR` no longer overflow the badge boundary
- `drawString()` changed to foreground-only (no background fill) to prevent bounding-box bleed outside the badge area
- `fillRoundRect` replaced with `fillRect` to eliminate corner pixel residue
- Affected states: `READY / WAITING / FIRE! / SHOW CLEAR / STOP / RESULT / STANDBY` across FREE, DRILL, RO modes

### Shot Page Indicator Overlap — Removed
- The orange `↑#08` page indicator was rendered at `SHOT_START_Y`, overlapping the SPLIT column of the first row on page 2+
- Indicator removed from both `drawShotRow()` (live modes) and `drawHistoryDetail()` (History detail view)
- Navigation is conveyed by BtnA `< PREV` / BtnC `NEXT >` button labels

### New Game Showing Previous Results — Resolved
- `onEnter()` for FREE, DRILL, and RO modes now calls `TimerCore::abortSilent()` followed by `UIScreen::resetShotPage()`
- `resetShotPage()` issues a `fillRect` over the full shot area before resetting the page pointer, ensuring a clean slate

### Double Beep on New Game — Resolved
- `TimerCore::abortSilent()` now calls `xQueueReset()` on both `audioQueue` and `audioDoneQueue`
- The state machine jumps directly to `IDLE` without transitioning through `SHOWCLEAR`, eliminating the spurious second beep

### NVS Preset THRESH Read Error — Resolved
- `loadPreset()` / `savePreset()` migrated from `putBytes` / `getBytes` (struct binary dump) to field-by-field NVS access (`putString`, `putShort`, `putUChar`, `putULong`)
- Eliminates struct alignment-induced corruption that caused THRESH to read back as 0 or garbage
- `FMT_VER_VAL` bumped from `1` → `2` to force a one-time NVS wipe on first boot

---

## 📱 Core2 Display — Improvements

### Unified Single-Column Shot Log with Pagination
- All five game modes (FREE, DRILL, RO, SPY, HISTORY) now render shot results in a single-column, 7-rows-per-page layout
- Row height: `SHOT_ROW_H = 16px`; page capacity: `ROWS_PER_PAGE_UI = 7` (112px ÷ 16px)
- Page advances on whole-page boundaries: `pageBase = ((hitIdx−1) / 7) × 7 + 1`
- BtnA `< PREV` / BtnC `NEXT >` for manual browsing; auto-advance only when the current page is full

### DRILL Review Mode
- `resetShotPage()` now called only on first entry into REVIEW, not on every re-render
- `setTotalShotUI()` called with the actual hit count so PREV/NEXT work correctly across all pages

### SPY Mode
- `drawSpyScreen()` migrated to shared `SHOT_START_Y` and `SHOT_ROW_H` constants
- Auto page-advance gated on `isNewShot` flag; manual PREV no longer gets overridden immediately

---

## 🌐 Web Dashboard — Bug Fixes

### Shot Log Header / Row Misalignment — Resolved
- SETTINGS panel `.srow` class renamed to `.set-row`, eliminating a CSS conflict that caused the shot log's `display:grid` to be overridden by `display:flex`

### Scrollbar Shift Misaligning SPLIT Column — Resolved
- `scrollbar-gutter: stable` applied to both `.srows` and `.shdr`
- Header and scroll container now always reserve the same gutter width regardless of scrollbar visibility

### History Detail Repeating Header Every 5 Rows — Resolved
- History detail shot log migrated from dual-column layout (`idx ≤ 5` → left, `idx > 6` → right, each with its own `<shdr>`) to single-column infinite scroll with one header bar

---

## 🌐 Web Dashboard — New Features & Improvements

### Shot Log Layout Overhaul
- Single-column layout with `column-gap: 20px` for clear visual separation between `#`, `SRC`, `TIME`, `SPLIT`
- Column widths: `40px | 64px | 1fr | 88px`
- Header labels enlarged from 9px → 11px with `font-weight: 600`; `SPL` renamed to `SPLIT`
- `SRC` badge and `#` number both flex-centered to align with header labels
- `TIME` value 19px → 20px, `SPLIT` value 13px → 14px

### History Batch Delete
- Per-row `DEL` button removed; replaced with a single toolbar above the list
- `☐ SELECT ALL` checkbox: selects / deselects all currently visible rows
- `🗑 DELETE (N)` button: disabled (grey) when no rows are selected; turns red when ≥ 1 row is checked; shows confirm dialog before deletion
- Individual row checkboxes: tap to toggle; checked rows highlighted with blue outline
- Category filter tags `FREE / DRILL / RO` perform filter only (no auto-select)
- SPY removed from History filter (SPY sessions are not saved to SD card by design)

### SYSTEM Tab — Power & System Console
- New panel in SYSTEM tab: **⚡ POWER & SYSTEM LOG**
- Real-time gauges: Battery voltage + estimated %, charge/discharge current, USB VBUS voltage, AXP192 die temperature, uptime
- SSE events `power` and `log` push updates every 60 seconds
- Log window: monospace terminal style, dark green background (`#0a1a0f`)
- Log text colour-coded: 🟢 Normal `#4ade80` · 🟡 Low `#fbbf24` · 🔴 Warning `#f87171`
- Timestamps shown in grey; `CLEAR` button to wipe log

### Miscellaneous UI Polish
- `Loading…` / empty-state text in History: font 10px → 13px, colour `var(--dim)` → `var(--cyan2)`

---

## ⚙️ Boot Diagnostics (Serial + Web)

AXP192 power diagnostics are now printed on every boot and streamed to the Web Power Console:

```
[Power] ===== AXP192 Boot Diagnostics =====
[Power] Battery  : 3.812 V  (67%)
[Power] Current  : -245.3 mA  (Charging)
[Power] VBUS     : 5.021 V / 412.0 mA
[Power] APS      : 3.807 V
[Power] AXP Temp : 38.4 C
[Power] =========================================
```

Low-battery warning printed if `V < 3.3 V` and not charging.
Loop broadcasts power data every 60 s via SSE to the Web Console.

---

## 📁 Changed Files

| File | Change |
|------|--------|
| `config.h` | Version bump 2.0.0 → 2.1.0; `FMT_VER_VAL` 1 → 2 |
| `ui_screen.h / .cpp` | Badge fix; page indicator removed; single-column pagination |
| `timer_core.h / .cpp` | `abortSilent()` queue reset; direct IDLE transition |
| `hal_storage.cpp` | NVS field-by-field read/write |
| `mode_free.h / .cpp` | `onEnter` abort+reset; `redrawFullPage` static fix |
| `mode_drill.h / .cpp` | `onEnter` abort+reset; REVIEW page fix |
| `mode_ro.h / .cpp` | `roUpdateUI` cleanup; badge residue fix |
| `mode_spy.cpp` | Shared layout constants; `isNewShot` guard |
| `mode_history.cpp` | Single-column detail view |
| `web_server.h / .cpp` | Batch delete; Power Console; layout fixes; `sendLog()` / `sendPower()` |
| `TacticalTimer2.ino` | Boot diagnostics; loop power monitor; version 2.1.0 |

---

## 🔄 Upgrade Notes

1. After flashing, the device will detect `FMT_VER = 1` in NVS and **automatically reset all presets to factory defaults** on first boot — this is expected and required to clear the old binary-format presets.
2. Re-enter your Preset names / THRESH values in SETTINGS after the first boot.
3. No SD card data is affected by this upgrade.
