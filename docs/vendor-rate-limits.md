# Vendor API request limits

Inventatory counts every request it sends to DigiKey, Mouser and TME. The count warns you before a
limit is reached, and says when a reached limit ends. The code is `src/core/vendor/VendorRateLimits.cpp`.

## Limits used

| Vendor | Limit | Source |
|---|---|---|
| DigiKey (Product Information) | 120 per minute, 1,000 per day | developer.digikey.com/documentation |
| Mouser (Search API) | 30 per minute, 1,000 per day | mouser.com/api-search |
| TME | none published | TME's developer documentation and OpenAPI file state no limit |

DigiKey sends `X-RateLimit-Limit` and `X-RateLimit-Remaining` with its answers. Inventatory uses them for
the daily count, so requests from other programs that share the key are counted too. A 429 answer carries
`Retry-After` (and `X-RateLimit-Reset` / `X-BurstLimit-Reset`); that sets the time the vendor is usable again.
Mouser and TME send no such headers, so their counts come from this computer's own requests only.

The windows are counted as rolling windows. A vendor may reset a day on a fixed clock time instead; when the
vendor sends a reset time, that time wins. Treat the "available again at" time from a local count as an estimate.

## What you see

- **Close to the limit:** once a window of an hour or longer is 80% used, one message appears (for example
  "Mouser: 800 of 1000 requests used per day"). It appears again only after the count has dropped below 80%.
  Per-minute windows are not warned about: the app waits for them instead.
- **Limit reached:** "Mouser request limit reached (1000 per day); available again at 14:32." The message
  appears once, and every failed lookup carries the same text. A batch (refresh, shortage lookup) stops at
  that point. No request is sent while a vendor is blocked.
- **Settings:** each vendor page has a **Usage** row, for example `850 of 1000 per day, 0 of 30 per minute`.
  It is red once the limit is reached, and yellow when it is close.

A wait of up to two minutes (a full per-minute window, a short `Retry-After`) is slept through. A longer one is
refused with the message above.

## Storage

Counts are kept in `vendor-usage.conf` beside `settings.conf` (not in the inventory data directory). The file
holds request times and block times only, no keys. Deleting it resets the counts.

## Changing a limit

Edit the table in `specs()` in `VendorRateLimits.cpp`, this page and the user guide together.
