# Web UI – SD card files

This directory contains the web dashboard files that must be copied
to the microSD card before the Web UI is accessible.

## Setup

Copy **this entire `www/` folder** to the root of the SD card:

```
/sdcard/www/index.html
```

The controller creates the `/sdcard/www/` directory automatically at
boot if it is missing, but it does not copy the files — they must be
placed manually (or via the SD-card file-browser in the Web UI once
a previous version of index.html is already present).

## How it works

On every `GET /` request the firmware opens `/sdcard/www/index.html`
and streams it to the browser in 2 KB chunks.  No RAM copy of the
whole page is needed.

If the SD card is not mounted or the file is not found, a minimal
fallback page is served with instructions to copy `www/` to the card.

All dynamic data (temperature, LED state, relay status, etc.) is
fetched by the page JavaScript via the `/api/*` REST endpoints.
The status bar (IP / NTP / Uptime / Partition) is populated on load
from `/api/status`.

## Updating the UI

To update the dashboard without re-flashing the firmware:
1. Edit `main/www/index.html` on your PC.
2. Copy the updated file to the SD card.
3. Hard-refresh the browser (Ctrl+Shift+R).
