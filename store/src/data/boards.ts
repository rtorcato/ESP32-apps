// Board catalogue. Specs and buy links come from DEVICES.md and each board's README.
// Sizes are approximate outer dimensions in mm (Zoey's mockup): correct them once
// the real boards are measured. Screen rects are in mm from the frame's top-left.

export type Frame =
  | {
      kind: "bar" | "pcb" | "tab"
      w: number
      h: number
      screen: { x: number; y: number; w: number; h: number }
    }
  | { kind: "knob-dark" | "knob-silver"; d: number; screen: number }

export type Board = {
  /** Folder name in the repo, also the URL slug. */
  slug: string
  short: string
  name: string
  maker: string
  tagline: string
  /** Shown on board cards. */
  meta: string
  frame: Frame
  /** Screen resolution, for the "Runs on" caption. */
  res: string
  /** Default app shown on the board (app slug). */
  heroApp: string
  chips: Array<string>
  specs: Array<[label: string, value: string]>
  inBox: Array<string>
  pickIf: string
  amazon: { ca: string; com: string }
}

export const boards: Array<Board> = [
  {
    slug: "esp32-c6-lcd-1.47",
    short: "C6 1.47″",
    name: "ESP32-C6 LCD 1.47″",
    maker: "Waveshare",
    tagline:
      "Waveshare ESP32-C6-LCD-1.47 · the only board here that speaks Zigbee and Thread",
    meta: "1.47″ · no touch · Wi-Fi 6",
    frame: {
      kind: "bar",
      w: 20,
      h: 47,
      screen: { x: 1.15, y: 5, w: 17.7, h: 33 },
    },
    res: "172×320",
    heroApp: "unifi-status",
    chips: [
      "1.47″ 172×320",
      "No touch",
      "ESP32-C6 · RISC-V",
      "Wi-Fi 6",
      "Zigbee · Thread",
      "microSD",
    ],
    specs: [
      ["Chip", "ESP32-C6"],
      ["Screen", "1.47″ IPS · 172×320"],
      ["Touch", "None"],
      ["Wireless", "Wi-Fi 6 · BLE 5 · 802.15.4"],
      ["Power", "USB-C 5 V"],
      ["Size", "≈ 20 × 47 mm"],
    ],
    inBox: ["Board with screen"],
    pickIf:
      "you want a small always-on display, or Zigbee and Thread sensors without a dongle.",
    amazon: {
      ca: "https://www.amazon.ca/dp/B0DHTMYTCY",
      com: "https://www.amazon.com/dp/B0DHTMYTCY",
    },
  },
  {
    slug: "esp32-2432s028r-cyd",
    short: "CYD 2.8″",
    name: "Cheap Yellow Display 2.8″",
    maker: "DIYmalls",
    tagline:
      "DIYmalls ESP32-2432S028R · the best-documented cheap ESP32 screen",
    meta: "2.8″ · resistive touch",
    frame: {
      kind: "pcb",
      w: 50,
      h: 86,
      screen: { x: 3.6, y: 4, w: 42.7, h: 56.9 },
    },
    res: "240×320",
    heroApp: "pomodoro",
    chips: [
      "2.8″ 240×320",
      "Resistive touch",
      "ESP32 · 2 cores",
      "microSD",
      "Speaker header",
      "USB · flash in Chrome",
    ],
    specs: [
      ["Chip", "ESP32-WROOM-32"],
      ["Screen", "2.8″ TFT · 240×320"],
      ["Touch", "Resistive (stylus)"],
      ["Wireless", "Wi-Fi 4 · Bluetooth"],
      ["Power", "USB 5 V"],
      ["Size", "≈ 86 × 50 mm"],
    ],
    inBox: [
      "Board with screen",
      "USB cable",
      "Touch stylus",
      "JST jumper leads",
    ],
    pickIf:
      "you want something that just works. Cheapest board here, best documented, has audio out.",
    amazon: {
      ca: "https://www.amazon.ca/dp/B0CG2WQGP9",
      com: "https://www.amazon.com/dp/B0CG2WQGP9",
    },
  },
  {
    slug: "elecrow-rotary-1.28",
    short: "Rotary 1.28″",
    name: "Rotary Display 1.28″",
    maker: "Elecrow",
    tagline: "Elecrow 1.28″ round ESP32-S3 · the cheapest way to add a knob",
    meta: "1.28″ round · knob + touch",
    frame: { kind: "knob-silver", d: 48, screen: 32.5 },
    res: "240×240",
    heroApp: "desk-timer",
    chips: [
      "1.28″ 240×240 round",
      "Capacitive touch",
      "Rotary knob",
      "ESP32-S3 · 2 cores",
    ],
    specs: [
      ["Chip", "ESP32-S3"],
      ["Screen", "1.28″ round IPS · 240×240"],
      ["Touch", "Capacitive"],
      ["Wireless", "Wi-Fi 4 · BLE 5"],
      ["Power", "USB-C 5 V"],
      ["Size", "≈ Ø 48 mm"],
    ],
    inBox: ["Board with screen"],
    pickIf:
      "you want a knob for one value (a timer, a gauge, a scroll wheel) on a small budget.",
    amazon: {
      ca: "https://www.amazon.ca/s?k=Elecrow+1.28+inch+ESP32+Rotary+Display+240x240",
      com: "https://www.amazon.com/s?k=Elecrow+1.28+inch+ESP32+Rotary+Display+240x240",
    },
  },
  {
    slug: "elecrow-rotary-2.1",
    short: "Rotary 2.1″",
    name: "Rotary Display 2.1″",
    maker: "Elecrow",
    tagline: "Elecrow 2.1″ round ESP32-S3 · a knob on a big round face",
    meta: "2.1″ round · knob + touch",
    frame: { kind: "knob-dark", d: 72, screen: 53 },
    res: "480×480",
    heroApp: "thermostat",
    chips: [
      "2.1″ 480×480 round",
      "Capacitive touch",
      "Rotary knob",
      "ESP32-S3 · 8 MB PSRAM",
    ],
    specs: [
      ["Chip", "ESP32-S3 N16R8"],
      ["Screen", "2.1″ round IPS · 480×480"],
      ["Touch", "Capacitive"],
      ["Wireless", "Wi-Fi 4 · BLE 5"],
      ["Power", "USB-C 5 V"],
      ["Size", "≈ Ø 72 mm"],
    ],
    inBox: ["Board with screen"],
    pickIf:
      "you want a thermostat, dimmer or volume knob that looks like an appliance, not a dev board.",
    amazon: {
      ca: "https://www.amazon.ca/s?k=Elecrow+2.1+inch+ESP32+Rotary+Display+480x480",
      com: "https://www.amazon.com/s?k=Elecrow+2.1+inch+ESP32+Rotary+Display+480x480",
    },
  },
  {
    slug: "esp32-s3-touch-lcd-7",
    short: "7″ Touch",
    name: "ESP32-S3 Touch LCD 7″",
    maker: "Waveshare",
    tagline:
      "Waveshare ESP32-S3-Touch-LCD-7 · wall dashboards, plus CAN and RS485",
    meta: "7″ · 5-pt touch · CAN/RS485",
    frame: {
      kind: "tab",
      w: 180,
      h: 108,
      screen: { x: 13.75, y: 8.25, w: 152.5, h: 91.5 },
    },
    res: "800×480",
    heroApp: "wall-dashboard",
    chips: [
      "7″ 800×480",
      "5-point touch",
      "ESP32-S3 · 8 MB PSRAM",
      "CAN · RS485",
      "microSD",
    ],
    specs: [
      ["Chip", "ESP32-S3"],
      ["Screen", "7″ LCD · 800×480"],
      ["Touch", "5-point capacitive"],
      ["Wireless", "Wi-Fi 4 · BLE 5"],
      ["Power", "USB-C 5 V"],
      ["Size", "≈ 180 × 108 mm"],
    ],
    inBox: ["Board with screen"],
    pickIf:
      "you want a wall dashboard you can read across the room, or to talk to a car or Modbus bus.",
    amazon: {
      ca: "https://www.amazon.ca/s?k=Waveshare+ESP32-S3+7inch+Capacitive+Touch+LCD",
      com: "https://www.amazon.com/s?k=Waveshare+ESP32-S3+7inch+Capacitive+Touch+LCD",
    },
  },
]

export const boardBySlug = (slug: string) => boards.find((b) => b.slug === slug)

/** Outer size of a frame in mm. */
export const frameSize = (f: Frame) =>
  "d" in f ? { w: f.d, h: f.d } : { w: f.w, h: f.h }

/** px/mm that fits a board's frame inside a w×h px box, capped at `max`. */
export const fitScale = (f: Frame, w: number, h: number, max = 4.6) => {
  const s = frameSize(f)
  return Math.min(max, w / s.w, h / s.h)
}
