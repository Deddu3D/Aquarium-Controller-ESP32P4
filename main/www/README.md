# Web UI – file sorgente

Questa directory contiene i file della Web UI.
Vengono **compilati direttamente nel firmware** tramite `CMake EMBED_TXTFILES`
e serviti dall'ESP32-P4 senza alcuna SD card.

## File

| File | Descrizione |
|------|-------------|
| `index.html` | Dashboard HTML + JS (inclusi sprite SVG inline) |
| `style.css`  | Foglio di stile dark (palette identica a `display_ui.c`) |
| `bg.svg`     | Scena subacquea SVG (sfondo animato) |
| `manifest.json` | PWA manifest (nome app, icone, theme color) |
| `sw.js`      | Service Worker (cache PWA offline) |

## Come funziona

Il `CMakeLists.txt` della componente `main` usa `EMBED_TXTFILES` per incorporare
tutti i file di questa directory nel binario ELF:

```cmake
EMBED_TXTFILES
    "www/index.html"
    "www/style.css"
    "www/bg.svg"
    "www/manifest.json"
    "www/sw.js"
```

Il `web_server.c` li serve con i MIME type corretti:

- `GET /`              → `index.html`
- `GET /www/style.css` → `style.css`
- `GET /www/bg.svg`    → `bg.svg`
- `GET /manifest.json` → `manifest.json`
- `GET /sw.js`         → `sw.js`

## Aggiornare la UI

Per aggiornare la Web UI è necessario ricompilare e reflashare il firmware:

```bash
# Modifica i file in main/www/
idf.py build
idf.py -p /dev/ttyACM0 flash
```

In alternativa, usa l'**OTA via upload browser** (`POST /api/ota/upload`)
per aggiornare senza collegamento fisico.

## Palette colori

| Variabile CSS | Hex | Ruolo |
|---------------|-----|-------|
| `--bg`      | `#0b1e2d` | Sfondo pagina |
| `--card`    | `#102a3d` | Superficie card (glassmorphism) |
| `--primary` | `#1fa3ff` | Blu accent |
| `--yellow`  | `#f1c40f` | Luci / scene |
| `--orange`  | `#f39c12` | Temperatura warning |
| `--text`    | `#ecf5fb` | Testo principale |
| `--muted`   | `#5c7f9a` | Testo secondario |
| `--on`      | `#2ecc71` | Attivo / OK |
| `--err`     | `#e74c3c` | Errore / allarme |

La palette è identica a quella usata nel display fisico (`display_ui.c`).
