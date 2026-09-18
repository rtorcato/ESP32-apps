# social — idea (2026-09-18)

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

Open question before building: which of these do you actually post on?
If the answer is mostly Instagram, TikTok or X, the board cannot read
them and the app has no rows worth having.
