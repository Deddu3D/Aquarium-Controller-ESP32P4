# 🐠 Aquarium Controller – ESP32-P4

Controller IoT completo per acquario d'acqua dolce basato su **Waveshare ESP32-P4-WiFi6**.  
Combina una **dashboard Web** accessibile da qualsiasi browser, un **display touch** LVGL da 720 × 720 px, notifiche **Telegram** e un motore di automazione avanzato — tutto in un unico firmware ESP-IDF.

> **Stack**: ESP-IDF v6.0 · ESP-Hosted (P4 + C6 WiFi 6) · LVGL v9 · FreeRTOS · HTTP/HTTPS embedded · PWA

---

## 📸 Screenshot

### Web UI

| Riepilogo | LED Strip |
|:---:|:---:|
| ![Web UI – Riepilogo](docs/screenshots/web_ui_riepilogo.png) | ![Web UI – LED Strip](docs/screenshots/web_ui_led_strip.png) |

| Telegram | Manutenzione |
|:---:|:---:|
| ![Web UI – Telegram](docs/screenshots/web_ui_telegram.png) | ![Web UI – Manutenzione](docs/screenshots/web_ui_manutenzione.png) |

| Mobile |
|:---:|
| ![Web UI – Mobile](docs/screenshots/web_ui_mobile.png) |

### Display touch (Waveshare 4-DSI-TOUCH-A)

| Home / Riepilogo | LED Strip |
|:---:|:---:|
| ![Display – Riepilogo](docs/screenshots/display_ui_riepilogo.png) | ![Display – LED Strip](docs/screenshots/display_ui_led_strip.png) |

| Manutenzione | Notifiche Telegram |
|:---:|:---:|
| ![Display – Manutenzione](docs/screenshots/display_ui_manutenzione.png) | ![Display – Telegram](docs/screenshots/display_ui_telegram.png) |

---

## ✨ Funzionalità

### 💡 Controllo LED WS2812B
- On/off con **ramp di acclimatazione** configurabile (0–120 s)
- Impostazione colore RGB e luminosità (0–255)
- **Schedule alba/tramonto** con orari indipendenti e ramp
- **5 scene animate** (engine FreeRTOS, tick 500 ms):
  - `SUNRISE` – ramp ambra caldo → bianco giorno
  - `SUNSET` – ramp bianco → ambra → buio
  - `MOONLIGHT` – luce lunare blu tenue
  - `STORM` – flickering casuale con lampeggi
  - `CLOUDS` – oscillazione sinusoidale lenta
- **Ciclo giornaliero automatico** con calcolo alba/tramonto NOAA da coordinate GPS:
  - fasi: Night → Sunrise → Morning → Noon → Afternoon → Sunset → Evening → Moonlight

### 🌡️ Temperatura
- Polling periodico **DS18B20** (1-Wire, GPIO configurabile)
- Media mobile per filtrare il rumore
- Storico giornaliero 24 h in RAM (campionamento configurabile, default 5 min)
- Esportazione CSV via REST API

### 🔌 Relè (4 canali)
- Controllo manuale e via schedule orario
- Nomi personalizzati persistiti in NVS
- Polarità active-low/high configurabile

### ♨️ Auto-Heater (termostato)
- Attivazione/spegnimento automatico del relè riscaldatore
- Target e isteresi configurabili
- Protezione **runaway** (timeout + allarme Telegram se relay rimane ON troppo a lungo)

### 💨 CO₂
- Programmazione oraria con **pre-anticipo ON** e **post-ritardo OFF** rispetto allo schedule luci
- Configurazione indipendente via Web UI o REST API

### 🐟 Modalità Alimentazione
- Pausa relè configurabile (1–60 min, default 10 min)
- Dimmer LED opzionale durante la pausa
- Notifica Telegram a inizio e fine sessione

### 📱 Notifiche Telegram
- Allarmi temperatura (alta/bassa)
- Cambio stato relè
- Promemoria cambio acqua e fertilizzante
- Messaggio di test

### 🌐 Web Dashboard (PWA)
- **Progressive Web App** installabile su smartphone
- Design dark con sfondo animato (bolle + scena subacquea SVG)
- Aggiornamento in tempo reale via **WebSocket**
- Sezioni: Riepilogo · LED Strip · Temperatura · Automazioni · Telegram · Manutenzione
- Autenticazione con sessione cookie (login/logout)
- UI **mobile-first** responsive

