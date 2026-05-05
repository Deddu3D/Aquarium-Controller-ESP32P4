# Web UI – SD card files

This directory contains the web dashboard files that must be copied
to the microSD card before the Web UI is accessible.

## File list

| File | Description |
|------|-------------|
| `index.html` | Main dashboard page (HTML + JS) |
| `style.css`  | Aquarium-themed stylesheet — edit this to change the look |
| `bg.jpg` *(optional)* | Full-page background photo (e.g. your tank). Drop it here and refresh. |
| `README.md` | This file |

## Setup

Copy **this entire `www/` folder** to the root of the SD card:

```
/sdcard/www/index.html
/sdcard/www/style.css
```

The controller creates the `/sdcard/www/` directory automatically at
boot if it is missing, but it does not copy the files — they must be
placed manually (or via the SD-card file-browser in the Web UI once
a previous version of index.html is already present).

## How it works

On every `GET /` request the firmware opens `/sdcard/www/index.html`
and streams it to the browser in 2 KB chunks.  No RAM copy of the
whole page is needed.

Any file under `/www/` on the SD card can be fetched by the browser
via `GET /www/<filename>` (e.g. `GET /www/style.css`).  MIME types
are detected automatically (.html, .css, .js, .json, .png, .jpg,
.gif, .svg, .ico, .woff, .woff2, .ttf).  Responses include
`Cache-Control: max-age=3600` so the browser caches them for 1 hour.

If the SD card is not mounted or the file is not found, a minimal
fallback page is served with instructions to copy `www/` to the card.

All dynamic data (temperature, LED state, relay status, etc.) is
fetched by the page JavaScript via the `/api/*` REST endpoints.
The status bar (IP / NTP / Uptime / Partition) is populated on load
from `/api/status`.

## Design language

The stylesheet uses the same colour palette as the physical display
(`display_ui.c`):

| Variable | Hex | Role |
|----------|-----|------|
| `--bg`      | `#0b1e2d` | Page background |
| `--card`    | `#102a3d` | Card surface |
| `--primary` | `#1fa3ff` | Blue accent |
| `--yellow`  | `#f1c40f` | Lights / scenes |
| `--orange`  | `#f39c12` | Temperature warning |
| `--text`    | `#ecf5fb` | Main text |
| `--muted`   | `#5c7f9a` | Secondary text |
| `--on`      | `#2ecc71` | Active / OK |
| `--err`     | `#e74c3c` | Error / alarm |

Glassmorphism cards use `backdrop-filter: blur(12px)`.

The background shows animated rising bubbles (pure CSS) plus optional
underwater light-ray gradients.  Add a `bg.jpg` for a real aquarium
photo background.

## Adding a custom background image

1. Take a photo of your aquarium (or download one).
2. Rename it `bg.jpg` and copy to `/sdcard/www/bg.jpg`.
3. Refresh the browser — the image appears behind the gradient overlays.

The `style.css` references `url('/www/bg.jpg')` as the bottom layer
of `background-image`.  Other supported formats: `.png`, `.webp`
(just update the reference in style.css).

## Updating the UI

To update the dashboard without re-flashing the firmware:
1. Edit files in `main/www/` on your PC.
2. Copy the updated files to the SD card.
3. Hard-refresh the browser (Ctrl+Shift+R / Cmd+Shift+R).

