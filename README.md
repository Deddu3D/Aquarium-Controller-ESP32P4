# 🐟 Aquarium Controller – ESP32-P4

Controller completo per acquario su **Waveshare ESP32-P4-WiFi6** con:
- gestione **LED WS2812B** (scene animate, schedule alba/tramonto, ciclo giornaliero solare)
- monitoraggio **temperatura DS18B20** con storico, media mobile e log CSV
- controllo **4 relè** manuale e automatico
- modulo **Auto-Heater** (termostato) e controller **CO₂** (pre/post timing)
- **Modalità alimentazione** (pausa relè + dimmer LED a tempo)
- **Scene LED** (Alba, Tramonto, Chiaro di luna, Tempesta, Nuvole)
- **Ciclo luminoso giornaliero** automatico (calcolo alba/tramonto NOAA)
- **SD card** – backup config JSON e log CSV giornalieri
- notifiche **Telegram** (allarmi, promemoria, report, test)
- **Web UI REST** locale (dashboard + 35+ endpoint REST JSON)
- **Touch Display UI** LVGL v9 su display 720×720 (Waveshare 4-DSI-TOUCH-A)
- **OTA** via URL remoto o immagine su SD card
- **DuckDNS** per accesso remoto

> Stack: ESP-IDF v6.0 + ESP Hosted (P4 + C6 WiFi6) + HTTP/HTTPS server embedded.

---

## 📸 Screenshot aggiornati

### Web UI (desktop + mobile)

| Riepilogo | LED Strip |
|---|---|
| ![Web UI – Riepilogo](docs/screenshots/web_ui_riepilogo.png) | ![Web UI – LED Strip](docs/screenshots/web_ui_led_strip.png) |

| Telegram | Manutenzione |
|---|---|
| ![Web UI – Telegram](docs/screenshots/web_ui_telegram.png) | ![Web UI – Manutenzione](docs/screenshots/web_ui_manutenzione.png) |

| Mobile |
|---|
| ![Web UI – Mobile](docs/screenshots/web_ui_mobile.png) |

---

## 🖥️ Touch Display UI

Il display circolare **Waveshare 4-DSI-TOUCH-A** (720 × 720, IPS, MIPI-DSI, touch capacitivo GT911) mostra
una dashboard LVGL v9 a **5 tab** con stile _dark IoT dashboard_.

### Palette & stile

| Token | Valore hex | Utilizzo |
|---|---|---|
| `C_BG` | `#0B1E2D` | Sfondo pagina – navy profondo |
| `C_CARD` | `#102A3D` | Superficie card – `radius=14`, no border |
| `C_BORDER` | `#1A3A55` | Divisori sottili |
| `C_INPUT` | `#0D2236` | Superfici input / bottoni secondari |
| `C_PRIMARY` | `#1FA3FF` | Blu – accent primario, arc indicatore |
| `C_YELLOW` | `#F1C40F` | Giallo – luci / scene giorno |
| `C_ORANGE` | `#F39C12` | Arancione – temperatura warning |
| `C_TEXT` | `#ECF5FB` | Bianco ghiaccio – testo principale |
| `C_MUTED` | `#5C7F9A` | Grigio ardesia – testo secondario |
| `C_ON` | `#2ECC71` | Verde – OK / attivo (con glow shadow) |
| `C_ERR` | `#E74C3C` | Rosso – allarme / errore |
| `C_BAR_BG` | `#070F1A` | Sfondo status bar |

- **Card** `radius=14`, no border, padding 14 px
- **Switch** con alone verde (`shadow`) quando attivi
- **Slider** h=8 px, knob con shadow giallo
- **Spinbox** `[−][value][+]` con bottoni circolari r=22
- **Tab bar** in fondo (65 px), tab attivo evidenziato
- **Status bar** fissa in cima (48 px): ora | temperatura + badge stato | WiFi

---

### 🔝 Status Bar (presente in tutti i tab)

```
┌────────────────────────────────────────────────────────────────────────┐
│  09:34              ⚠  25.4°C   [✓  OK]                      WiFi    │
│  (ora locale)       (temp)  (badge verde/arancio/rosso)      (icona)  │
└────────────────────────────────────────────────────────────────────────┘
```

Il badge centrale diventa **⚠ ALLARME** (rosso) quando `display_ui_show_alarm()` è invocato.