### 🖥️ Touch Display LVGL v9
- Display circolare 720 × 720 px MIPI-DSI, touch capacitivo GT911
- Dashboard a **5 tab**: Home · Luci · Temperatura · Automazioni · Dati
- Status bar fissa con ora, temperatura badge e stato WiFi
- Overlay allarme modale

### 🔄 OTA (Over-the-Air)
- Aggiornamento firmware da **URL HTTP remoto**
- Upload diretto dal browser (**multipart form**)
- Partizioni dual OTA con **auto-rollback** in caso di crash

### 🛰️ DuckDNS
- Aggiornamento automatico IP dinamico per accesso remoto

### 🔒 Sicurezza
- HTTP Basic Auth + **session cookie** (POST `/api/login`)
- **HTTPS opzionale** con certificato self-signed embedded
- Credenziali modificabili a runtime via `/api/auth`

---

## 🧱 Hardware necessario

### Componenti obbligatori

| Componente | Descrizione | Note |
|---|---|---|
| **Waveshare ESP32-P4-WiFi6** | Board principale (ESP32-P4 + ESP32-C6 coprocessore WiFi 6) | rev 1.3 o superiore |
| **Striscia LED WS2812B** | Striscia indirizzabile RGB | collegata a GPIO 20 (default) |
| **Sensore DS18B20** | Sensore temperatura 1-Wire | GPIO 21 + pull-up 4.7 kΩ a 3.3 V |
| **Modulo relè 4 canali** | Active-low, optoisolato | GPIO 28/29/30/31 (default) |
| **Alimentatore 5 V** | Per la striscia LED e il modulo relè | dimensionare in base al numero di LED |

### Componenti opzionali ma consigliati

| Componente | Descrizione | Note |
|---|---|---|
| **Waveshare 4-DSI-TOUCH-A** | Display IPS 720×720 MIPI-DSI + touch GT911 | collegato via connettore DSI on-board |
| **Valvola CO₂ + elettrovalvola** | Controllo CO₂ via relè | relè 3 (default) |
| **Riscaldatore acquario** | Controllato dal termostato firmware | relè 2 (default) |

### Schema pin di default

```
ESP32-P4-WiFi6 header DESTRO (lato LED/sensore):
  GPIO 20  ──►  WS2812B  DATA
  GPIO 21  ──►  DS18B20  DATA  (+ 4.7 kΩ pull-up a 3.3V)

ESP32-P4-WiFi6 header SINISTRO (lato relè):
  GPIO 28  ──►  Relè 1  (Filtro)
  GPIO 29  ──►  Relè 2  (Riscaldatore)
  GPIO 30  ──►  Relè 3  (CO₂)
  GPIO 31  ──►  Relè 4  (Pompa)

Display DSI (connettore on-board):
  GPIO  7  ──►  GT911 I2C SDA
  GPIO  8  ──►  GT911 I2C SCL
  Backlight: hardware-controlled (nessun GPIO aggiuntivo)

⚠️  GPIO 24 e GPIO 25 sono DM/DP USB – non usare!
```

> Tutti i pin sono modificabili da `idf.py menuconfig → Aquarium *`.

---

## 🖥️ Touch Display UI – dettaglio

Il display **Waveshare 4-DSI-TOUCH-A** (720 × 720 px, IPS, MIPI-DSI, touch capacitivo GT911)
mostra una dashboard LVGL v9 a **5 tab** con stile _dark IoT dashboard_.

### Palette colori

| Token | Hex | Utilizzo |
|---|---|---|
| `C_BG` | `#0B1E2D` | Sfondo – navy profondo |
| `C_CARD` | `#102A3D` | Card – `radius=14` |
| `C_PRIMARY` | `#1FA3FF` | Blu accent, arc indicatore |
| `C_YELLOW` | `#F1C40F` | Luci / scene giorno |
| `C_ORANGE` | `#F39C12` | Temperatura warning |
| `C_ON` | `#2ECC71` | Verde – OK / attivo |
| `C_ERR` | `#E74C3C` | Rosso – allarme / errore |
| `C_TEXT` | `#ECF5FB` | Bianco ghiaccio – testo |
| `C_MUTED` | `#5C7F9A` | Grigio – testo secondario |

### Status Bar (tutti i tab)

