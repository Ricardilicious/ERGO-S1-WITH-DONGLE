# Ergo S1 — ZMK config (v0.3)

Handwired split, 34 keys per half, two nice!nano v2 (no displays),
plus a third nice!nano as the central ZMK Studio dongle with a 128x32 SSD1306 OLED.

## Wiring

Each half is a 6-row x 6-column matrix, `diode-direction = "col2row"`, one diode per switch,
band toward the row wire. Columns are enamel wire; rows are the bent diode legs.
Pins are identical on both halves:

| Signal | R0 | R1 | R2 | R3 | R4 | R5 |
|---|---|---|---|---|---|---|
| pro_micro | 21 | 20 | 19 | 18 | 15 | 14 |

| Signal | C0 | C1 | C2 | C3 | C4 | C5 |
|---|---|---|---|---|---|---|
| pro_micro | 4 | 5 | 6 | 7 | 8 | 9 |

- C0 is always the outermost column.
- Pins 0, 1, 2, 3, 10, 16 are spare (no nice!view).
- R4 (bottom row) uses C1–C4 only; R4 C0 and R4 C5 are empty.
- R5 is the thumb cluster with its own row wire and column jumpers.

**Left thumb cluster:** C0 Fn, C1 Ctrl, C2 Home, C3 Alt, C4 End, C5 Shift.
Transform order: `RC(5,5) RC(5,0) RC(5,1) RC(5,3) RC(5,2) RC(5,4)` (Shift, Fn, Ctrl, Alt, Home, End).

**Right thumb cluster (open item):** physical mirror of the left (Space mirrors Shift, Enter mirrors Fn,
Ctrl mirrors Ctrl, Caps Lock mirrors Alt, Page Up mirrors Home, Page Down mirrors End). The right overlay
lists the column GPIOs reversed (9→4) with `col-offset = <6>`, so right-half column Cn is global column `11 - n`.
The transform currently *assumes* the same jumpers as the left — C0 Enter, C1 Ctrl, C2 Page Up, C3 Caps Lock,
C4 Page Down, C5 Space — giving `RC(5,7) RC(5,9) RC(5,8) RC(5,10) RC(5,11) RC(5,6)`
(Page Down, Page Up, Caps Lock, Ctrl, Enter, Space).
TODO once the right half is wired: record which thumb key landed on which column here, then update the last
`map` line in `boards/shields/ergo_s1/ergo_s1-layouts.dtsi` (key → `RC(5, 11 - n)`).

## Dongle OLED

128x32 SSD1306 (I2C, 0x3C) on the dongle nice!nano: SDA → D2, SCL → D3, VCC → 3V3 (VCC), GND → GND.
For a 128x64 panel, edit `ergo_s1_dongle.overlay` (height 64, multiplex-ratio 63, remove `com-sequential`).

The dongle uses a custom status screen (`src/dongle_status_screen.c`, 128x32):

```
[tiny  ]  L 85%        R 72%
[keybd ]  USB  42 wpm   Base
```

- Top row: left and right half batteries (`--` when a half isn't connected).
- Bottom row: a small USB / Bluetooth-profile icon, words per minute, and the active layer.
- Left: an original pixel keyboard whose keys tap faster as WPM rises, idle when you stop.
- The first key press on each half tells the screen which connection is left and which is right.

## Builds (`build.yaml`)

Dongle mode (default):

| Artifact | Flash to |
|---|---|
| `ergo_s1_dongle` | dongle nice!nano (central, ZMK Studio over USB) |
| `ergo_s1_left_peripheral` | left half |
| `ergo_s1_right` | right half |
| `settings_reset` | flash to every board first when switching modes |

Standalone mode: swap in the commented `ergo_s1_left_central` block; the right half firmware is the same.

## ZMK Studio

Studio runs on whichever board is central (the dongle, or the left half in standalone mode), over USB.
Studio locking is turned off on the dongle, so it opens without pressing Fn + Esc. Three reserved layers are available for adding layers from Studio.

## Keymap

`config/ergo_s1.keymap` — Base and Fn layers taken from `layout_reference.png`
(Fn: F1–F10, Print Screen, Insert, volume, and a numpad on the right).