---

### Tab 0 – 🏠 Home

Panoramica in tempo reale – aggiornamento ogni 2 s. Ogni card è **cliccabile** e naviga al relativo tab.

```
┌────────────────────────────────────────────────────────────────────────┐
│  STATUS BAR  09:34        ⚠ 25.4°C  [✓  OK]               WiFi       │
├─────────────────────────────────┬──────────────────────────────────────┤
│  ⚠  TEMPERATURA                 │  ☰  LUCI                            │
│                                 │                                      │
│     25.4°C  (Montserrat 28)     │     80%   (Montserrat 28)           │
│     [✓  OK]   (badge verde)     │     [ON]  (badge verde)             │
│                                 │                                      │
│  ← naviga Tab 2 →               │  ← naviga Tab 1 →                   │
├─────────────────────────────────┼──────────────────────────────────────┤
│  ↺  CO₂                         │  ↻  LIVELLO ACQUA                   │
│                                 │                                      │
│     OFF   (Montserrat 28)       │     OK    (Montserrat 28, verde)    │
│     Terminazione: --:--         │     Stato: Normale                  │
│                                 │                                      │
│  ← naviga Tab 3 →               │  (statico, placeholder)             │
└─────────────────────────────────┴──────────────────────────────────────┘
│  [ Home ]  [ Luci ]  [ Temp ]  [ Auto ]  [ Dati ]   ← tab bar (65px) │
└────────────────────────────────────────────────────────────────────────┘
```

- Temperatura: verde `C_ON` se 24–28 °C · arancione fuori range · grigio se errore sensore
- Badge CO₂: testo `ON` verde o `OFF` grigio in base al controller

---

### Tab 1 – 💡 Luci

Controllo luminosità manuale + selezione scena.

```
┌────────────────────────────────────────────────────────────────────────┐
│  STATUS BAR                                                            │
├────────────────────────────────────────────────────────────────────────┤
│  ┌─ LUCI ─────────────────────────────────── OFF  [○] ─────────────┐  │
│  └──────────────────────────────────────────────────────────────────┘  │
│                                                                        │
│  ┌─ Luminosità ──────────────────────────────────────────────────────┐ │
│  │  −                          80%                           +       │ │
│  │  ●══════════════════════════════════════════════════════○         │ │
│  │  (slider giallo, aggiornamento immediato)                         │ │
│  └───────────────────────────────────────────────────────────────────┘ │
│                                                                        │
│  ┌─ Scena ────────────────────────────────────────────────────────────┐ │
│  │  ┌─── Alba ───┐  ┌── Giorno ──┐  ┌─ Tramonto ─┐  ┌─ Notte ────┐ │ │
│  │  │  + (amber) │  │ ☰ (giallo) │  │ − (arancio)│  │ ↻ (blu)   │ │ │
│  │  │    Alba    │  │   Giorno   │  │  Tramonto  │  │   Notte   │ │ │
│  │  │    30%     │  │   100%     │  │    50%     │  │   10%     │ │ │
│  │  └────────────┘  └────────────┘  └────────────┘  └───────────┘ │ │
│  │  (tap: avvia LED_SCENE_SUNRISE / LED off+full / SUNSET / MOONLIGHT)│ │
│  └────────────────────────────────────────────────────────────────────┘ │
│  [ Home ]  [💡Luci]  [ Temp ]  [ Auto ]  [ Dati ]                     │
└────────────────────────────────────────────────────────────────────────┘
```

- Lo switch ON/OFF invoca `led_controller_fade_on/off()` con ramp Kconfig
- Il bottone scena selezionato riceve bordo colorato; bordo assente sugli altri
- Scene disponibili: `LED_SCENE_SUNRISE`, `LED_SCENE_SUNSET`, `LED_SCENE_MOONLIGHT`

---

### Tab 2 – 🌡 Temperatura

Arc gauge + target ± spinbox + stato riscaldatore/raffreddamento.