```
┌────────────────────────────────────────────────────────────────────────┐
│  09:34              ⚠  25.4°C   [✓  OK]                      WiFi    │
│  (ora locale)        (temp)  (badge verde/arancio/rosso)      (icona)  │
└────────────────────────────────────────────────────────────────────────┘
```

### Tab 0 – 🏠 Home

Panoramica 2×2 con aggiornamento ogni 2 s. Ogni card è cliccabile e naviga al tab relativo.

```
┌─────────────────────────────────┬──────────────────────────────────────┐
│  ⚠  TEMPERATURA                 │  ☰  LUCI                            │
│      25.4°C  [✓ OK]             │      80%  [ON]                      │
│  → Tab Temperatura              │  → Tab Luci                          │
├─────────────────────────────────┼──────────────────────────────────────┤
│  ↺  CO₂                         │  ↻  LIVELLO ACQUA                   │
│      OFF  Terminazione: --:--   │      OK  – Stato: Normale           │
│  → Tab Automazioni              │  (placeholder)                       │
└─────────────────────────────────┴──────────────────────────────────────┘
│  [ Home ]  [ Luci ]  [ Temp ]  [ Auto ]  [ Dati ]  ← tab bar (65px)  │
```

### Tab 1 – 💡 Luci

Slider luminosità + 4 bottoni scena.

```
  Switch ON/OFF   Slider brightness 0–100%   Scena: Alba | Giorno | Tramonto | Notte
```

### Tab 2 – 🌡 Temperatura

Arc gauge (15–40 °C) + spinbox target + stato riscaldatore/raffreddamento.

```
         ╭──────────╮         TARGET
        /  25.4°C   \         [−]  26.0  [+]   [ Salva ]
        │   ATTUALE  │
         ╰──────────╯
  ┌─ ⚠ RISCALDATORE ──┐   ┌─ ↺ RAFFREDDAMENTO ─┐
  │       OFF          │   │        OFF           │
  └────────────────────┘   └──────────────────────┘
```

### Tab 3 – ⚙ Automazioni

Toggle per ogni automazione (Luci, CO₂, Riscaldatore, 4 relè) + bottone **Avvia/Ferma Alimentazione**.

### Tab 4 – 📊 Dati

Grafico storico 24 h (48 punti) con selettore: **Temperatura** · **Luci** · **CO₂**.

### Overlay Allarme

Chiamabile da qualsiasi task via `display_ui_show_alarm(msg, detail)`:

```
  ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░
  ░░  ┌──────────────────────┐  ░░
  ░░  │    ⚠                 │  ░░
  ░░  │  TEMPERATURA ALTA!   │  ░░
  ░░  │  Attuale: 29.3 °C    │  ░░
  ░░  │ [DISATTIVA]  [ OK ]  │  ░░
  ░░  └──────────────────────┘  ░░
  ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░
```

---

## 🌐 Web UI – dettaglio

La Web UI è un'applicazione **PWA** (Progressive Web App) installabile su Android e iOS.
Il frontend (HTML + CSS + JS + SVG) è **embedded nel firmware** e servito direttamente dall'ESP32-P4
senza necessità di SD card o server esterno.

- **Sfondo**: scena subacquea SVG animata con bolle CSS
- **Glassmorphism cards**: `backdrop-filter: blur(12px)`
- **Aggiornamento real-time**: WebSocket push ogni 3 s
- **Autenticazione**: form login → cookie di sessione (24 h)

### Sezioni principali

| Sezione | Contenuto |
|---|---|
| **Riepilogo** | Stato sistema, uptime, IP, NTP, versione firmware, boot count |
| **LED Strip** | On/off, luminosità, colore RGB, preset, schedule, scena attiva |
| **Temperatura** | Gauge attuale, target heater, storico chart 24h, esporta CSV |
| **Automazioni** | Relè (manuale + schedule), heater, CO₂, modalità alimentazione |
| **Telegram** | Configura bot, test messaggio, promemoria cambio acqua/fertilizzante |
| **Manutenzione** | DuckDNS, OTA (URL o upload browser), timezone, factory reset, log eventi |

---

## 🌐 REST API

Tutti gli endpoint richiedono autenticazione (Basic Auth o cookie di sessione).

