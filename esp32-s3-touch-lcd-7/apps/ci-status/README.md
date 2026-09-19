# ci-status — idea (2026-09-19)
Green and red tiles for the things that should be green: your repos'
latest workflow runs and the sites an uptime monitor watches.

## Sources

- GitHub Actions: `api.github.com/repos/{owner}/{repo}/actions/runs?per_page=1`,
  keyless for public repos (sixty an hour without a token, so the refresh
  is spaced; a token for private ones). `tools/my-github.py` from the
  social app already lists the repos.
- Uptime Kuma, Healthchecks.io, Better Stack: each has a status endpoint
  with a key.
- npm publish state and the latest version of each package from the
  registry, keyless.

## On the panel

- **Tiles.** A grid, one per repo or monitor, green, red, or amber while
  running; the name, the branch, how long ago; a red tile jumps to the
  front.
- **Detail.** Tap a tile: the run's jobs, the commit message, who pushed.
- **Banner.** A red tile is a banner across every page, the way the ticker
  banners a move.

## Built on

`lib/ui` (themes, sheets, the header, the gestures, the on-screen keyboard)
and the fetch path from the ticker: HTTP/1.0 into a buffer, ArduinoJson with
a filter, one request at a time from the fetch task. The Wi-Fi network is
the ticker's (NVS `ticker`). A key, where one is needed, is typed once on
a setup page like Network's and kept in NVS, or put in a gitignored
`data/config.local.json`.