```
┌────────────────────────────────────────────────────────────────────────┐
│  STATUS BAR                                                            │
├────────────────────────────────────────────────────────────────────────┤
│               TEMPERATURA                                              │
│                                                                        │
│   ┌──────────────────────────┐          TARGET                        │
│   │     ╭──────────╮         │                                        │
│   │    /  25.4°C   \         │      [−]  26.0  [+]                   │
│   │   │   ATTUALE   │        │      (spinbox ×0.1 °C)                │
│   │    \           /         │                                        │
│   │     ╰──────────╯         │      [ Salva ]  (btn primario blu)    │
│   │  (arc 270°, blu→verde    │                                        │
│   │   se OK, arancio se OOB) │                                        │
│   └──────────────────────────┘                                        │
│                                                                        │
│   ┌─ ⚠ RISCALDATORE ──────────┐   ┌─ ↺ RAFFREDDAMENTO ─────────────┐ │
│   │        OFF                │   │          OFF                    │ │
│   └───────────────────────────┘   └─────────────────────────────────┘ │
│   (pill: rosso se ON, grigio scuro se OFF)                            │
│  [ Home ]  [ Luci ]  [🌡Temp]  [ Auto ]  [ Dati ]                    │
└────────────────────────────────────────────────────────────────────────┘
```

- Arc range: 15.0–40.0 °C (mappato su 150–400 ×10). Indicatore blu; rosso/arancione se allarme.
- "Salva" aggiorna `auto_heater_set_config()` con il target impostato.

---

### Tab 3 – ⚙ Automazioni

Lista scorrevole di automazioni + controllo manuale relè + bottone alimentazione.

```
┌────────────────────────────────────────────────────────────────────────┐
│  STATUS BAR                                                            │
├────────────────────────────────────────────────────────────────────────┤
│  AUTOMAZIONI                                                           │
│                                                                        │
│  ┌─ ☰  Luci ──────────────────── 08:00 – 22:00  ·  Tutti i giorni ─[●]─┐ │
│  └────────────────────────────────────────────────────────────────────┘ │
│                                                                        │
│  ┌─ ↺  CO₂ ───────────────────── 07:30 – 22:10  ·  Tutti i giorni ─[●]─┐ │
│  └────────────────────────────────────────────────────────────────────┘ │
│                                                                        │
│  ┌─ ⚠  Riscaldatore ──── Target: 26.0°C  ·  Termostato auto ────────[●]─┐ │
│  └────────────────────────────────────────────────────────────────────┘ │
│                                                                        │
│  ┌─ ⚙  Filtro ────────────────────────────────── Relè 1 ───────────[●]─┐ │
│  ┌─ ⚙  Riscaldatore ──────────────────────────── Relè 2 ───────────[○]─┐ │
│  ┌─ ⚙  CO₂ ───────────────────────────────────── Relè 3 ───────────[●]─┐ │
│  ┌─ ⚙  Pompa ─────────────────────────────────── Relè 4 ───────────[○]─┐ │
│                                                                        │
│  ┌───────────────────────────────────────────────────────────────────┐ │
│  │  ▶  Avvia Alimentazione                   (verde, h=60px)        │ │
│  └───────────────────────────────────────────────────────────────────┘ │
│  [ Home ]  [ Luci ]  [ Temp ]  [⚙Auto]  [ Dati ]                     │
└────────────────────────────────────────────────────────────────────────┘
```

- Ogni toggle aggiorna immediatamente il modulo corrispondente (persiste in NVS).
- Il bottone Alimentazione diventa **■ Ferma Alimentazione** (sfondo rosso scuro) se la modalità è attiva.
- I nomi dei relè mostrano il valore attuale da `relay_controller_get_name()`.

---

### Tab 4 – 📊 Dati

Grafico storico a 48 punti (= 24 h × 30 min per punto) con selettore canale.

```
┌────────────────────────────────────────────────────────────────────────┐
│  STATUS BAR                                                            │
├────────────────────────────────────────────────────────────────────────┤
│  DATI                                                                  │
│                                                                        │
│  [ TEMPERATURA ]   [   LUCI   ]   [   CO₂   ]   ← bottoni selettore  │
│  (blu attivo)      (grigio)        (grigio)                           │
│                                                                        │
│  ┌────────────────────────────────────────────────────────────────┐   │
│  │ 32.0 ┤                                                         │   │
│  │      │                   ╭──╮                                  │   │
│  │ 26.0 ┤     ╭─────────────╯  ╰──────────────────╮              │   │
│  │      │    /                                      \             │   │
│  │ 20.0 ┤───╯                                        ╰─────       │   │
│  │      └─────────────────────────────────────────────────────    │   │
│  │      -24h          -18h          -12h          -6h          0h │   │
│  └────────────────────────────────────────────────────────────────┘   │
│                                                                        │
│  TEMPERATURA → line chart (blu), range 18–32 °C,                     │
│  LUCI        → bar chart (giallo), range 0–100 % schedule             │
│  CO₂         → bar chart (verde), finestra attiva CO₂                 │
│  [ Home ]  [ Luci ]  [ Temp ]  [ Auto ]  [📊Dati]                    │
└────────────────────────────────────────────────────────────────────────┘
```

