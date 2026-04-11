# Aquarium Controller – ESP32-P4

Firmware per **ESP32-P4** con coprocessore **ESP32-C6** per la gestione WiFi.

| | |
|---|---|
| **Scheda** | [Waveshare ESP32-P4-WiFi6](https://www.waveshare.com/esp32-p4-wifi6.htm?sku=32020) rev 1.3 |
| **MCU principale** | ESP32-P4 (application processor, nessun WiFi nativo) |
| **Coprocessore WiFi** | ESP32-C6 (WiFi 6 / BLE 5) collegato via **SDIO** |
| **Framework** | ESP-IDF v6.0.0 |
| **IDE** | VS Code + estensione ESP-IDF |

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
│   ├── idf_component.yml       # Dipendenze (esp_wifi_remote, esp_hosted)
│   ├── Kconfig.projbuild       # Menu WiFi per menuconfig
│   ├── main.c                  # Entry point applicazione
│   ├── wifi_manager.h          # API gestione WiFi
│   └── wifi_manager.c          # Implementazione WiFi STA
├── .vscode/
│   └── settings.json           # Configurazione VS Code + ESP-IDF
└── README.md
```

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