### Sistema
| Metodo | Endpoint | Descrizione |
|---|---|---|
| `GET` | `/api/health` | Ping – stato sistema |
| `GET` | `/api/status` | Stato completo JSON (tutti i moduli) |
| `GET` | `/api/events` | Log eventi (relay, scene, boot, allarmi) |
| `POST` | `/api/login` | Crea sessione cookie |
| `POST` | `/api/logout` | Invalida sessione |
| `POST` | `/api/auth` | Cambia username/password |
| `POST` | `/api/factory_reset` | Reset NVS e riavvio |

### LED
| Metodo | Endpoint | Descrizione |
|---|---|---|
| `GET` | `/api/leds` | Stato LED (on, R, G, B, brightness) |
| `POST` | `/api/leds` | Imposta LED (on/off, colore, brightness) |
| `GET` | `/api/led_schedule` | Legge schedule alba/tramonto |
| `POST` | `/api/led_schedule` | Aggiorna schedule |
| `GET` | `/api/led_presets` | Legge preset colore |
| `POST` | `/api/led_presets` | Applica preset colore |

### Scene LED
| Metodo | Endpoint | Descrizione |
|---|---|---|
| `GET` | `/api/scene` | Scena attiva + configurazione |
| `POST` | `/api/scene` | Avvia/ferma scena (`sunrise`/`sunset`/`moonlight`/`storm`/`clouds`/`none`) |

### Ciclo giornaliero
| Metodo | Endpoint | Descrizione |
|---|---|---|
| `GET` | `/api/daily_cycle` | Configurazione e fase attuale |
| `POST` | `/api/daily_cycle` | Aggiorna `enabled`, `lat`, `lon` |

### Temperatura
| Metodo | Endpoint | Descrizione |
|---|---|---|
| `GET` | `/api/temperature` | Temperatura attuale |
| `GET` | `/api/temperature_history` | Storico campioni JSON |
| `GET` | `/api/temperature/export.csv` | Esporta storico CSV |

### Relè
| Metodo | Endpoint | Descrizione |
|---|---|---|
| `GET` | `/api/relays` | Stato tutti i relè |
| `POST` | `/api/relays` | Imposta relè (index, on/off, nome, schedule) |

### Automazioni
| Metodo | Endpoint | Descrizione |
|---|---|---|
| `GET` | `/api/heater` | Configurazione auto-heater |
| `POST` | `/api/heater` | Aggiorna heater (target, isteresi, relay) |
| `GET` | `/api/co2` | Configurazione CO₂ |
| `POST` | `/api/co2` | Aggiorna CO₂ |
| `GET` | `/api/feeding` | Configurazione + stato modalità alimentazione |
| `POST` | `/api/feeding` | Configura / avvia / ferma alimentazione |

### Telegram
| Metodo | Endpoint | Descrizione |
|---|---|---|
| `GET` | `/api/telegram` | Config bot (token, chat_id, flag notifiche) |
| `POST` | `/api/telegram` | Aggiorna configurazione |
| `POST` | `/api/telegram_test` | Invia messaggio di test |
| `POST` | `/api/telegram_wc` | Promemoria cambio acqua |
| `POST` | `/api/telegram_fert` | Promemoria fertilizzante |

### Manutenzione
| Metodo | Endpoint | Descrizione |
|---|---|---|
| `GET` | `/api/duckdns` | Configurazione DuckDNS |
| `POST` | `/api/duckdns` | Aggiorna DuckDNS |
| `POST` | `/api/duckdns_update` | Forza aggiornamento IP |
| `POST` | `/api/ota` | Avvia OTA da URL remoto |
| `POST` | `/api/ota/upload` | Upload firmware dal browser (multipart) |
| `GET` | `/api/ota_status` | Stato aggiornamento OTA |
| `GET` | `/api/timezone` | Timezone configurata |
| `POST` | `/api/timezone` | Imposta timezone POSIX |
| `GET` | `/api/config/export` | Scarica configurazione JSON |
| `POST` | `/api/config/import` | Carica configurazione JSON |

### WebSocket
| URI | Descrizione |
|---|---|
| `ws://<ip>/ws` | Push stato JSON ogni 3 s a tutti i client connessi |

---

## ⚙️ Build & Flash

### Prerequisiti

- [ESP-IDF v6.0](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32p4/) installato e attivato
- Toolchain ESP32-P4 (`idf.py set-target esp32p4`)
- Python 3.8+