---

### 🚨 Overlay Allarme

Chiamabile da qualsiasi task via `display_ui_show_alarm(msg, detail)`.

```
┌────────────────────────────────────────────────────────────────────────┐
│  STATUS BAR   ⚠ ALLARME  (badge rosso)                                │
│                                                                        │
│  ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ (overlay 50%)   │
│  ░░  ┌────────────────────────────────────────────┐  ░░              │
│  ░░  │              ⚠                             │  ░░              │
│  ░░  │   TEMPERATURA TROPPO ALTA!                 │  ░░              │
│  ░░  │   Attuale: 29.3°C  (dettaglio muted)       │  ░░              │
│  ░░  │                                            │  ░░              │
│  ░░  │  [ DISATTIVA ALLARME ]   [  OK  ] (rosso)  │  ░░              │
│  ░░  └────────────────────────────────────────────┘  ░░              │
│  ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ (overlay 50%)   │
└────────────────────────────────────────────────────────────────────────┘
```

- **DISATTIVA ALLARME**: nasconde l'overlay ma mantiene il badge rosso nella status bar
- **OK**: nasconde l'overlay e ripristina il badge verde `✓ OK`

---

## ✨ Funzionalità principali

- **WiFi STA + AP fallback** (captive portal di configurazione)
- **Web dashboard** con controlli in tempo reale
- **REST API JSON** per integrazione esterna (35+ endpoint)
- **LED control avanzato**
  - on/off rapido con ramp configurabile
  - RGB + luminosità (0–255)
  - schedule alba/tramonto con ramp
  - preset colore e scena
- **Scene LED animate** (engine FreeRTOS, tick 500 ms)
  - `SUNRISE` – ramp alba: ambra caldo → bianco giorno
  - `SUNSET` – ramp tramonto: bianco → ambra → spento
  - `MOONLIGHT` – luce lunare blu tenue
  - `STORM` – flickering casuale con lampeggi
  - `CLOUDS` – variazione sinusoidale lenta della luminosità
- **Ciclo luminoso giornaliero automatico** (`daily_cycle`)
  - fasi: Night → Sunrise → Morning → Noon → Afternoon → Sunset → Evening
  - calcolo alba/tramonto NOAA da lat/lon configurabili in NVS
  - ricalcolo automatico ogni giorno
- **Temperatura acqua**
  - polling periodico DS18B20
  - media mobile
  - storico giornaliero (esportazione CSV via HTTP)
  - log CSV giornaliero su SD card
- **Relè**
  - comando manuale
  - nomi personalizzati (persistiti in NVS)
- **Auto-Heater (termostato)**
  - accensione/spegnimento automatico per relè configurabile
  - target e isteresi impostabili
- **CO₂**
  - programmazione oraria con pre-anticipo ON e post-ritardo OFF rispetto allo schedule luci
- **Modalità alimentazione** (`feeding_mode`)
  - pausa relè configurabile per N minuti (1–60)
  - dimmer LED opzionale durante la pausa
  - notifica Telegram a inizio e fine sessione
- **SD Card** (microSD via SDMMC slot 1)
  - mount FAT, crea directory `/sdcard/logs/` e `/sdcard/config/`
  - backup/restore configurazione completa in JSON
  - log temperatura CSV giornaliero
  - log eventi (relay, feeding, CO₂, scene) CSV giornaliero
  - log notifiche Telegram giornaliero
  - log diagnostico WARN/ERR giornaliero
  - OTA da immagine `firmware.bin` sulla SD card
- **Telegram bot**
  - eventi relè
  - allarmi temperatura
  - test messaggio
  - promemoria cambio acqua / fertilizzante
