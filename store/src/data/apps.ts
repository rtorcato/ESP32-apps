import { boards } from "./boards"

// Every `<board>/apps/<app>/preview.svg` in the repo, as a URL. An app with the same
// folder name on several boards is one app that runs on all of them.
const previews = import.meta.glob<string>("../../../*/apps/*/preview.svg", {
  query: "?url",
  import: "default",
  eager: true,
})

type Meta = { name: string; tagline: string; price?: string }

// ponytail: hand-written until app manifests exist. A preview without an entry here
// still shows up, named after its folder.
const meta: Record<string, Meta> = {
  "ble-sensors": {
    name: "BLE Sensors",
    tagline: "Cheap wireless thermometers, on one panel.",
  },
  "desk-clock": {
    name: "Desk Clock",
    tagline: "Time, weather and a three-day forecast.",
  },
  "host-monitor": {
    name: "Host Monitor",
    tagline: "CPU, memory and disk for any machine.",
  },
  notifier: {
    name: "Notifier",
    tagline: "An ambient alert screen for MQTT or HTTP.",
  },
  "poe-cycle": {
    name: "PoE Cycle",
    tagline: "Every UniFi switch port, and power-cycle the stuck ones.",
  },
  "protect-doorbell": {
    name: "Protect Doorbell",
    tagline: "Doorbell rings and camera motion, live.",
  },
  ticker: {
    name: "Ticker",
    tagline: "Stocks and crypto, always on your desk.",
  },
  "unifi-status": {
    name: "UniFi Status",
    tagline: "Your home network on one glanceable panel.",
  },
  "wan-watchdog": {
    name: "WAN Watchdog",
    tagline: "Keep your ISP honest: uptime, latency, outages.",
  },
  "wifi-scanner": {
    name: "Wi-Fi Scanner",
    tagline: "Nearby networks ranked by signal and channel.",
  },
  "zigbee-hub": {
    name: "Zigbee Hub",
    tagline: "Zigbee sensors straight over the C6 radio.",
  },
  "bike-buddy": {
    name: "Bike Buddy",
    tagline: "A handlebar computer for mountain biking.",
  },
  pomodoro: {
    name: "Pomodoro",
    tagline: "Focus timer with a session count for the day.",
    price: "$0.99",
  },
  "sd-file-browser": {
    name: "SD Browser",
    tagline: "Browse the SD card: text, images and more.",
  },
  soundboard: {
    name: "Soundboard",
    tagline: "Tap a tile, play a sound off the SD card.",
    price: "$0.99",
  },
  "touch-dashboard": {
    name: "Touch Dashboard",
    tagline: "Tiles for lights, scenes and switches.",
  },
  "desk-timer": {
    name: "Desk Timer",
    tagline: "Turn to set the minutes, press to start.",
  },
  "scroll-knob": {
    name: "Scroll Knob",
    tagline: "A desktop scroll wheel with a screen.",
  },
  "single-gauge": {
    name: "Single Gauge",
    tagline: "One value, one dial, done properly.",
  },
  "light-dimmer": {
    name: "Light Dimmer",
    tagline: "Turn to dim, press to toggle, hold for scenes.",
  },
  "media-knob": {
    name: "Media Knob",
    tagline: "A volume knob that shows what's playing.",
  },
  thermostat: {
    name: "Thermostat",
    tagline: "The classic round dial for your heating.",
  },
  "world-clock": {
    name: "World Clock",
    tagline: "Turn the knob, turn the globe, see the time.",
  },
  "ha-panel": {
    name: "HA Panel",
    tagline: "A full-size Home Assistant control surface.",
  },
  "modbus-readout": {
    name: "Modbus Readout",
    tagline: "Energy meters and drives over RS485.",
  },
  "obd2-gauge": {
    name: "OBD2 Gauge",
    tagline: "Live car telemetry the dashboard won't show.",
  },
  social: {
    name: "Social",
    tagline: "Followers, stars and downloads, ticking up.",
  },
  "wall-dashboard": {
    name: "Wall Dashboard",
    tagline: "Today's calendar and status, on the wall.",
  },
}

export type App = Meta & {
  slug: string
  price: string
  /** Board slug → preview URL, in catalogue board order. */
  screens: Map<string, string>
}

const bySlug = new Map<string, App>()
for (const board of boards) {
  for (const [path, url] of Object.entries(previews)) {
    const [, dir, slug] =
      path.match(/([^/]+)\/apps\/([^/]+)\/preview\.svg$/) ?? []
    // `hello` is each board's smoke test, not an app.
    if (dir !== board.slug || slug === "hello") continue
    let app = bySlug.get(slug)
    if (!app) {
      const m = meta[slug] ?? { name: slug.replace(/-/g, " "), tagline: "" }
      app = { slug, price: "FREE", ...m, screens: new Map() }
      bySlug.set(slug, app)
    }
    app.screens.set(board.slug, url)
  }
}

export const apps = [...bySlug.values()].sort((a, b) =>
  a.name.localeCompare(b.name)
)
export const appBySlug = (slug: string) => bySlug.get(slug)
export const appsForBoard = (board: string) =>
  apps.filter((a) => a.screens.has(board))
