# 🐟 Aquarium Controller – ESP32-P4

<p align="center">
  <strong>Controller completo per acquario basato su ESP32-P4</strong><br>
  Display touch MIPI DSI · LED WS2812B · DS18B20 · Termostato · Telegram · DuckDNS · OTA · 4 Relè
</p>

---

| | |
|---|---|
| **Scheda** | [Waveshare ESP32-P4-WiFi6](https://www.waveshare.com/esp32-p4-wifi6.htm?sku=32020) rev 1.3 |
| **MCU principale** | ESP32-P4 (dual-core, 400 MHz, 32 MB PSRAM, 16 MB Flash) |
| **Coprocessore WiFi** | ESP32-C6 (WiFi 6 / BLE 5) collegato via **SDIO** |
| **Display** | 5″ MIPI DSI 800×480 ILI9881C + touch capacitivo GT911 |
| **Framework** | ESP-IDF v6.0.0 |
| **Grafica** | LVGL v9.x (double-buffered PSRAM) |
| **IDE** | VS Code + estensione ESP-IDF |

---

## 📸 Screenshots

### Display LVGL (5″ 800×480)

Dashboard on-screen con 4 tab a scorrimento (stessa struttura della Web UI): **Riepilogo** (temperatura, WiFi, scena LED, relè), **LED Strip** (scena, luminosità, RGB, configurazione), **Telegram** (stato notifiche, promemoria), **Manutenzione** (WiFi/IP, heap, uptime, termostato, DuckDNS). Refresh ogni 10s, touch capacitivo GT911.

![Display LVGL Dashboard](https://github.com/user-attachments/assets/97c94714-cc51-4dab-95f7-3573bac166da)

### Web UI – 4 Tab

Dashboard web responsive con 4 tab: **Riepilogo**, **LED Strip**, **Telegram**, **Manutenzione**. Accessibile da qualsiasi browser sulla rete locale. Auto-refresh ogni 2s per dati real-time.

#### Tab Riepilogo

Temperatura in tempo reale, grafico storico 24h, stato sensore, WiFi RSSI, scena LED con campione colore RGB, stato 4 relè con toggle, regola termostato, fase solare.

![Web UI – Riepilogo](docs/screenshots/web_ui_riepilogo.png)

#### Tab LED Strip

Controllo luminosità e colore, selezione scena (8 scene), configurazione avanzata (durate, siesta, temperatura colore, fase lunare), geolocalizzazione per ciclo giornaliero.

![Web UI – LED Strip](docs/screenshots/web_ui_led_strip.png)

#### Tab Telegram

Configurazione bot token e chat ID, allarmi temperatura, promemoria manutenzione (cambio acqua, fertilizzante), riepilogo giornaliero, messaggio di test.

![Web UI – Telegram](docs/screenshots/web_ui_telegram.png)

#### Tab Manutenzione

Stato sistema (WiFi, IP, SSID, RSSI, heap, uptime), aggiornamento firmware OTA con progress bar, configurazione termostato automatico, DuckDNS DDNS, controllo 4 relè con nomi e toggle.

![Web UI – Manutenzione](docs/screenshots/web_ui_manutenzione.png)

#### Versione mobile

![Web UI – Mobile](docs/screenshots/web_ui_mobile.png)

---

## ✨ Funzionalità

| Categoria | Descrizione |
|-----------|-------------|
| 🖥️ **Display touch MIPI DSI** | Schermo 5″ 800×480 con dashboard LVGL a 4 tab (stessa struttura della Web UI): temperatura, grafico 24h, scena LED, stato WiFi, 4 relè. Refresh ogni 10s, touch capacitivo GT911. Init non bloccante su CPU 1 |
| 🐟 **Illuminazione LED WS2812B** | Controllo colore RGB con correzione gamma 2.2, **8 scene** predefinite + ciclo giornaliero automatico basato su alba/tramonto reali con ripresa dalla fase corrente. Fade-in/fade-out graduali (default 30s). Siesta anti-alghe configurabile |
| 🌡️ **Sensore temperatura DS18B20** | Lettura ogni 5s con media mobile a 3 campioni, storico 24h (288 campioni), grafico su display e web, esportazione CSV, offset di calibrazione configurabile |
| 🔥 **Termostato automatico** | Controllo termostatico relè + DS18B20 con isteresi configurabile (0.1–3.0 °C), target 15–35 °C, logica anti-oscillazione |
| 📱 **Notifiche Telegram** | Allarmi temperatura (alta/bassa), promemoria cambio acqua e fertilizzante con intervallo configurabile, riepilogo giornaliero programmabile. TLS con certificato embedded |
| 🔌 **4 Relè configurabili** | Nomi personalizzabili (32 char), stato persistente NVS, **scheduling time-of-day** con granularità 1 minuto e supporto finestre overnight |
| 🌐 **DuckDNS DDNS** | Aggiornamento automatico IP ogni 5 minuti con stato diagnostico |
| 📡 **WiFi con riconnessione** | Backoff esponenziale (1s → 60s cap), riconnessione infinita in background, timeout init 30s non bloccante |
| 📊 **Dashboard web** | UI responsive con 4 tab, tema scuro, grafico canvas temperatura, auto-refresh ogni 2s, campione colore RGB in tempo reale, interfaccia italiana |
| 🔄 **OTA Firmware Update** | Aggiornamento firmware via HTTP/HTTPS in background con progress tracking e reboot automatico 3s dopo completamento |
| 🐕 **Task Watchdog** | Watchdog 30s con panic e reboot automatico in caso di blocco |
| 📦 **Persistenza NVS** | Tutte le configurazioni salvate in flash: scene LED, relè, termostato, Telegram, DuckDNS, geolocalizzazione |

### Scene LED disponibili

| Scena | Descrizione |
|-------|-------------|
| `off` | LED spenti, controllo manuale |
| `daylight` | Luce diurna statica con temperatura colore configurabile (6500–20000 K) |
| `sunrise` | Transizione graduale alba (durata configurabile 1–120 min) |
| `sunset` | Transizione graduale tramonto (durata configurabile 1–120 min) |
| `moonlight` | Luce lunare blu tenue con modulazione opzionale fase lunare reale |
| `cloudy` | Onde sinusoidali di luminosità (effetto nuvole) |
| `storm` | Base scura con lampi casuali (effetto temporale) |
| `full_day_cycle` | Ciclo completo 24h automatico: alba → luce diurna → (siesta) → tramonto → luna. Orari reali via geolocalizzazione + SNTP. Ripresa automatica dalla fase corrente dopo riavvio |

---

## 🏗️ Architettura

```
┌─────────────────┐    SDIO (4-bit)    ┌─────────────────┐
│    ESP32-P4      │◄──────────────────►│    ESP32-C6      │──── WiFi 6 antenna
│    (host MCU)    │  CLK=18 CMD=19     │    (slave)       │
│    dual-core     │  D0-D3=14-17       │    WiFi/BLE      │
│    32MB PSRAM    │  RST=54            │                   │
└────────┬────────┘                     └──────────────────┘
         │
         │  MIPI DSI (2 data lane, 500 Mbps)
         ▼
┌─────────────────┐     ┌──────────┐     ┌─────────┐
│  5″ LCD 800×480 │     │ DS18B20  │     │ WS2812B │
│  ILI9881C +     │     │ (GPIO 21)│     │ (GPIO 8)│
│  GT911 touch    │     │ 1-Wire   │     │ 105 LED │
└─────────────────┘     └──────────┘     └─────────┘
         │
    ┌────┴────────────────────────────┐
    │  4× Relè (GPIO 22-25)          │
    │  Active-low configurabile       │
    └─────────────────────────────────┘
```

L'ESP32-P4 utilizza le API standard `esp_wifi_*` di ESP-IDF.
Il componente **esp_wifi_remote** (+ **esp_hosted**) intercetta le chiamate e le
delega al coprocessore C6 attraverso il bus SDIO, in modo completamente
trasparente per l'applicazione.

### Gestione dual-core

| CPU | Ruolo |
|-----|-------|
| **CPU 0** | Applicazione principale: web server, sensori, relè, LED, Telegram, main loop |
| **CPU 1** | Init display MIPI DSI (busy-wait ILI9881C), LVGL handler task |

Il display init è eseguito su CPU 1 tramite `xTaskCreatePinnedToCore()` per evitare
che il polling busy-wait del pannello DSI blocchi CPU 0 e faccia scattare il watchdog.

### Sequenza di avvio

1. **NVS** – Inizializzazione Non-Volatile Storage
2. **Watchdog** – Timer 30s con panic
3. **WiFi** – Connessione via ESP32-C6 (30s timeout, retry in background)
4. **Geolocalizzazione** – Caricamento lat/lng/UTC da NVS (default: Roma)
5. **Timezone + SNTP** – Sincronizzazione orologio (max 15s)
6. **LED Strip** – Init WS2812B + avvio scene engine
7. **Temperatura** – Init DS18B20 + avvio storico 24h
8. **Telegram** – Init notifiche con task di monitoraggio
9. **Relè** – Configurazione GPIO + ripristino stato da NVS
10. **Auto-Heater** – Caricamento configurazione termostato
11. **DuckDNS** – Avvio aggiornamenti DNS periodici
12. **Display** – Init MIPI DSI + LVGL (task background su CPU 1)
13. **HTTP Server** – Avvio web server porta 80

**Main loop** (ogni 10s): controllo WiFi → tick relè schedules → tick termostato → refresh display UI → feed watchdog

### Moduli software

| Modulo | File | Descrizione |
|--------|------|-------------|
| WiFi Manager | `wifi_manager.h/.c` | Connessione STA con backoff esponenziale (1s → 60s) |
| LED Controller | `led_controller.h/.c` | Driver WS2812B thread-safe: mutex, gamma LUT 2.2, fade ramp, lock/unlock API |
| LED Scenes | `led_scenes.h/.c` | Engine 8 scene + ciclo giornaliero con alba/tramonto, siesta anti-alghe, fase lunare, ripresa dalla fase corrente |
| Temperature | `temperature_sensor.h/.c` | DS18B20 con media mobile 3 campioni e calibrazione offset |
| Temp History | `temperature_history.h/.c` | Ring buffer 24h (288 campioni × 5 min) |
| Telegram | `telegram_notify.h/.c` | Bot API TLS, allarmi, promemoria, riepilogo giornaliero, tracking manutenzione |
| Relay Controller | `relay_controller.h/.c` | 4 relè GPIO con mutex, stato NVS, nomi custom, scheduling time-of-day |
| Auto Heater | `auto_heater.h/.c` | Termostato con isteresi configurabile (relè + DS18B20) |
| DuckDNS | `duckdns.h/.c` | Client DDNS con aggiornamento periodico (5 min), diagnostica stato |
| OTA Update | `ota_update.h/.c` | Aggiornamento firmware HTTP/HTTPS con progress % e reboot automatico |
| Display Driver | `display_driver.h/.c` | MIPI DSI 2-lane + ILI9881C + GT911 I2C + LVGL 9.x init + PSRAM draw buffers |
| Display UI | `display_ui.h/.c` | Dashboard LVGL a 4 tab: Riepilogo, LED Strip, Telegram, Manutenzione (layout speculare alla Web UI) |
| Geolocation | `geolocation.h/.c` | Lat/lng/UTC per calcolo alba/tramonto (default: Roma, mutex-protected) |
| Sun Position | `sun_position.h/.c` | Algoritmo NOAA semplificato per alba/tramonto (±1 min accuratezza) |
| Web Server | `web_server.h/.c` | HTTP server porta 80 con dashboard 4-tab e 26 endpoint REST API |

### Dipendenze esterne (ESP Component Registry)

| Componente | Versione | Uso |
|-----------|----------|-----|
| `espressif/esp_hosted` | ^2.0.0 | Trasporto SDIO host ↔ slave |
| `espressif/esp_wifi_remote` | ^1.0.0 | API WiFi delegata al C6 |
| `espressif/led_strip` | ^3.0.0 | Driver WS2812B via RMT |
| `espressif/ds18b20` | ^0.3.0 | Sensore temperatura 1-Wire via RMT |
| `espressif/esp_lcd_ili9881c` | ^1.0.0 | Driver pannello LCD MIPI DSI |
| `espressif/esp_lcd_touch_gt911` | ^1.0.0 | Driver touch capacitivo I2C |
| `lvgl/lvgl` | ^9.0.0 | Libreria grafica per display embedded |
| `idf` | ≥5.5.0 | Versione minima ESP-IDF |

### Thread safety

| Modulo | Meccanismo | Note |
|--------|-----------|------|
| LED Controller | FreeRTOS mutex (`s_mutex`) | `lock()`/`unlock()` per accesso multi-step da moduli esterni |
| LED Scenes | Mutex interno | Scene switching thread-safe |
| Relay Controller | FreeRTOS mutex (`s_mutex`) | Tutte le API pubbliche acquisiscono il mutex |
| Display | LVGL API lock | `display_lock()`/`display_unlock()` per accesso thread-safe |
| Geolocation | FreeRTOS mutex | Lettura/scrittura config protetta |
| Temperature | Lettura atomica | Getter thread-safe con flag di validità |

---

## 📌 Pin mapping

### SDIO (Waveshare ESP32-P4-WiFi6)

| Funzione SDIO | GPIO ESP32-P4 |
|---------------|---------------|
| CLK           | GPIO 18       |
| CMD           | GPIO 19       |
| D0            | GPIO 14       |
| D1            | GPIO 15       |
| D2            | GPIO 16       |
| D3            | GPIO 17       |
| RESET slave   | GPIO 54       |

### Periferiche (valori default, configurabili via Kconfig)

| Periferica | GPIO | Note |
|-----------|------|------|
| WS2812B LED strip | GPIO 8 | 105 LED, RMT |
| DS18B20 | GPIO 21 | 1-Wire via RMT, pull-up 4.7kΩ |
| Relay 1 | GPIO 22 | Active-low configurabile |
| Relay 2 | GPIO 23 | Active-low configurabile |
| Relay 3 | GPIO 24 | Active-low configurabile |
| Relay 4 | GPIO 25 | Active-low configurabile |

### Display MIPI DSI (valori default, configurabili via Kconfig)

| Funzione | GPIO | Note |
|----------|------|------|
| LCD Backlight | GPIO 26 | Enable/disable retroilluminazione |
| LCD Reset | GPIO 27 | Reset pannello ILI9881C |
| Touch I2C SDA | GPIO 7 | GT911 data line |
| Touch I2C SCL | GPIO 9 | GT911 clock line |
| Touch INT | -1 | Polling mode (default) |
| Touch RST | -1 | Non collegato (default) |

---

## ⚡ Prerequisiti

1. **ESP-IDF v6.0.0** installato e configurato
   ([guida ufficiale](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/get-started/index.html))
2. **VS Code** con l'estensione [Espressif IDF](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension)
3. La scheda Waveshare ESP32-P4-WiFi6 con il firmware slave **esp_hosted** già
   presente sul coprocessore C6 (è pre-flashato di fabbrica; se necessario,
   aggiornarlo via OTA o UART – vedi sezione sotto)

## 🔨 Build & Flash

```bash
# 1. Impostare il target su ESP32-P4
idf.py set-target esp32p4

# 2. Configurare SSID e password WiFi (facoltativo – si possono anche
#    modificare direttamente in sdkconfig.defaults)
idf.py menuconfig
#    → "Aquarium Controller Settings" → impostare SSID e Password

# 3. Compilare
idf.py build

# 4. Flashare (adattare la porta seriale alla propria macchina)
idf.py -p /dev/ttyACM0 flash

# 5. Monitorare l'output seriale
idf.py -p /dev/ttyACM0 monitor
```

### Da VS Code

1. Aprire il progetto con VS Code
2. Premere **F1** → "ESP-IDF: Set Espressif Device Target" → selezionare `esp32p4`
3. **F1** → "ESP-IDF: SDK Configuration Editor" → configurare WiFi, periferiche, display
4. **F1** → "ESP-IDF: Build your Project"
5. **F1** → "ESP-IDF: Flash your Project"
6. **F1** → "ESP-IDF: Monitor Device"

---

## 📁 Struttura del progetto

```
├── CMakeLists.txt              # Root CMake – progetto ESP-IDF
├── sdkconfig.defaults          # Defaults per la scheda Waveshare
├── partitions.csv              # Tabella partizioni (NVS + PHY + factory 3MB)
├── docs/
│   └── screenshots/            # Screenshot Web UI
├── main/
│   ├── CMakeLists.txt          # Componente main + dipendenze IDF
│   ├── idf_component.yml       # Dipendenze ESP Component Registry
│   ├── Kconfig.projbuild       # Menu configurazione (WiFi, LED, sensori, relè, display)
│   ├── main.c                  # Entry point: init sequenza + main loop 10s
│   ├── wifi_manager.h/.c       # WiFi STA con backoff esponenziale
│   ├── web_server.h/.c         # HTTP server + dashboard HTML 4-tab + 26 REST API
│   ├── led_controller.h/.c     # Driver WS2812B thread-safe: mutex, gamma 2.2, fade ramp
│   ├── led_scenes.h/.c         # Engine 8 scene LED con siesta e configurazione avanzata
│   ├── temperature_sensor.h/.c # DS18B20 con media mobile 3 campioni e calibrazione
│   ├── temperature_history.h/.c# Ring buffer storico 24h (288 campioni)
│   ├── telegram_notify.h/.c    # Notifiche Telegram Bot API con TLS
│   ├── telegram_root_cert.pem  # Certificato Go Daddy Root CA G2 per Telegram
│   ├── relay_controller.h/.c   # Controller 4 relè: GPIO, NVS, nomi, scheduling
│   ├── auto_heater.h/.c        # Termostato automatico con isteresi
│   ├── ota_update.h/.c         # Aggiornamento firmware OTA via HTTP/HTTPS
│   ├── display_driver.h/.c     # Driver MIPI DSI + ILI9881C + GT911 + LVGL init
│   ├── display_ui.h/.c         # Dashboard LVGL on-screen
│   ├── duckdns.h/.c            # Client DuckDNS DDNS (aggiornamento ogni 5 min)
│   ├── geolocation.h/.c        # Configurazione lat/lng/UTC (default: Roma)
│   └── sun_position.h/.c       # Calcolo astronomico alba/tramonto (NOAA)
├── .vscode/
│   └── settings.json           # Configurazione VS Code + ESP-IDF
└── README.md
```

---

## 🌐 REST API

Tutti gli endpoint restituiscono `application/json`. I POST accettano `Content-Type: application/json`.

**26 endpoint** totali su **HTTP porta 80**.

### Sistema

| Metodo | Endpoint | Descrizione |
|--------|----------|-------------|
| `GET` | `/` | Dashboard HTML completa (4 tab, tema scuro, responsive) |
| `GET` | `/api/status` | Stato sistema: IP WiFi, heap libero, uptime, partizione |
| `GET` | `/api/health` | Health check rapido |

### LED

| Metodo | Endpoint | Descrizione |
|--------|----------|-------------|
| `GET` | `/api/leds` | Stato corrente: `{"on":true,"brightness":128,"r":255,"g":128,"b":0}` |
| `POST` | `/api/leds` | Imposta LED: `{"on":true,"brightness":128,"r":255,"g":0,"b":0}` |
| `GET` | `/api/scenes` | Scena attiva + config completa + lista 8 scene |
| `POST` | `/api/scenes` | Cambia scena e/o config: `{"scene":"daylight","color_temp_kelvin":10000,...}` |

**Configurazione scene (POST /api/scenes):**

| Campo | Tipo | Range | Descrizione |
|-------|------|-------|-------------|
| `scene` | string | – | Nome scena da attivare |
| `sunrise_duration_min` | int | 1–120 | Durata alba standalone (minuti) |
| `sunset_duration_min` | int | 1–120 | Durata tramonto standalone (minuti) |
| `transition_duration_min` | int | 1–120 | Transizione Full Day Cycle (minuti) |
| `color_temp_kelvin` | int | 6500–20000 | Temperatura colore daylight (K) |
| `lunar_moonlight` | bool | – | Modulazione fase lunare per moonlight |
| `siesta_enabled` | bool | – | Abilita pausa anti-alghe nel Full Day |
| `siesta_start_min` | int | 0–1439 | Inizio siesta (minuti da mezzanotte) |
| `siesta_end_min` | int | 0–1439 | Fine siesta (minuti da mezzanotte) |
| `siesta_intensity_pct` | int | 0–100 | Intensità durante siesta (%) |

### Temperatura

| Metodo | Endpoint | Descrizione |
|--------|----------|-------------|
| `GET` | `/api/temperature` | Lettura corrente: `{"valid":true,"temperature_c":25.50}` |
| `GET` | `/api/temperature_history` | Storico 24h: array di `{"timestamp":...,"temp_c":...}` |
| `GET` | `/api/temperature/export.csv` | Esportazione CSV con headers `timestamp,temperature_c` |

### Geolocalizzazione

| Metodo | Endpoint | Descrizione |
|--------|----------|-------------|
| `GET` | `/api/geolocation` | Lat, lng, UTC offset, orari alba/tramonto calcolati |
| `POST` | `/api/geolocation` | Imposta: `{"latitude":41.9,"longitude":12.5,"utc_offset_min":60}` |

### Telegram

| Metodo | Endpoint | Descrizione |
|--------|----------|-------------|
| `GET` | `/api/telegram` | Configurazione corrente completa |
| `POST` | `/api/telegram` | Aggiorna: token, chat_id, allarmi, promemoria, riepilogo |
| `POST` | `/api/telegram_test` | Invia messaggio di test |
| `POST` | `/api/telegram_wc` | Registra cambio acqua + notifica |
| `POST` | `/api/telegram_fert` | Registra dose fertilizzante + notifica |

### Relè

| Metodo | Endpoint | Descrizione |
|--------|----------|-------------|
| `GET` | `/api/relays` | Stato tutti i 4 relè (on/off, nome, schedule) |
| `POST` | `/api/relays` | Controlla: `{"index":0,"on":true}` · Rinomina: `{"index":0,"name":"Filtro"}` · Schedule: `{"index":0,"schedule":{"enabled":true,"on_min":480,"off_min":1200}}` |

### Auto-Heater (Termostato)

| Metodo | Endpoint | Descrizione |
|--------|----------|-------------|
| `GET` | `/api/heater` | Config: `{"enabled":true,"relay_index":1,"target_temp_c":25.0,"hysteresis_c":0.5}` |
| `POST` | `/api/heater` | Aggiorna config termostato |

### DuckDNS

| Metodo | Endpoint | Descrizione |
|--------|----------|-------------|
| `GET` | `/api/duckdns` | Configurazione DDNS + ultimo stato |
| `POST` | `/api/duckdns` | Aggiorna: `{"domain":"myaquarium","token":"xxx","enabled":true}` |
| `POST` | `/api/duckdns_update` | Forza aggiornamento DNS immediato |

### OTA Firmware Update

| Metodo | Endpoint | Descrizione |
|--------|----------|-------------|
| `POST` | `/api/ota` | Avvia aggiornamento: `{"url":"https://server/firmware.bin"}` |
| `GET` | `/api/ota_status` | Stato: `{"status":"idle|downloading|flashing|done|error","progress_pct":0}` |

---

## ⚙️ Configurazione Kconfig

Il menu **"Aquarium Controller Settings"** in `idf.py menuconfig` permette di configurare:

### WiFi

| Parametro | Default | Descrizione |
|-----------|---------|-------------|
| `WIFI_SSID` | `your_ssid` | SSID della rete WiFi |
| `WIFI_PASSWORD` | `your_password` | Password WiFi |

### LED Strip

| Parametro | Default | Range | Descrizione |
|-----------|---------|-------|-------------|
| `LED_STRIP_GPIO` | 8 | – | GPIO data WS2812B |
| `LED_STRIP_NUM_LEDS` | 105 | 1–1024 | Numero LED sulla strip |
| `LED_STRIP_DEFAULT_BRIGHTNESS` | 128 | 0–255 | Luminosità all'avvio |
| `LED_SUNRISE_DURATION_MIN` | 30 | 1–120 | Durata alba standalone (min) |
| `LED_SUNSET_DURATION_MIN` | 30 | 1–120 | Durata tramonto standalone (min) |
| `LED_RAMP_DURATION_SEC` | 30 | 0–120 | Durata fade-in/fade-out manuale (s) |
| `LED_DEFAULT_COLOR_TEMP_K` | 10000 | 6500–20000 | Temperatura colore daylight (K) |

### DS18B20 Temperatura

| Parametro | Default | Range | Descrizione |
|-----------|---------|-------|-------------|
| `DS18B20_GPIO` | 21 | – | GPIO 1-Wire (richiede pull-up 4.7kΩ) |
| `DS18B20_READ_INTERVAL_MS` | 5000 | 1000–60000 | Intervallo lettura (ms) |
| `TEMP_HISTORY_INTERVAL_SEC` | 300 | 60–3600 | Intervallo campionamento storico (s) |
| `DS18B20_CALIBRATION_OFFSET_CENTI` | 0 | -500 – +500 | Offset calibrazione (centesimi di °C) |

### Relè

| Parametro | Default | Descrizione |
|-----------|---------|-------------|
| `RELAY_1_GPIO` | 22 | GPIO relè 1 |
| `RELAY_2_GPIO` | 23 | GPIO relè 2 |
| `RELAY_3_GPIO` | 24 | GPIO relè 3 |
| `RELAY_4_GPIO` | 25 | GPIO relè 4 |
| `RELAY_ACTIVE_LOW` | false | Inversione logica (true = LOW attiva il relè) |

### Display MIPI DSI

| Parametro | Default | Descrizione |
|-----------|---------|-------------|
| `DISPLAY_BK_LIGHT_GPIO` | 26 | GPIO retroilluminazione (-1 per disabilitare) |
| `DISPLAY_RST_GPIO` | 27 | GPIO reset pannello (-1 se non collegato) |
| `DISPLAY_TOUCH_I2C_SDA_GPIO` | 7 | GPIO I2C SDA per GT911 |
| `DISPLAY_TOUCH_I2C_SCL_GPIO` | 9 | GPIO I2C SCL per GT911 |
| `DISPLAY_TOUCH_INT_GPIO` | -1 | GPIO interrupt touch (-1 = polling) |
| `DISPLAY_TOUCH_RST_GPIO` | -1 | GPIO reset touch (-1 = non collegato) |

---

## 🔒 Note di sicurezza

- Le credenziali WiFi sono in `sdkconfig.defaults` – **non committare** password reali
- Il bot token Telegram e il token DuckDNS sono salvati in NVS (flash interna), non nel codice
- L'SSID WiFi è servito esclusivamente tramite API JSON con `json_escape()` – non è incorporato nel template HTML
- Le risposte JSON utilizzano `json_escape()` per prevenire injection
- Le API non hanno autenticazione – **limitare l'accesso alla rete locale**
- Il certificato TLS per Telegram include Go Daddy Root CA G2 embedded con fallback al bundle ESP-IDF
- OTA update supporta sia HTTP che HTTPS – **si consiglia HTTPS in produzione**
- L'accesso RMT ai LED è serializzato tramite `led_controller_lock()`/`unlock()` per evitare conflitti tra scene e fade ramp

---

## 🔧 Troubleshooting

| Problema | Soluzione |
|----------|-----------|
| WiFi non si connette | Controllare SSID/password in menuconfig. Il dispositivo ritenta con backoff esponenziale fino a 60s |
| Temperatura mostra "No sensor" | Verificare collegamento DS18B20 al GPIO 21 con resistenza pull-up 4.7kΩ |
| Telegram "clock not synced" | Il dispositivo necessita di sincronizzazione SNTP (automatica dopo connessione WiFi, ~15s) |
| LED non si accendono | Verificare GPIO 8 e alimentazione 5V della strip WS2812B |
| Heap basso (< 30KB) | Ridurre `CONFIG_LED_STRIP_NUM_LEDS` o disabilitare moduli non necessari |
| DuckDNS "update failed" | Verificare dominio e token; assicurarsi che il dispositivo abbia accesso a internet |
| Display nero / nessun output | Verificare connessione MIPI DSI e GPIO backlight (26) / reset (27). Il display init è non bloccante: il sistema si avvia anche senza display |
| Touch non risponde | Verificare I2C SDA (GPIO 7) e SCL (GPIO 9); controllare indirizzo GT911 |
| OTA fallisce | Verificare URL firmware e connessione di rete; controllare `GET /api/ota_status` per dettagli errore |
| Auto-heater non funziona | Verificare relay_index nella config (0–3) e che il sensore DS18B20 sia operativo |
| Task watchdog timeout | Il display init blocca CPU 0: è risolto con pinning a CPU 1. Se persiste, verificare loop infiniti in altri task |
| Full Day Cycle non cambia fase | Verificare geolocalizzazione (`GET /api/geolocation`) e sincronizzazione SNTP |

---

## 🔄 Aggiornamento firmware coprocessore C6

Il coprocessore ESP32-C6 sulla scheda Waveshare viene fornito con un firmware
**esp_hosted** slave pre-installato. Se necessario aggiornarlo:

1. **Via UART** (se i pin sono accessibili): utilizzare `esptool.py` per flashare
   il binario slave direttamente sul C6.
2. **Via OTA over SDIO**: utilizzare l'esempio
   [`host_performs_slave_ota`](https://components.espressif.com/components/espressif/esp_hosted/examples/host_performs_slave_ota)
   di Espressif per aggiornare il C6 dall'ESP32-P4.

---

## 📋 Tabella partizioni

| Nome | Tipo | SubType | Offset | Dimensione | Uso |
|------|------|---------|--------|------------|-----|
| `nvs` | data | nvs | 0x9000 | 24 KB | Configurazioni, stati relè, scene, Telegram, DuckDNS |
| `phy_init` | data | phy | 0xF000 | 4 KB | Calibrazione PHY radio |
| `factory` | app | factory | 0x10000 | 3 MB | Firmware applicazione |

---

## 📄 Licenza

MIT