- **Touch Display UI** LVGL v9 su display 720×720
  - 5 tab: Home / Luci / Temperatura / Automazioni / Dati
  - card 2×2 panoramica con navigazione rapida al tab
  - arc gauge temperatura con colore adattivo
  - slider luminosità + 4 bottoni scena (Alba/Giorno/Tramonto/Notte)
  - spinbox target temperatura + pulsante salva
  - toggle automazioni (Luci, CO₂, Riscaldatore, 4 relè)
  - bottone Avvia/Ferma Alimentazione
  - grafico storico 24h (Temperatura/Luci/CO₂) con selettore
  - overlay allarme modale con badge status bar
- **OTA** via URL HTTP remoto
- **DuckDNS** per accesso remoto con aggiornamento IP automatico
- **HTTPS opzionale** (certificato embedded)
- **Fuso orario POSIX** con SNTP e lista timezone configurabili

---

## 🧱 Architettura

### Hardware target
- **MCU principale**: ESP32-P4
- **Coprocessore**: ESP32-C6 (WiFi 6 / BLE 5 via SDIO)
- **Board**: Waveshare ESP32-P4-WiFi6 rev 1.3

### Flusso di avvio (semplificato)
1. NVS init + Task WDT
2. SD card init (non fatale)
3. SD logger init
4. WiFi manager init
5. SNTP + timezone
6. init moduli (LED, scene, daily cycle, sensore, Telegram, relè, heater, CO₂, feeding, DuckDNS)
7. avvio Web server
8. **display UI init** (task separato: LVGL + MIPI-DSI HX8394 + GT911 touch)
9. loop applicativo (tick moduli ogni ~10 s)

### Moduli principali (`main/`)

| File | Responsabilità |
|---|---|
| `main.c` | bootstrap, orchestrazione loop applicativo |
| `wifi_manager.*` | STA/AP, captive portal di provisioning |
| `web_server.*` | HTTP/HTTPS server, dashboard + 35+ endpoint REST |
| `display_ui.*` | touch UI LVGL v9 (5 tab, dark IoT theme) |
| `led_controller.*` | WS2812B – on/off, RGB, brightness, fade |
| `led_schedule.*` | schedule orario alba/tramonto con ramp |
| `led_scenes.*` | engine 5 scene animate (FreeRTOS task) |
| `daily_cycle.*` | ciclo luminoso giornaliero da coordinate GPS |
| `sun_position.*` | calcolo alba/tramonto NOAA |
| `temperature_sensor.*` | polling DS18B20, media mobile |
| `temperature_history.*` | storico campioni in-RAM |
| `relay_controller.*` | 4 relè GPIO, nomi NVS |
| `auto_heater.*` | termostato automatico |
| `co2_controller.*` | programmazione CO₂ con pre/post offset |
| `feeding_mode.*` | pausa alimentazione a tempo |
| `telegram_notify.*` | notifiche Telegram via HTTPS |
| `duckdns.*` | aggiornamento DDNS |
| `ota_update.*` | OTA via URL HTTP |
| `sd_card.*` | mount SDMMC, backup/restore config JSON |
| `sd_logger.*` | log CSV giornalieri su SD card |
| `timezone_manager.*` | POSIX timezone, lista preset |

---

## 📌 Pin di default (Kconfig)

### Periferiche
- LED strip data: **GPIO 20**
- DS18B20 data: **GPIO 21**
- Relay 1..4: **GPIO 28 / 29 / 30 / 31** (lato sinistro, opposto a LED/sensore)
- Polarità relè: **active-low** (default, tipico moduli optoisolati)
- **Display MIPI-DSI** touch I2C: SCL **GPIO 8**, SDA **GPIO 7** · Backlight: hardware-controlled (GPIO -1)

> Tutti i valori sono modificabili da `idf.py menuconfig`.

> ⚠️ **Non usare GPIO24/GPIO25**: su questa board sono DM/DP del bus USB; il loro uso causa conflitti con la programmazione e l'USB device.

> 📌 **GPIO disponibili sull'header (Waveshare ESP32-P4-WiFi6)**:  
> Lato destro (LED/sensore): 20, 21, 22, 23, 26, 27, 32, 33, 46, 47, 48  
> Lato sinistro (relè): 2, 3, 4, 5, **28, 29, 30, 31**, 49, 50, 51, 52 · 7(SDA) · 8(SCL riservati touch)

