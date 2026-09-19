# launcher — idea (2026-09-19)

The base app. It owns the board and the settings that are the board's, shows
a picker, and loads one app at a time. Everything else here is an app it
launches.

## Why one app at a time

Storage was never the constraint. Measured on 2026-09-19: a whole app is
1.18–1.33MB against a 6.5MB slot, and ~1.15MB of every one of those is the
same Arduino/Wi-Fi/TLS/GFX/JSON core — the app's own code is 30–180KB. All
twenty apps in the board README would fit in one firmware with room over.

The reason not to is RAM, and the fact that one panel shows one thing. Four
apps compiled together is ~145KB of the 327KB DRAM before anything runs;
twenty is not possible without turning every app's globals into a PSRAM
struct. One app resident sidesteps all of it: each gets the whole chip, apps
stay standalone firmwares, and `sports` keeps its `LCD_BOUNCE_LINES=40`
without costing the others 128KB.

So this is not iOS. There is no dynamic linker here, and nothing is loaded
into a running program. Apps share code at *link* time through `lib/`, and
the launcher swaps which firmware boots.

## Partitions

```
nvs       data nvs       0x9000    0x5000     shared settings, launcher + app
otadata   data ota       0xe000    0x2000
factory   app  factory   0x10000   0x200000   2MB   launcher, always resident
ota_0     app  ota_0     0x210000  0x300000   3MB   whichever app is loaded
littlefs  data spiffs    0x510000  0xAE0000   11.1MB  logos, thumbnails, config
coredump  data coredump  0xFF0000  0x10000
```

Every env builds against this one CSV, so NVS and LittleFS line up between
the launcher and whatever it launched. The same `.bin` boots from either app
partition, so an app can still be flashed straight over USB during
development and the launcher skipped entirely.

## The two transitions

**Home**, the frequent one: the app calls
`esp_ota_set_boot_partition(factory)` and restarts. ~2s, no network, six
lines in `lib/board`.

**Load a different app**, the rare one: the launcher copies `<app>.bin` into
`ota_0`, sets the boot partition and reboots. From a LittleFS cache that is
3–5s and works with the internet down; over the network it is ~1.2MB and
about 30s.

That asymmetry is the point. You bounce Home constantly and change apps
occasionally.

## Shared settings

NVS namespace `base`: ssid, pass, timezone, theme, brightness, rotation,
sleep schedule. The launcher owns the setup flow and the on-screen keyboard;
apps read it.

This replaces something that is currently a hack — `apps/sports` reads Wi-Fi
credentials out of the **ticker's** NVS namespace
(`apps/sports/src/main.cpp:700`) and prints "no network in NVS: run the
ticker once and set it up there" when they are missing.

## The escape hatch

If a bad app crash-loops in `ota_0` you need out without a USB cable. IDF
rollback works but the Arduino core ships it disabled, so the cheap version
is three lines at the top of boot: BOOT held at power-on → set boot to
factory → restart.

## Built on

`lib/board` (panel, touch, `netjoin.h`, `sleep.h`) and `lib/ui` (themes,
sheets, header, gestures, on-screen keyboard). The picker is a grid; the
thumbnails come from each app's existing `preview.svg`, downscaled to
160×100 `.565`, 32KB each.

## Deliberately deferred

- **Downloading apps from the repo.** The launcher code is the small part;
  the cost is a release pipeline — tagged releases, a CI build of N
  binaries, a manifest with hashes, CA pinning, version discipline. Cache in
  LittleFS first and let the network earn its keep on updates only.
- **Launcher self-update.** You cannot rewrite `factory` while running from
  it. Solvable — write the new launcher to `ota_0`, boot it, let it copy
  itself into `factory`, boot back — but that is ~40 lines of fiddly for
  something that changes twice a year. USB until it annoys.
- **Thumbnails.** A text grid is fine at four apps.

## Before any of it

Verifying the download is not optional: the launcher would be fetching
executable code over the network. Pin the root CA and check a sha256 from
the manifest *before* `esp_ota_set_boot_partition`, not after.

And `apps/rtc-probe` has to answer whether `.rtc.data` survives a plain
`esp_restart`, because an app swap is exactly that. `lib/board/sleep.h`
already defends against the aliasing either way; the answer decides whether
that defence is load-bearing or insurance.
