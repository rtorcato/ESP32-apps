# social — **built as a demo** (first flash 2026-09-18)

A ticker for your own numbers: followers, stars, downloads, views, and the
likes on whatever you posted last, one row per account, each with a
sparkline of the last thirty days and today's change. The Ticker Tape
shape, with accounts where the stocks are. Nothing to sign in to on the
device.

## What can actually be read, keyless (probed 2026-09-18)

| service | endpoint | gives | note |
|---|---|---|---|
| Bluesky | `public.api.bsky.app/xrpc/app.bsky.actor.getProfile?actor=` | followers, follows, posts | and `app.bsky.feed.getAuthorFeed` gives likes / reposts / replies per post -- the richest source by far |
| Mastodon | `<instance>/api/v1/accounts/lookup?acct=` | followers, posts | any instance; `/statuses` for the latest post's favourites |
| GitHub | `api.github.com/users/<u>`, `/repos/<u>/<r>` | followers, public repos; stars, forks, watchers per repo | 60 requests an hour unauthenticated, plenty for a few rows every ten minutes |
| npm | `api.npmjs.org/downloads/point/last-week/<pkg>` | downloads last day / week / month | per package |
| YouTube | `youtube.com/feeds/videos.xml?channel_id=` | the latest videos with their view counts | subscriber counts need the Data API (a key) or a 2.5MB page scrape -- not on the board |

Refused or closed: Reddit (403 to anything without a browser session),
Instagram, TikTok, X, Threads, Twitch, Spotify (all need an app token or
worse). The list on the panel is what the table above allows, which is
the open web: Bluesky, Mastodon, GitHub, npm, YouTube views.

## What the panel would show

- **List.** A row per account: the service's mark, the handle, a 30-day
  sparkline, the count in the tall digits' small cousin, today's change
  green or red. Tabs by service. Crawls like the ticker.
- **Account page.** The count large, the change today / this week / this
  month, the sparkline as a chart with the range chips, and the latest post
  with its likes, reposts and replies (Bluesky and Mastodon) or the latest
  video with its views (YouTube).
- **History.** The board keeps one number a day per account in a small file
  on the data partition; thirty days of ten accounts is under 4KB. That is
  where the sparklines and the deltas come from -- no service hands out
  history, so the board has to have been running to show it.
- **Alerts.** A banner when a post passes a like threshold or an account
  crosses a round number, the way the ticker banners a 5% move.

Built on `lib/ui` (themes, sheets, the header, the gestures) and the
ticker's fetch path. config.json names the accounts:

```json
{ "accounts": [
    { "service": "bluesky",  "id": "rtorcato.bsky.social" },
    { "service": "mastodon", "id": "mastodon.social/@rtorcato" },
    { "service": "github",   "id": "rtorcato" },
    { "service": "github",   "id": "rtorcato/js-common", "repo": true },
    { "service": "npm",      "id": "@rtorcato/js-common" },
    { "service": "youtube",  "id": "UC..." }
] }
```

```sh
pio run -e social -t upload
./push-config social
```

## On the board

- **Home.** A tile per service in use: its logo (Simple Icons, made by
  `tools/make-service-logos.py`), its name, the first account's number,
  how many accounts. A tap opens the account when there is one, the list
  when there are more.
- **List.** A row per account: the service's tile, the label, what the
  number is, a 30-day sparkline, the count, today's change (green, red, or
  "new" until there is a yesterday). Tabs ALL and one per service in use; a
  drag scrolls, a swipe changes the tab, a tap opens the account.
- **Account.** The count in the tall digits, the change today and this
  week, the second number (posts, forks, repos, downloads a day), the
  history as a chart, and the latest post with its likes, reposts and
  replies -- or the latest video with its views, or a repo's description
  and open issues.
- **History.** `/history.json` on the data partition: one number a day per
  account, thirty kept. The first day shows "new"; the deltas and the lines
  appear from the second day on.
- **Settings.** Clock, Theme (the twelve), Info, Wi-Fi (read-only, the
  ticker's), Shut down (a sheet, or two seconds on the header).

The shipped config names demo accounts (the official Bluesky and Mastodon
accounts, rtorcato on GitHub and npm, a large YouTube channel). Your own
go in `data/config.local.json`, the same shape, merged on top and
gitignored; `tools/my-github.py <user>` fills it with a GitHub user and
every public repo. GitHub accounts refresh hourly whatever `refreshMinutes`
says: sixty requests an hour is the limit without a token. The open question stands: if your numbers live on
Instagram, TikTok or X, the board cannot read them.