---

## ⚙️ Configurazione

### Prerequisiti
- ESP-IDF installato e attivato in shell
- Toolchain per target ESP32-P4

### Build & flash
```bash
idf.py set-target esp32p4
idf.py menuconfig
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

### Configurazioni importanti (`menuconfig`)
- **Aquarium WiFi Settings**
- **Aquarium Timezone Settings**
- **Aquarium LED Strip Settings**
- **Aquarium Temperature Sensor Settings**
- **Aquarium Relay Settings**
- **Aquarium Auto-Heater Settings**
- **Aquarium CO₂ Controller Settings**
- **Aquarium Feeding Mode Settings**
- **Aquarium LED Scene Settings**
- **Aquarium Daily Cycle Settings**
- **SD Card Settings** (`CONFIG_SD_CARD_ENABLED`, bus width/speed, pin CLK/CMD/D0..D3, internal LDO SD)
- **Display Settings** (`CONFIG_DISPLAY_ENABLED`, `CONFIG_TOUCH_I2C_SCL/SDA`)
- **Aquarium Telegram Settings**
- **Aquarium DuckDNS Settings**
- **Aquarium HTTPS Settings**

---

## 🌐 REST API (endpoint completi)

### Sistema
| Metodo | Endpoint | Descrizione |
|---|---|---|
| `GET` | `/api/health` | Stato sistema (ping) |
| `GET` | `/api/status` | Stato completo JSON |

### LED
| Metodo | Endpoint | Descrizione |
|---|---|---|
| `GET` | `/api/leds` | Stato LED (on, R, G, B, brightness) |
| `POST` | `/api/leds` | Imposta LED (on/off, colore, brightness) |
| `GET` | `/api/led_schedule` | Legge schedule luci |
| `POST` | `/api/led_schedule` | Aggiorna schedule luci |
| `GET` | `/api/led_presets` | Legge preset LED |
| `POST` | `/api/led_presets` | Applica preset LED |

### Scene LED
| Metodo | Endpoint | Descrizione |
|---|---|---|
| `GET` | `/api/scene` | Scena attiva + configurazione |
| `POST` | `/api/scene` | Avvia/ferma scena (sunrise/sunset/moonlight/storm/clouds/none) |

### Ciclo giornaliero
| Metodo | Endpoint | Descrizione |
|---|---|---|
| `GET` | `/api/daily_cycle` | Configurazione e fase attuale |
| `POST` | `/api/daily_cycle` | Aggiorna enabled, lat, lon |

### Temperatura
| Metodo | Endpoint | Descrizione |
|---|---|---|
| `GET` | `/api/temperature` | Temperatura attuale |
| `GET` | `/api/temperature_history` | Storico campioni JSON |
| `GET` | `/api/temperature/export.csv` | Esporta storico in CSV |

### Relè
| Metodo | Endpoint | Descrizione |
|---|---|---|
| `GET` | `/api/relays` | Stato tutti i relè |
| `POST` | `/api/relays` | Imposta relè (index, on/off, nome) |

### Automazioni
| Metodo | Endpoint | Descrizione |
|---|---|---|
| `GET` | `/api/heater` | Config auto-heater |
| `POST` | `/api/heater` | Aggiorna auto-heater |
| `GET` | `/api/co2` | Config CO₂ controller |
| `POST` | `/api/co2` | Aggiorna CO₂ controller |
| `GET` | `/api/feeding` | Config + stato modalità alimentazione |
| `POST` | `/api/feeding` | Config / avvia / ferma modalità alimentazione |

### Telegram
| Metodo | Endpoint | Descrizione |
|---|---|---|
| `GET` | `/api/telegram` | Config bot (token, chat_id, flags) |
| `POST` | `/api/telegram` | Aggiorna config bot |
| `POST` | `/api/telegram_test` | Invia messaggio di test |
| `POST` | `/api/telegram_wc` | Invia promemoria cambio acqua |
| `POST` | `/api/telegram_fert` | Invia promemoria fertilizzante |

### SD Card
| Metodo | Endpoint | Descrizione |
|---|---|---|
| `GET` | `/api/sdcard` | Stato SD (montata, spazio, nome) |
| `GET` | `/api/sdcard/ls` | Lista file in una directory |
| `GET` | `/api/sdcard/download` | Scarica file dalla SD card |
| `DELETE` | `/api/sdcard/delete` | Elimina file dalla SD card |
| `POST` | `/api/sdcard/config/export` | Esporta config JSON su SD |
| `POST` | `/api/sdcard/config/import` | Importa config JSON da SD |

### Manutenzione
| Metodo | Endpoint | Descrizione |
|---|---|---|
| `GET` | `/api/duckdns` | Config DuckDNS |
| `POST` | `/api/duckdns` | Aggiorna config DuckDNS |
| `POST` | `/api/duckdns_update` | Forza aggiornamento IP DuckDNS |
| `POST` | `/api/ota` | Avvia OTA da URL remoto |
| `GET` | `/api/ota_status` | Stato aggiornamento OTA |
| `POST` | `/api/ota/sd` | Avvia OTA da `firmware.bin` su SD card |
| `GET` | `/api/timezone` | Timezone configurata |
| `POST` | `/api/timezone` | Imposta timezone POSIX |

---

## 🔒 Sicurezza

- Se abiliti HTTPS (`AQUARIUM_HTTPS_ENABLE`), usa connessioni TLS sulla LAN.
- Certificato self-signed predefinito: il browser mostrerà warning iniziale.
- Per ambienti esposti su Internet, consigliato reverse proxy con certificati validi.

---

## 🛠️ Troubleshooting rapido

- **WiFi non connesso** → verificare SSID/password o usare AP setup.
- **Telegram non invia** → controllare token/chat ID e ora SNTP sincronizzata.
- **Temperatura nulla** → verificare DS18B20 e pull-up 4.7 kΩ su GPIO 21.
- **OTA fallisce** → verificare URL binario, rete e spazio partizioni.
- **SD card non montata** → verificare formattazione FAT32, pin CLK/CMD/D0 e `CONFIG_SD_CARD_ENABLED=y`.
- **Display nero** → verificare `CONFIG_DISPLAY_ENABLED=y` e rev chip ≥ v1.0 (`CONFIG_ESP32P4_REV_MIN_FULL=100`).
- **Scene non si avviano** → verificare `CONFIG_LV_USE_ARC=y` e heap disponibile (≥ 100 kB raccomandati).
- **Daily cycle inattivo** → verificare ora SNTP sincronizzata e `enabled=true` in `/api/daily_cycle`.

---

## 📁 Struttura repository

```text
.
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
├── README.md
├── components/
│   └── esp_lcd_hx8394/        ← driver HX8394 locale (override managed component)
├── docs/
│   └── screenshots/
│       ├── web_ui_*.png        ← screenshot Web UI
│       └── display_ui_*.png   ← screenshot display (hardware reale)
└── main/
    ├── Kconfig.projbuild       ← tutte le opzioni menuconfig
    ├── idf_component.yml       ← dipendenze managed (lvgl, cjson, gt911, hx8394…)
    ├── CMakeLists.txt
    ├── main.c                  ← bootstrap + loop applicativo
    ├── web_server.c/h          ← HTTP/HTTPS + 35+ endpoint REST
    ├── display_ui.c/h          ← LVGL v9 touch dashboard (5 tab)
    ├── led_controller.c/h
    ├── led_schedule.c/h
    ├── led_scenes.c/h          ← engine scene animate (5 scene)
    ├── daily_cycle.c/h         ← ciclo luminoso giornaliero NOAA
    ├── sun_position.c/h        ← calcolo alba/tramonto
    ├── temperature_sensor.c/h
    ├── temperature_history.c/h
    ├── relay_controller.c/h
    ├── auto_heater.c/h
    ├── co2_controller.c/h
    ├── feeding_mode.c/h        ← modalità alimentazione a tempo
    ├── telegram_notify.c/h
    ├── sd_card.c/h             ← SD card mount + config backup
    ├── sd_logger.c/h           ← log CSV/log giornalieri su SD
    ├── duckdns.c/h
    ├── ota_update.c/h
    ├── timezone_manager.c/h
    ├── wifi_manager.c/h
    ├── server.crt              ← certificato HTTPS embedded
    └── server.key
```

---

## 📄 Licenza

Questo progetto è distribuito con licenza **MIT**.
