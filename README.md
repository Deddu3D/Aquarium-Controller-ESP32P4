# Aquarium Controller – ESP32-P4

Firmware per **ESP32-P4** con coprocessore **ESP32-C6** per la gestione WiFi.
Un controller completo per acquario con LED WS2812B, sensore di temperatura DS18B20,
notifiche Telegram, DDNS DuckDNS, e 4 relè configurabili.

| | |
|---|---|
| **Scheda** | [Waveshare ESP32-P4-WiFi6](https://www.waveshare.com/esp32-p4-wifi6.htm?sku=32020) rev 1.3 |
| **MCU principale** | ESP32-P4 (application processor, nessun WiFi nativo) |
| **Coprocessore WiFi** | ESP32-C6 (WiFi 6 / BLE 5) collegato via **SDIO** |
| **Framework** | ESP-IDF v6.0.0 |
| **IDE** | VS Code + estensione ESP-IDF |

---

## Funzionalità

- 🐟 **Illuminazione LED WS2812B** – Controllo colore RGB, luminosità con correzione gamma 2.2, 7 scene predefinite + ciclo giornaliero automatico basato su alba/tramonto
- 🌡️ **Sensore temperatura DS18B20** – Lettura ogni 5s con media mobile a 3 campioni, storico 24h con grafico, offset di calibrazione configurabile
- 📱 **Notifiche Telegram** – Allarmi temperatura (alta/bassa), promemoria cambio acqua e fertilizzante, riepilogo giornaliero
- 🔌 **4 Relè configurabili** – Nomi personalizzabili, stato persistente in NVS
- 🌐 **DuckDNS DDNS** – Aggiornamento automatico ogni 5 minuti
- 📡 **WiFi con riconnessione automatica** – Backoff esponenziale (1s → 60s), nessun limite tentativi
- 📊 **Dashboard web mobile-first** – UI responsive con 5 tab: Dashboard, Luci, Relè, Telegram, Network

---

## Architettura

```
┌────────────┐   SDIO    ┌────────────┐
│  ESP32-P4  │◄─────────►│  ESP32-C6  │──── WiFi 6 antenna
│  (host)    │           │  (slave)   │
└────────────┘           └────────────┘
```

L'ESP32-P4 utilizza le API standard `esp_wifi_*` di ESP-IDF.
Il componente **esp_wifi_remote** (+ **esp_hosted**) intercetta le chiamate e le
delega al coprocessore C6 attraverso il bus SDIO, in modo completamente
trasparente per l'applicazione.

### Moduli software

| Modulo | File | Descrizione |
|--------|------|-------------|
| WiFi Manager | `wifi_manager.h/.c` | Connessione STA con backoff esponenziale |
| LED Controller | `led_controller.h/.c` | Driver WS2812B con mutex thread-safe e gamma LUT |
| LED Scenes | `led_scenes.h/.c` | Engine 7 scene + ciclo giornaliero con alba/tramonto |
| Temperature | `temperature_sensor.h/.c` | DS18B20 con media mobile e calibrazione |
| Temp History | `temperature_history.h/.c` | Ring buffer 24h (288 campioni) |
| Telegram | `telegram_notify.h/.c` | Bot API con TLS, allarmi, promemoria, riepilogo |
| Relay Controller | `relay_controller.h/.c` | 4 relè GPIO con stato NVS |
| DuckDNS | `duckdns.h/.c` | Client DDNS con aggiornamento periodico |
| Geolocation | `geolocation.h/.c` | Lat/lng/UTC per calcolo alba/tramonto |
| Sun Position | `sun_position.h/.c` | Algoritmo astronomico alba/tramonto |
| Web Server | `web_server.h/.c` | HTTP server con dashboard e REST API |

## Pin mapping SDIO (Waveshare ESP32-P4-WiFi6)

| Funzione SDIO | GPIO ESP32-P4 |
|---------------|---------------|
| CLK           | GPIO 18       |
| CMD           | GPIO 19       |
| D0            | GPIO 14       |
| D1            | GPIO 15       |
| D2            | GPIO 16       |
| D3            | GPIO 17       |
| RESET slave   | GPIO 54       |

### Pin periferiche (valori default, configurabili via Kconfig)

| Periferica | GPIO | Note |
|-----------|------|------|
| WS2812B LED strip | GPIO 8 | 105 LED, RMT |
| DS18B20 | GPIO 21 | 1-Wire via RMT |
| Relay 1 | GPIO 22 | Active-low |
| Relay 2 | GPIO 23 | Active-low |
| Relay 3 | GPIO 24 | Active-low |
| Relay 4 | GPIO 25 | Active-low |

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
│   ├── idf_component.yml       # Dipendenze (esp_wifi_remote, esp_hosted, ...)
│   ├── Kconfig.projbuild       # Menu WiFi + periferiche per menuconfig
│   ├── main.c                  # Entry point applicazione
│   ├── wifi_manager.h/.c       # WiFi STA con backoff esponenziale
│   ├── web_server.h/.c         # HTTP server + dashboard + REST API
│   ├── led_controller.h/.c     # Driver WS2812B thread-safe
│   ├── led_scenes.h/.c         # Engine scene LED
│   ├── temperature_sensor.h/.c # DS18B20 con media mobile
│   ├── temperature_history.h/.c# Ring buffer storico 24h
│   ├── telegram_notify.h/.c    # Notifiche Telegram Bot API
│   ├── telegram_root_cert.pem  # Certificato CA root per api.telegram.org
│   ├── relay_controller.h/.c   # Controller 4 relè
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
| `GET` | `/` | Dashboard HTML completa |
| `GET` | `/api/status` | Stato WiFi, heap, uptime |

### LED

| Metodo | Endpoint | Descrizione |
|--------|----------|-------------|
| `GET` | `/api/leds` | Stato corrente (on, brightness, r, g, b) |
| `POST` | `/api/leds` | Imposta LED: `{"on":true,"brightness":128,"r":255,"g":0,"b":0}` |
| `GET` | `/api/scenes` | Scena attiva e lista scene disponibili |
| `POST` | `/api/scenes` | Cambia scena: `{"scene":"daylight"}` |

**Scene disponibili:** `off`, `daylight`, `sunrise`, `sunset`, `moonlight`, `cloudy`, `storm`, `full_day_cycle`

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
| `GET` | `/api/relays` | Stato di tutti i 4 relè |
| `POST` | `/api/relays` | Controlla: `{"index":0,"on":true}` o rinomina: `{"index":0,"name":"Filtro"}` |

### DuckDNS

| Metodo | Endpoint | Descrizione |
|--------|----------|-------------|
| `GET` | `/api/duckdns` | Configurazione DDNS |
| `POST` | `/api/duckdns` | Aggiorna config: `{"domain":"myaquarium","token":"xxx","enabled":true}` |
| `POST` | `/api/duckdns_update` | Forza aggiornamento DNS immediato |

---

## Configurazione Kconfig

Il menu **"Aquarium Controller Settings"** in `idf.py menuconfig` permette di configurare:

- **WiFi**: SSID e password
- **LED Strip**: GPIO, numero LED, luminosità default
- **DS18B20**: GPIO, intervallo lettura, intervallo storico, offset calibrazione
- **Relay**: GPIO per ciascuno dei 4 relè, inversione logica (active-low)

---

## Note di sicurezza

- Le credenziali WiFi sono in `sdkconfig.defaults` – **non committare** password reali
- Il bot token Telegram e il token DuckDNS sono salvati in NVS (flash interna)
- L'SSID WiFi è HTML-escaped nella dashboard per prevenire XSS
- Le API non hanno autenticazione – limitare l'accesso alla rete locale
- Il certificato TLS per Telegram include Go Daddy Root CA G2; c'è un fallback al bundle ESP-IDF

---

## Troubleshooting

| Problema | Soluzione |
|----------|-----------|
| WiFi non si connette | Controllare SSID/password in menuconfig. Il dispositivo ritenta con backoff esponenziale fino a 60s |
| Temperatura mostra "No sensor" | Verificare collegamento DS18B20 al GPIO 21 con resistenza pull-up 4.7kΩ |
| Telegram "clock not synced" | Il dispositivo necessita di sincronizzazione SNTP (automatica dopo connessione WiFi, ~30s) |
| LED non si accendono | Verificare GPIO 8 e alimentazione 5V della strip WS2812B |
| Heap basso (< 30KB) | Ridurre `CONFIG_LED_STRIP_NUM_LEDS` o disabilitare moduli non necessari |
| DuckDNS "update failed" | Verificare dominio e token; assicurarsi che il dispositivo abbia accesso a internet |

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