### Comandi

```bash
# Clona il repository
git clone https://github.com/Deddu3D/Aquarium-Controller-ESP32P4.git
cd Aquarium-Controller-ESP32P4

# Configura target e opzioni
idf.py set-target esp32p4
idf.py menuconfig

# Compila e flasha
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

### Configurazioni principali (`menuconfig`)

| Menu | Opzioni chiave |
|---|---|
| **Aquarium WiFi Settings** | SSID, password WiFi |
| **Aquarium Web Authentication** | username, password Web UI |
| **Aquarium Timezone Settings** | stringa POSIX timezone (default: `CET-1CEST,M3.5.0/2,M10.5.0/3`) |
| **Aquarium LED Strip Settings** | GPIO data, numero LED, luminosità default, durata ramp |
| **Aquarium Temperature Sensor Settings** | GPIO DS18B20, intervallo lettura, calibrazione |
| **Aquarium Relay Settings** | GPIO relè 1–4, polarità active-low |
| **Aquarium Display Settings** | abilita display, GPIO I2C touch (SDA=7, SCL=8), timeout backlight |
| **Aquarium Auto-Heater Settings** | timeout runaway protection |
| **Aquarium Feeding Mode Settings** | relè pausa, durata default |
| **Aquarium LED Scene Settings** | durata alba/tramonto, luminosità moonlight |
| **Aquarium Daily Cycle Settings** | latitudine × 10000, longitudine × 10000, durata moonlight |
| **Aquarium HTTPS Settings** | abilita TLS (default: off) |
| **Aquarium WebSocket Settings** | intervallo push (default: 3000 ms) |

> **Prima configurazione**: imposta almeno SSID + password WiFi. Le credenziali vengono salvate in NVS  
> e possono essere aggiornate in seguito anche via captive portal (AP mode) o Web UI.

---

## 🧱 Architettura

### Hardware target

| Componente | Dettaglio |
|---|---|
| MCU principale | ESP32-P4 (dual-core Xtensa LX7 @ 400 MHz, no WiFi integrato) |
| Coprocessore WiFi | ESP32-C6 (WiFi 6 / BLE 5, collegato via SDIO) |
| Board | Waveshare ESP32-P4-WiFi6 rev 1.3 |
| Flash | 16 MB (partizioni: NVS 24 KB + OTA dual 6 MB × 2) |
| Display | Waveshare 4-DSI-TOUCH-A – 720 × 720 px IPS MIPI-DSI, touch GT911 |

### Flusso di avvio

```
1. NVS init
2. Task Watchdog (45 s timeout)
3. WiFi manager (STA → captive portal AP se fallisce)
4. Timezone (NVS → default POSIX)
5. SNTP sync (max 15 s)
6. LED strip + schedule + scene + daily cycle
7. DS18B20 + history
8. Telegram
9. Relè + auto-heater + CO₂ + feeding mode
10. DuckDNS
11. Web server HTTP/HTTPS
12. Display UI (task separato: LVGL + MIPI-DSI HX8394 + GT911)
13. OTA validate (auto-rollback)
14. Main loop (tick moduli ogni 5–60 s)
```

### Moduli (`main/`)

| File | Responsabilità |
|---|---|
| `main.c` | Bootstrap e loop applicativo |
| `wifi_manager.*` | STA/AP, captive portal di provisioning |
| `web_server.*` | HTTP/HTTPS server, PWA embedded, 35+ endpoint REST + WebSocket |
| `display_ui.*` | Touch UI LVGL v9 (5 tab, dark IoT theme) |
| `led_controller.*` | WS2812B – on/off, RGB, brightness, fade |
| `led_schedule.*` | Schedule orario alba/tramonto con ramp |
| `led_scenes.*` | Engine 5 scene animate (FreeRTOS task) |
| `daily_cycle.*` | Ciclo luminoso giornaliero automatico |
| `sun_position.*` | Calcolo alba/tramonto NOAA |
| `temperature_sensor.*` | Polling DS18B20, media mobile |
| `temperature_history.*` | Storico campioni in-RAM |
| `relay_controller.*` | 4 relè GPIO, nomi NVS, schedule |
| `auto_heater.*` | Termostato automatico + runaway protection |
| `co2_controller.*` | Controller CO₂ con pre/post offset |
| `feeding_mode.*` | Pausa alimentazione a tempo |
| `telegram_notify.*` | Notifiche Telegram via HTTPS |
| `duckdns.*` | Aggiornamento DDNS |
| `ota_update.*` | OTA via URL HTTP e upload diretto |
| `timezone_manager.*` | POSIX timezone, lista preset, SNTP |
| `event_log.*` | Log eventi in-RAM (relay, scene, boot, allarmi) |

### Componente locale

| Componente | Descrizione |
|---|---|
| `components/esp_lcd_hx8394/` | Driver panel MIPI-DSI HX8394 (override del managed component) |

---

## 📁 Struttura repository

```text
.
├── CMakeLists.txt
├── partitions.csv              ← OTA dual (ota_0 + ota_1, 6 MB ciascuna)
├── sdkconfig.defaults
├── README.md
├── components/
│   └── esp_lcd_hx8394/        ← driver HX8394 locale
├── docs/
│   └── screenshots/
│       ├── web_ui_*.png
│       └── display_ui_*.png
└── main/
    ├── Kconfig.projbuild       ← tutte le opzioni menuconfig
    ├── idf_component.yml       ← dipendenze managed (lvgl, ds18b20, gt911, cjson, mdns…)
    ├── CMakeLists.txt
    ├── main.c
    ├── web_server.c/h          ← HTTP/HTTPS + REST + WebSocket
    ├── display_ui.c/h          ← LVGL v9 touch dashboard
    ├── led_controller.c/h
    ├── led_schedule.c/h
    ├── led_scenes.c/h
    ├── daily_cycle.c/h
    ├── sun_position.c/h
    ├── temperature_sensor.c/h
    ├── temperature_history.c/h
    ├── relay_controller.c/h
    ├── auto_heater.c/h
    ├── co2_controller.c/h
    ├── feeding_mode.c/h
    ├── event_log.c/h
    ├── telegram_notify.c/h
    ├── duckdns.c/h
    ├── ota_update.c/h
    ├── timezone_manager.c/h
    ├── wifi_manager.c/h
    ├── server.crt              ← certificato HTTPS embedded
    ├── server.key
    └── www/                    ← Web UI (embedded nel firmware via CMake EMBED_TXTFILES)
        ├── index.html
        ├── style.css
        ├── bg.svg
        ├── manifest.json
        └── sw.js
