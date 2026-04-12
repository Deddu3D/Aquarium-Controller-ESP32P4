# Aquarium Controller – ESP32-P4

Firmware per **ESP32-P4** con coprocessore **ESP32-C6** per la gestione WiFi.
Un controller completo per acquario con display touch MIPI DSI, LED WS2812B,
sensore di temperatura DS18B20, termostato automatico, notifiche Telegram,
DDNS DuckDNS, OTA update, e 4 relè configurabili con scheduling.

| | |
|---|---|
| **Scheda** | [Waveshare ESP32-P4-WiFi6](https://www.waveshare.com/esp32-p4-wifi6.htm?sku=32020) rev 1.3 |
| **MCU principale** | ESP32-P4 (application processor, nessun WiFi nativo) |
| **Coprocessore WiFi** | ESP32-C6 (WiFi 6 / BLE 5) collegato via **SDIO** |
| **Display** | 5″ MIPI DSI 800×480 ILI9881C + touch GT911 |
| **Framework** | ESP-IDF v6.0.0 |
| **Grafica** | LVGL v9.x (double-buffered PSRAM) |
| **IDE** | VS Code + estensione ESP-IDF |

---

## Screenshots

### Display LVGL (5″ 800×480)

Dashboard on-screen con temperatura, scena LED attiva, stato WiFi, stato 4 relè e orologio real-time.

![Display LVGL Dashboard](https://github.com/user-attachments/assets/97c94714-cc51-4dab-95f7-3573bac166da)

### Web UI (mobile-first)

Dashboard web responsive con 5 tab: Dashboard, Luci, Relè, Telegram, Network. Accessibile da qualsiasi browser sulla rete locale.

![Web UI Dashboard](https://github.com/user-attachments/assets/7790f6e3-d14e-40e2-96a4-b2f19f0e0772)

---

## Funzionalità

- 🖥️ **Display touch MIPI DSI** – Schermo 5″ 800×480 con dashboard LVGL: temperatura, scena LED, stato WiFi, 4 relè, orologio. Refresh ogni 10s, touch capacitivo GT911
- 🐟 **Illuminazione LED WS2812B** – Controllo colore RGB, luminosità con correzione gamma 2.2, 7 scene predefinite + ciclo giornaliero automatico basato su alba/tramonto. Fade-in/fade-out graduali (default 30s) per il benessere dei pesci
- 🌡️ **Sensore temperatura DS18B20** – Lettura ogni 5s con media mobile a 3 campioni, storico 24h con grafico, offset di calibrazione configurabile
- 🔥 **Termostato automatico (Auto-Heater)** – Controllo termostatico tramite relè + DS18B20 con isteresi configurabile (0.1–3.0 °C), target temperatura 15–35 °C
- 📱 **Notifiche Telegram** – Allarmi temperatura (alta/bassa), promemoria cambio acqua e fertilizzante, riepilogo giornaliero. TLS con certificato Go Daddy Root CA G2 embedded
- 🔌 **4 Relè configurabili** – Nomi personalizzabili, stato persistente in NVS, **scheduling time-of-day** con supporto finestre overnight (on > off)
- 🌐 **DuckDNS DDNS** – Aggiornamento automatico ogni 5 minuti
- 📡 **WiFi con riconnessione automatica** – Backoff esponenziale (1s → 60s), nessun limite tentativi
- 📊 **Dashboard web mobile-first** – UI responsive con 5 tab: Dashboard, Luci, Relè, Telegram, Network
- 🔄 **OTA Firmware Update** – Aggiornamento firmware via HTTP/HTTPS in background con progress tracking e reboot automatico
- 🐕 **Task Watchdog Timer** – Watchdog 30s con reboot automatico in caso di blocco

---

## Architettura

```
┌────────────┐   SDIO    ┌────────────┐
│  ESP32-P4  │◄─────────►│  ESP32-C6  │──── WiFi 6 antenna
│  (host)    │           │  (slave)   │
└────────────┘           └────────────┘
       │
       │  MIPI DSI (2 lane, 500 Mbps)
       ▼
┌─────────────────┐
│  5″ LCD 800×480 │
│  ILI9881C +     │
│  GT911 touch    │
└─────────────────┘
```

L'ESP32-P4 utilizza le API standard `esp_wifi_*` di ESP-IDF.
Il componente **esp_wifi_remote** (+ **esp_hosted**) intercetta le chiamate e le
delega al coprocessore C6 attraverso il bus SDIO, in modo completamente
trasparente per l'applicazione.

### Moduli software

| Modulo | File | Descrizione |
|--------|------|-------------|
| WiFi Manager | `wifi_manager.h/.c` | Connessione STA con backoff esponenziale |
| LED Controller | `led_controller.h/.c` | Driver WS2812B con mutex thread-safe, gamma LUT 2.2, fade ramp |
| LED Scenes | `led_scenes.h/.c` | Engine 7 scene + ciclo giornaliero con alba/tramonto, siesta anti-alghe |
| Temperature | `temperature_sensor.h/.c` | DS18B20 con media mobile a 3 campioni e calibrazione |
| Temp History | `temperature_history.h/.c` | Ring buffer 24h (288 campioni, 5 min intervallo) |
| Telegram | `telegram_notify.h/.c` | Bot API con TLS, allarmi, promemoria, riepilogo giornaliero |
| Relay Controller | `relay_controller.h/.c` | 4 relè GPIO con stato NVS e scheduling time-of-day |
| Auto Heater | `auto_heater.h/.c` | Termostato automatico con isteresi (relè + DS18B20) |
| DuckDNS | `duckdns.h/.c` | Client DDNS con aggiornamento periodico (5 min) |
| OTA Update | `ota_update.h/.c` | Aggiornamento firmware via HTTP con progress tracking |
| Display Driver | `display_driver.h/.c` | Driver MIPI DSI + ILI9881C + GT911 touch + LVGL init |
| Display UI | `display_ui.h/.c` | Dashboard LVGL on-screen (temperatura, LED, WiFi, relè, orologio) |
| Geolocation | `geolocation.h/.c` | Lat/lng/UTC per calcolo alba/tramonto |
| Sun Position | `sun_position.h/.c` | Algoritmo astronomico alba/tramonto |
| Web Server | `web_server.h/.c` | HTTP server con dashboard mobile-first e REST API |

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

## Pin mapping

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

## Prerequisiti

1. **ESP-IDF v6.0.0** installato e configurato
   ([guida ufficiale](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/get-started/index.html))
2. **VS Code** con l'estensione [Espressif IDF](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension)
3. La scheda Waveshare ESP32-P4-WiFi6 con il firmware slave **esp_hosted** già
   presente sul coprocessore C6 (è pre-flashato di fabbrica; se necessario,
   aggiornarlo via OTA o UART – vedi sezione sotto)

## Build & Flash

```bash
# 1. Impostare il target su ESP32-P4
idf.py set-target esp32p4

# 2. Configurare SSID e password WiFi (facoltativo – si possono anche
#    modificare direttamente in sdkconfig.defaults)
idf.py menuconfig
#    → "Aquarium WiFi Settings" → impostare SSID e Password

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
3. **F1** → "ESP-IDF: SDK Configuration Editor" → impostare SSID/password
4. **F1** → "ESP-IDF: Build your Project"
5. **F1** → "ESP-IDF: Flash your Project"
6. **F1** → "ESP-IDF: Monitor Device"

---

## Struttura del progetto

```
├── CMakeLists.txt              # Root CMake – progetto ESP-IDF
├── sdkconfig.defaults          # Defaults per la scheda Waveshare
├── partitions.csv              # Tabella partizioni personalizzata
├── main/
│   ├── CMakeLists.txt          # Componente main
│   ├── idf_component.yml       # Dipendenze (esp_wifi_remote, esp_hosted, LVGL, ...)
│   ├── Kconfig.projbuild       # Menu WiFi + periferiche + display per menuconfig
│   ├── main.c                  # Entry point applicazione
│   ├── wifi_manager.h/.c       # WiFi STA con backoff esponenziale
│   ├── web_server.h/.c         # HTTP server + dashboard + REST API
│   ├── led_controller.h/.c     # Driver WS2812B thread-safe con fade ramp
│   ├── led_scenes.h/.c         # Engine scene LED con siesta e configurazione avanzata
│   ├── temperature_sensor.h/.c # DS18B20 con media mobile e calibrazione
│   ├── temperature_history.h/.c# Ring buffer storico 24h
│   ├── telegram_notify.h/.c    # Notifiche Telegram Bot API
│   ├── telegram_root_cert.pem  # Certificato CA root per api.telegram.org
│   ├── relay_controller.h/.c   # Controller 4 relè con scheduling time-of-day
│   ├── auto_heater.h/.c        # Termostato automatico (relè + DS18B20)
│   ├── ota_update.h/.c         # Aggiornamento firmware OTA via HTTP
│   ├── display_driver.h/.c     # Driver MIPI DSI + ILI9881C + GT911 + LVGL
│   ├── display_ui.h/.c         # Dashboard LVGL on-screen
│   ├── duckdns.h/.c            # Client DuckDNS DDNS
│   ├── geolocation.h/.c        # Config geolocalizzazione
│   └── sun_position.h/.c       # Calcolo alba/tramonto
├── .vscode/
│   └── settings.json           # Configurazione VS Code + ESP-IDF
└── README.md
```

---

## REST API

Tutti gli endpoint restituiscono `application/json`. I POST accettano `Content-Type: application/json`.

### Dashboard

| Metodo | Endpoint | Descrizione |
|--------|----------|-------------|
| `GET` | `/` | Dashboard HTML completa (mobile-first, 5 tab) |
| `GET` | `/api/status` | Stato WiFi, heap, uptime |
| `GET` | `/api/health` | Health check rapido |

### LED

| Metodo | Endpoint | Descrizione |
|--------|----------|-------------|
| `GET` | `/api/leds` | Stato corrente (on, brightness, r, g, b) |
| `POST` | `/api/leds` | Imposta LED: `{"on":true,"brightness":128,"r":255,"g":0,"b":0}` |
| `GET` | `/api/scenes` | Scena attiva, config completa e lista scene |
| `POST` | `/api/scenes` | Cambia scena e/o config: `{"scene":"daylight","color_temp_kelvin":10000,...}` |

**Scene disponibili:** `off`, `daylight`, `sunrise`, `sunset`, `moonlight`, `cloudy`, `storm`, `full_day_cycle`

**Configurazione scene (POST /api/scenes):**

| Campo | Tipo | Range | Descrizione |
|-------|------|-------|-------------|
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
| `GET` | `/api/temperature_history` | Storico 24h con campioni timestampati |

### Geolocalizzazione

| Metodo | Endpoint | Descrizione |
|--------|----------|-------------|
| `GET` | `/api/geolocation` | Lat, lng, UTC offset, alba/tramonto |
| `POST` | `/api/geolocation` | Imposta: `{"latitude":41.9,"longitude":12.5,"utc_offset_min":60}` |

### Telegram

| Metodo | Endpoint | Descrizione |
|--------|----------|-------------|
| `GET` | `/api/telegram` | Configurazione corrente |
| `POST` | `/api/telegram` | Aggiorna configurazione (token, chat_id, allarmi, ecc.) |
| `POST` | `/api/telegram_test` | Invia messaggio di test |
| `POST` | `/api/telegram_wc` | Registra cambio acqua |
| `POST` | `/api/telegram_fert` | Registra dose fertilizzante |

### Relè

| Metodo | Endpoint | Descrizione |
|--------|----------|-------------|
| `GET` | `/api/relays` | Stato di tutti i 4 relè (incluso schedule) |
| `POST` | `/api/relays` | Controlla: `{"index":0,"on":true}`, rinomina: `{"index":0,"name":"Filtro"}`, o schedule: `{"index":0,"schedule":{"enabled":true,"on_min":480,"off_min":1200}}` |

### Auto-Heater (Termostato)

| Metodo | Endpoint | Descrizione |
|--------|----------|-------------|
| `GET` | `/api/heater` | Configurazione termostato: `{"enabled":true,"relay_index":1,"target_temp_c":25.0,"hysteresis_c":0.5}` |
| `POST` | `/api/heater` | Aggiorna config: `{"enabled":true,"relay_index":1,"target_temp_c":25.0,"hysteresis_c":0.5}` |

### DuckDNS

| Metodo | Endpoint | Descrizione |
|--------|----------|-------------|
| `GET` | `/api/duckdns` | Configurazione DDNS |
| `POST` | `/api/duckdns` | Aggiorna config: `{"domain":"myaquarium","token":"xxx","enabled":true}` |
| `POST` | `/api/duckdns_update` | Forza aggiornamento DNS immediato |

### OTA Firmware Update

| Metodo | Endpoint | Descrizione |
|--------|----------|-------------|
| `POST` | `/api/ota` | Avvia aggiornamento: `{"url":"http://server/firmware.bin"}` |
| `GET` | `/api/ota_status` | Stato aggiornamento: `{"status":"idle\|downloading\|flashing\|done\|error","progress_pct":0}` |

---

## Configurazione Kconfig

Il menu **"Aquarium Controller Settings"** in `idf.py menuconfig` permette di configurare:

- **WiFi**: SSID e password
- **LED Strip**: GPIO, numero LED, luminosità default, durata alba/tramonto standalone, durata transizione Full Day, rampa fade-in/fade-out manuale (default 30s), temperatura colore default (6500–20000 K)
- **DS18B20**: GPIO, intervallo lettura, intervallo storico, offset calibrazione (centesimi di °C)
- **Relay**: GPIO per ciascuno dei 4 relè, inversione logica (active-low)
- **Display MIPI DSI**: GPIO backlight, reset, I2C SDA/SCL per touch GT911, GPIO interrupt e reset touch

---

## Note di sicurezza

- Le credenziali WiFi sono in `sdkconfig.defaults` – **non committare** password reali
- Il bot token Telegram e il token DuckDNS sono salvati in NVS (flash interna)
- L'SSID WiFi è HTML-escaped nella dashboard per prevenire XSS
- Le risposte JSON utilizzano `json_escape()` per prevenire injection
- Le API non hanno autenticazione – limitare l'accesso alla rete locale
- Il certificato TLS per Telegram include Go Daddy Root CA G2 embedded; c'è un fallback al bundle ESP-IDF
- OTA update supporta sia HTTP che HTTPS – si consiglia HTTPS in produzione

---

## Troubleshooting

| Problema | Soluzione |
|----------|-----------|
| WiFi non si connette | Controllare SSID/password in menuconfig. Il dispositivo ritenta con backoff esponenziale fino a 60s |
| Temperatura mostra "No sensor" | Verificare collegamento DS18B20 al GPIO 21 con resistenza pull-up 4.7kΩ |
| Telegram "clock not synced" | Il dispositivo necessita di sincronizzazione SNTP (automatica dopo connessione WiFi, ~15s) |
| LED non si accendono | Verificare GPIO 8 e alimentazione 5V della strip WS2812B |
| Heap basso (< 30KB) | Ridurre `CONFIG_LED_STRIP_NUM_LEDS` o disabilitare moduli non necessari |
| DuckDNS "update failed" | Verificare dominio e token; assicurarsi che il dispositivo abbia accesso a internet |
| Display nero / nessun output | Verificare connessione MIPI DSI e GPIO backlight (26) / reset (27) |
| Touch non risponde | Verificare I2C SDA (GPIO 7) e SCL (GPIO 9); controllare indirizzo GT911 |
| OTA fallisce | Verificare URL firmware e connessione di rete; controllare `/api/ota_status` per dettagli errore |
| Auto-heater non funziona | Verificare relay_index nella config (0–3) e che il sensore DS18B20 sia operativo |

---

## Aggiornamento firmware coprocessore C6

Il coprocessore ESP32-C6 sulla scheda Waveshare viene fornito con un firmware
**esp_hosted** slave pre-installato. Se necessario aggiornarlo:

1. **Via UART** (se i pin sono accessibili): utilizzare `esptool.py` per flashare
   il binario slave direttamente sul C6.
2. **Via OTA over SDIO**: utilizzare l'esempio
   [`host_performs_slave_ota`](https://components.espressif.com/components/espressif/esp_hosted/examples/host_performs_slave_ota)
   di Espressif per aggiornare il C6 dall'ESP32-P4.

## Licenza

MIT