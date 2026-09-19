# rtc-probe — does RTC memory survive a restart?

**Throwaway.** Delete it once the answer is written down.

## The question

Swapping apps on this board means `esp_restart()` into a different firmware.
`.rtc.data` is PROGBITS at a fixed address — `0x50000200` in every build here —
so two firmwares put two different layouts in the same bytes.

- **If a plain restart reinitialises it**, an app swap starts from a clean
  region and the snapshot header in `lib/board/sleep.h` is cheap insurance.
- **If it does not**, app B reads app A's bytes through B's struct and believes
  them, and that header is load-bearing — as is checking it before *every*
  restore, in every app, forever.

The IDF docs are easy to misread on this, and the answer decides a design, so
this measures it instead.

## Running it

```
pio run -e rtc-probe -t upload -t monitor
```

The screen shows the wake cause, whether the snapshot was restored, and a boot
counter. Two buttons:

| Button | What it tests |
|---|---|
| **RESTART** | `esp_restart()` — the app-swap case, the one we care about |
| **SLEEP 10s** | deep sleep — the control, which should always restore |

## Reading the result

Tap **SLEEP** first. The counter must survive — if it doesn't, something is
wrong with the probe, not with the chip.

Then tap **RESTART** twice.

| Counter after RESTART | Meaning |
|---|---|
| resets to 1 | `.rtc.data` is reinitialised on a restart. App swaps are safe by construction. |
| keeps climbing | RTC memory persists across a restart. **The header check is mandatory**, and any app that skips it will eventually read another app's bytes. |

Either way, write the answer into `lib/board/sleep.h`'s comment and delete this
app and its `[env:rtc-probe]` block.