```

---

## 🛠️ Troubleshooting

| Problema | Soluzione |
|---|---|
| **WiFi non connesso** | Verificare SSID/password; connettersi a `AquariumSetup` per il captive portal |
| **Telegram non invia** | Controllare token e chat ID; verificare SNTP sincronizzato |
| **Temperatura 0 / errore** | Verificare DS18B20 e pull-up 4.7 kΩ su GPIO 21 |
| **OTA fallisce** | Verificare URL binario accessibile dalla rete locale, partizioni dual OTA presenti |
| **Display nero** | Verificare `CONFIG_DISPLAY_ENABLED=y` e rev chip ≥ v1.0 (`CONFIG_ESP32P4_REV_MIN_FULL=100`) |
| **Web UI non carica** | Il firmware include la UI embedded – nessuna SD card necessaria. Verificare connessione WiFi e autenticazione |
| **Scene non si avviano** | Verificare `CONFIG_LV_USE_ARC=y` e heap disponibile (≥ 100 kB) |
| **Daily cycle inattivo** | Verificare SNTP sincronizzato e `enabled=true` via `/api/daily_cycle` |
| **Bootloader rifiuta la board** | Aggiungere `CONFIG_ESP32P4_REV_MIN_FULL=100` a `sdkconfig.defaults` |

---

## 🔒 Note di sicurezza

- **Autenticazione abilitata di default** (username: `admin`, password: `aquarium`) — cambiare le credenziali prima di esporre su Internet.
- **HTTPS opzionale** (`CONFIG_AQUARIUM_HTTPS_ENABLE=y`): usa TLS con certificato self-signed embedded; il browser mostrerà un avviso iniziale da accettare.
- Per esposizione su Internet si raccomanda un **reverse proxy** (es. nginx) con certificato Let's Encrypt valido.
- Le credenziali Web UI vengono salvate in NVS e possono essere modificate via `POST /api/auth`.

---

## 📄 Licenza

Distribuito con licenza **MIT**.  
Vedere il file `LICENSE` per i dettagli.
