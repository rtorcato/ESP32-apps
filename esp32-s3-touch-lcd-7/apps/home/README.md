# Home — the base app

**Status: built 2026-09-19.** Splash, app picker, shared settings, Wi-Fi
setup and boot switching work. The picker can only run what is already in the
slot -- loading a different app is still a USB flash.

It owns the board and the settings that are the board's, and loads one app at
a time. Everything else on this board is an app it hands the screen to.

Called Home rather than "launcher" because it is more than a launcher -- it
holds the settings, the Wi-Fi and the shutdown -- and because it names the
gesture: an app goes Home.

## 2.4GHz only

The ESP32-S3's radio is Wi-Fi 4 — 802.11 b/g/n, all of which are 2.4GHz. There
is no 5GHz hardware on the chip, so a 5GHz network cannot appear in the setup
scan, and typing its name into the hidden-network box does not help either.

Routers commonly publish the same network twice, `Name` and `Name-5G`. The one
without the suffix is the 2.4GHz radio; that is the one to pick. The setup form
and the panel both say so, because "my network is not in the list" is otherwise
a dead end with no clue in it.

## Why your phone will not autofill the password

It is not the form. iOS and Android keep **Wi-Fi passwords in a different
vault from website passwords**, and only the website vault is offered to a web
form. The iOS Passwords app will show you a Wi-Fi entry, but it will not hand
it to a page. Three other things stack on top of that, none of them fixable
here: the origin is plain HTTP, it is a bare IP with no domain for a saved
credential to match against, and a captive-portal page opens in a restricted
web sheet rather than Safari.

The form still carries `autocomplete=username` / `autocomplete=current-password`,
because they cost nothing and make the fields behave correctly for anyone using
a third-party manager that does offer to fill them.

What does matter, and was a real bug: `autocapitalize=none` on the
hidden-network box. iOS capitalises the first letter of a plain text input,
which silently turns a lowercase SSID into one that does not exist.

To get the password across: Settings › Wi-Fi › (i) › Password, then paste.

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
Home swaps which firmware boots.

## Partitions

```
nvs       data nvs       0x9000    0x5000     shared settings, Home + app
otadata   data ota       0xe000    0x2000
factory   app  factory   0x10000   0x200000   2MB   Home, always resident
ota_0     app  ota_0     0x210000  0x300000   3MB   whichever app is loaded
littlefs  data spiffs    0x510000  0xAE0000   11.1MB  logos, thumbnails, config
coredump  data coredump  0xFF0000  0x10000
```

Every env builds against this one CSV, so NVS and LittleFS line up between
Home and whatever it launched. The same `.bin` boots from either app
partition, so an app can still be flashed straight over USB during
development and Home skipped entirely.

## The two transitions

**Home**, the frequent one: the app calls
`esp_ota_set_boot_partition(factory)` and restarts. ~2s, no network, six
lines in `lib/board`.

**Load a different app**, the rare one: Home copies `<app>.bin` into
`ota_0`, sets the boot partition and reboots. From a LittleFS cache that is
3–5s and works with the internet down; over the network it is ~1.2MB and
about 30s.

That asymmetry is the point. You bounce Home constantly and change apps
occasionally.

## Shared settings

NVS namespace `base`: ssid, pass, timezone, theme, brightness, rotation,
sleep schedule. Home owns the setup flow and the on-screen keyboard;
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

- **Downloading apps from the repo.** Home code is the small part;
  the cost is a release pipeline — tagged releases, a CI build of N
  binaries, a manifest with hashes, CA pinning, version discipline. Cache in
  LittleFS first and let the network earn its keep on updates only.
- **Home self-update.** You cannot rewrite `factory` while running from
  it. Solvable — write the new Home to `ota_0`, boot it, let it copy
  itself into `factory`, boot back — but that is ~40 lines of fiddly for
  something that changes twice a year. USB until it annoys.
- **Thumbnails.** A text grid is fine at four apps.

## Before any of it

Verifying the download is not optional: Home would be fetching
executable code over the network. Pin the root CA and check a sha256 from
the manifest *before* `esp_ota_set_boot_partition`, not after.

And `apps/rtc-probe` has to answer whether `.rtc.data` survives a plain
`esp_restart`, because an app swap is exactly that. `lib/board/sleep.h`
already defends against the aliasing either way; the answer decides whether
that defence is load-bearing or insurance.
