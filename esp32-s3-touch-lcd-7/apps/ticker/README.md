# ticker — **built** (first flash 2026-09-16, unverified by eye)

The CYD ticker, ported to the 7" 800×480 panel with everything it learned:
the same fetch path (Yahoo, CoinGecko, HTTP/1.0 with the body read by hand),
sessions and holidays, the four row kinds and sections, the display
currency, search and add, remove by long press, headlines, the heatmap, the
Wi-Fi setup portal with no `secrets.h`, deep sleep with a snapshot, the
settings in NVS. Same gestures, same touch-down feel. The differences are
the board's, and they are all in one place:

```sh
python3 tools/make-logos.py --size 32 --out data/logo/32     # once, on the Mac
python3 tools/make-logos.py --size 128 --out data/logo/128
pio run -e ticker -t upload
./push-config ticker                                          # config + logos
```

## Previews

Wireframes at the panel's 800×480, drawn from the firmware's layout table
(2026-09-17). No logos in them: the badges are the initial tiles the firmware
draws for a symbol without a logo file.

| | |
|---|---|
| [preview.svg](preview.svg) | The list: tabs, icons, nine rows with badge (a flag for a currency, none for an index), sparkline, price, percent |
| [preview-splash.svg](preview-splash.svg) | Boot: TICKER over three bands of symbol chips |
| [preview-detail.svg](preview-detail.svg) | A stock's page, two columns: price and ranges left, chart, range chips and headlines right |
| [preview-settings.svg](preview-settings.svg) | Settings: label, value in grey, chevron; every row opens a page |
| [preview-themes.svg](preview-themes.svg) | Themes: twelve tiles, each in its own colours, the chosen one outlined |
| [preview-orientation.svg](preview-orientation.svg) | Orientation: four miniatures of the list, the cable edge marked |

## What changed for this board

| CYD | 7" | Why |
|---|---|---|
| ILI9341 over SPI, hardware scroll | RGB panel, our framebuffer in PSRAM, scrolled at scan-out | No scroll register, so the panel driver's bounce-buffer callback reads the list band through a ring offset: moving the list is writing one number. The CYD's ring, slot for slot. |
| Resistive touch, calibration page | GT911 capacitive, no calibration | Coordinates are panel pixels. The chip reports every ~10ms while a finger is down and once when it lifts; the board layer keeps the last report between them so the gesture code sees a steady state. |
| LDR, backlight levels | On/off through the CH422G expander | This board has no PWM on the backlight and no light sensor. The Backlight row is gone. |
| Speaker chime, LED glow | A banner across the header for ten seconds | No speaker, no LED. The Sound and LED rows are gone. |
| 96px logos, 24px badges | 128 and 32 | The 3.4MB data partition has room for 35 of each many times over. |
| Four faces, 8–25px | 14 / 19 / 25px Helvetica, 50px Logisoso digits for the price | Read from across a room. |
| Seven 42px rows | Nine 44px rows landscape, sixteen portrait | Settings > Orientation opens a page of four tiles, each a little screen drawn that way round with the cable edge marked: landscape, portrait, or either flipped. Every position comes from one layout table with a landscape and a portrait column; the scan-out callback scrolls a band of lines or of columns to match. A change restarts the board. |
| Detail page stacked | Two columns: logo, name, the big price and the range bars on the left; chart, chips and three headlines on the right | The full headline list is still a swipe up. |
| 3×4 heatmap | 6×4, 24 tiles a page | |
| 6×5 letter grid | 10×3, 80px keys | |
| Settings scroll | Twelve rows, one scroll step | Shutdown, Scroll, Columns, Orientation, Clock, Theme (a page of twelve colour tiles, each with its own rule and accent: black, midnight, royal, ocean, forest, terminal, espresso, burgundy, purple, slate, graphite, olive), Auto return, Sleep, Currency, Info, Wi-Fi, Clear device. |
| Touch wake on GPIO36 low | GT911 INT on GPIO4, level high | The controller pulses INT on every report and keeps running through deep sleep. **Unverified.** |
| Native USB CDC | UART0 through the board's CH343 bridge | The USB-C is a bridge here; with the CDC flags the monitor saw only the ROM. |

**The USB-C is switchable.** The CH422G's EXIO5 hands the connector to
either the chip's USB or the CAN transceiver. It must stay low; high turned
the serial monitor to garbage mid-boot, which is how it was found.

## Not yet seen

Every page has been drawn against the layout constants and every geometry
assertion passes at boot, but nothing has been looked at on the panel. The
order to check: the setup page (are the colours right -- gold digits, green
step numbers; if gold reads blue the R and B pin groups are swapped in
`board.h`), then the list's crawl, then a drag, then a stock's page, then Settings >
Orientation once round: portrait has been booted and run its self-checks
but nobody has looked at it.

The [CYD ticker's README](../../../esp32-2432s028r-cyd/apps/ticker/) is the
reference for everything the two share.
