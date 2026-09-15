# ESP32-apps

Apps for my ESP32 boards. One directory per device, because pinouts and display
drivers are not portable between them — a "clock" for a 172×320 SPI panel shares
almost no code with a clock for an 800×480 RGB-parallel panel.

```
<device>/
  README.md          device spec, verified pinout, toolchain gotchas
  APP-CHECKLIST.md   conventions every app on that device follows
  platformio.ini     only for devices I actually own
  lib/board/         pin definitions for that board
  apps/<app>/
    README.md        the idea: what it does, why this board, the hard parts
    preview.svg      layout wireframe at the panel's real pixel size (vertical)
    preview-h.svg    the horizontal layout, where the app supports both
    src/main.cpp     only once the app is actually built
```

Where a panel works either way up, **an app should ship both orientations** —
one source, a `-DBOARD_LANDSCAPE` build flag, and a second `<app>-h` env. Write
layouts against the board's `LCD_W`/`LCD_H` rather than literal dimensions and
that stays cheap. Each orientation gets its own preview.

Most `apps/*` directories are **ideas only** — a README and a preview, no code.
That is deliberate: the list is a menu to pick from later, not a backlog of
half-finished sketches.

Every `preview.svg` is drawn at its panel's **native resolution** (172×320,
800×480, 480×480 clipped to a circle, …), so if the text fits in the wireframe
it fits on the hardware. They double as the layout spec to code against, and use
only presentation attributes so they render anywhere — GitHub, VS Code preview,
or a browser.

## Devices

| Device | Status | Display | Built |
|---|---|---|---|
| [esp32-c6-lcd-1.47](esp32-c6-lcd-1.47/) | Owned | 172×320 SPI ST7789 | `hello` |
| [esp32-2432s028r-cyd](esp32-2432s028r-cyd/) | Wishlist | 240×320 SPI ILI9341 + resistive touch | — |
| [esp32-s3-touch-lcd-7](esp32-s3-touch-lcd-7/) | Wishlist | 800×480 RGB parallel + cap touch | — |
| [elecrow-rotary-2.1](elecrow-rotary-2.1/) | Owned | 480×480 round RGB ST7701 + cap touch + knob | `hello` |
| [elecrow-rotary-1.28](elecrow-rotary-1.28/) | Wishlist | 240×240 round SPI + knob | — |

Purchase links and the cross-device comparison live in [DEVICES.md](DEVICES.md).
Credential handling on a device that can be stolen is in
[SECURITY.md](SECURITY.md).

## Toolchain

Arduino framework via PlatformIO, using the **pioarduino** platform fork —
official `platformio/platform-espressif32` has no Arduino support for the C6
(build fails with `This board doesn't support arduino framework!`), because the
C6 needs Arduino-ESP32 core 3.x and upstream PlatformIO is still on 2.x.

**`brew install platformio` does not work here** — the bottle fails with
`Couldn't find manifest matching bottle checksum`. PlatformIO is a Python
package, so install it into its own venv:

```sh
python3 -m venv ~/.platformio-venv
~/.platformio-venv/bin/pip install platformio
```

Then build and flash (add `~/.platformio-venv/bin` to `PATH` to drop the prefix):

```sh
cd esp32-c6-lcd-1.47
~/.platformio-venv/bin/pio run -e hello -t upload
```

`pio device monitor` needs an interactive terminal. To read serial from a
script, use pyserial directly — it ships in the venv:

```sh
~/.platformio-venv/bin/python -c "import serial;s=serial.Serial('/dev/cu.usbmodem21201',115200);[print(s.readline().decode().strip()) for _ in range(20)]"
```

Each app is a PlatformIO *environment* inside its device's `platformio.ini`, so
`src_dir = apps` plus a per-env `build_src_filter` compiles exactly one app.
Shared board code lives in `lib/board/` and is picked up automatically.
