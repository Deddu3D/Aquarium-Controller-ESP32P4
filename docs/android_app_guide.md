# 📱 Guida alla creazione dell'app Android – Aquarium Controller

Questa guida ti accompagna passo dopo passo nell'apertura, configurazione, compilazione e personalizzazione dell'app Android **AquariumController** che si connette all'ESP32-P4.

---

## Indice

1. [Prerequisiti](#1-prerequisiti)
2. [Struttura del progetto](#2-struttura-del-progetto)
3. [Aprire il progetto in Android Studio](#3-aprire-il-progetto-in-android-studio)
4. [Configurare il certificato HTTPS](#4-configurare-il-certificato-https)
5. [Compilare e installare l'app](#5-compilare-e-installare-lapp)
6. [Architettura dell'app](#6-architettura-dellapp)
7. [Schermate e funzionalità](#7-schermate-e-funzionalità)
8. [Personalizzare l'app](#8-personalizzare-lapp)
9. [Generare l'APK firmato per la distribuzione](#9-generare-lapk-firmato-per-la-distribuzione)
10. [Risoluzione problemi comuni](#10-risoluzione-problemi-comuni)

---

## 1. Prerequisiti

### Software richiesto

| Strumento | Versione minima | Note |
|-----------|-----------------|-------|
| **Android Studio** | Ladybug (2024.x) o superiore | [Scarica qui](https://developer.android.com/studio) |
| **JDK** | 17 | Incluso in Android Studio |
| **Android SDK** | API 24 (Android 7.0) minimo, API 35 (Android 15) target | Installato via SDK Manager in Android Studio |
| **Gradle** | 8.9 (gestito dal wrapper) | Non serve installarlo separatamente |

### Dispositivo di test

- Smartphone o tablet Android **API 24+** (Android 7.0+)
- Oppure **emulatore** configurato in Android Studio (API 24+ con Google APIs)

### Requisiti di rete

- Lo smartphone deve essere **sulla stessa rete WiFi** dell'ESP32-P4
- L'ESP32-P4 deve essere raggiungibile tramite indirizzo IP o hostname mDNS (es. `aquarium.local`)

---

## 2. Struttura del progetto

```
android/
├── app/
│   ├── build.gradle.kts          ← dipendenze e configurazione build
│   ├── proguard-rules.pro
│   └── src/main/
│       ├── AndroidManifest.xml   ← permessi e dichiarazione Activity
│       ├── assets/
│       │   └── server.crt        ← certificato TLS self-signed dell'ESP32
│       ├── java/com/aquarium/controller/
│       │   ├── AquariumApp.kt    ← Application (inizializza Hilt + canale notifiche)
│       │   ├── MainActivity.kt   ← entry point UI (Compose)
│       │   ├── data/
│       │   │   ├── api/          ← interfaccia Retrofit (AquariumApi.kt)
│       │   │   ├── model/        ← modelli dati JSON (Models.kt)
│       │   │   ├── network/      ← HTTPS self-signed, WebSocket, cookie jar
│       │   │   └── prefs/        ← DataStore (salva IP/porta/HTTPS)
│       │   ├── di/               ← moduli Hilt (NetworkModule.kt)
│       │   ├── repository/       ← AquariumRepository.kt (logica di accesso dati)
│       │   └── ui/
│       │       ├── nav/          ← Navigation Compose (grafo schermate)
│       │       ├── connect/      ← schermata connessione
│       │       ├── login/        ← schermata login
│       │       ├── home/         ← dashboard principale
│       │       ├── leds/         ← controllo LED WS2812B
│       │       ├── temperature/  ← grafico temperatura
│       │       ├── automations/  ← relè, heater, CO₂, feeding
│       │       ├── settings/     ← impostazioni (Telegram, DuckDNS, OTA…)
│       │       └── theme/        ← colori, tipografia, tema Material 3
│       └── res/
│           ├── mipmap-*/         ← icone launcher
│           └── values/           ← colori, stringhe, temi
├── build.gradle.kts
├── settings.gradle.kts
└── gradle/
    └── libs.versions.toml        ← version catalog (tutte le versioni librerie)
```

---

## 3. Aprire il progetto in Android Studio

### Passo 3.1 – Clona il repository (se non l'hai già fatto)

```bash
git clone https://github.com/Deddu3D/Aquarium-Controller-ESP32P4.git
cd Aquarium-Controller-ESP32P4
```

### Passo 3.2 – Apri la cartella `android` in Android Studio

1. Avvia **Android Studio**
2. Nella schermata di benvenuto clicca **"Open"**
3. Naviga nella cartella del repository e seleziona la sottocartella **`android/`**
4. Clicca **OK** → Android Studio sincronizza il progetto Gradle

> ⚠️ **Importante:** apri la cartella `android/`, non la radice del repository. La radice contiene il progetto ESP-IDF che Android Studio non sa gestire.

### Passo 3.3 – Installare l'SDK Android mancante (se richiesto)

Se Android Studio mostra l'avviso *"SDK platforms missing"*:

1. Vai su **File → Settings → Appearance & Behavior → System Settings → Android SDK**
2. Nella scheda **"SDK Platforms"** installa **Android 15 (API 35)**
3. Nella scheda **"SDK Tools"** verifica che siano presenti:
   - Android SDK Build-Tools 35
   - Android Emulator
   - Android SDK Platform-Tools

### Passo 3.4 – Sincronizzazione Gradle

Dopo l'apertura, Android Studio scarica automaticamente le dipendenze.  
Se la sincronizzazione fallisce con errori di rete, verifica la connessione internet e riprova con **File → Sync Project with Gradle Files**.

---

## 4. Configurare il certificato HTTPS

L'ESP32-P4 espone un server HTTPS con certificato **self-signed**. L'app deve conoscere quel certificato per stabilire la connessione TLS.

### Passo 4.1 – Estrarre il certificato dall'ESP32

Sul firmware dell'ESP32-P4, il certificato è compilato nel binario. Per esportarlo:

**Opzione A – via browser:**
1. Apri Chrome sul PC e vai su `https://<IP_ESP32>/`
2. Clicca sull'icona 🔒 nella barra degli indirizzi → **Connessione non sicura → Certificato**
3. Vai su **Dettagli → Esporta** e salva il file come `server.crt` (formato PEM/Base64)

**Opzione B – via `openssl`:**
```bash
openssl s_client -connect <IP_ESP32>:443 -showcerts </dev/null 2>/dev/null \
  | openssl x509 -outform PEM > server.crt
```

### Passo 4.2 – Copiare il certificato nel progetto

Sostituisci il file esistente con il tuo certificato:

```bash
cp server.crt android/app/src/main/assets/server.crt
```

> Il file `server.crt` è già presente nel repo con un certificato di esempio. Se il tuo ESP32 usa un certificato diverso (perché hai rigenerato le chiavi), devi aggiornarlo.

---

## 5. Compilare e installare l'app

### Passo 5.1 – Connettere un dispositivo Android

**Su dispositivo fisico:**
1. Abilita le **Opzioni sviluppatore** sul telefono (tocca 7 volte il numero build in Impostazioni → Info telefono)
2. Attiva **Debug USB**
3. Collega il telefono via USB → Android Studio dovrebbe rilevarlo nella barra superiore

**Su emulatore:**
1. Vai su **Device Manager** (icona telefono nella barra laterale destra)
2. Clicca **"+"** → **"Create Virtual Device"**
3. Scegli un dispositivo (es. Pixel 6) con API 34 o 35
4. Scarica e avvia l'immagine

### Passo 5.2 – Compilare e installare (debug)

Clicca il pulsante ▶️ **Run** (oppure `Shift+F10`) nella barra superiore.

Android Studio:
1. Compila il progetto
2. Genera il file APK
3. Installa l'APK sul dispositivo/emulatore
4. Avvia l'app

### Passo 5.3 – Primo avvio

Al primo avvio l'app mostra la schermata **Connect**:

1. Inserisci l'indirizzo IP o hostname dell'ESP32 (es. `192.168.1.100` oppure `aquarium.local`)
2. Porta: `443` per HTTPS, `80` per HTTP
3. Attiva lo switch **"Use HTTPS"** se il firmware usa TLS
4. Oppure usa il pulsante **"Scan mDNS"** per trovare automaticamente il controller sulla rete
5. Clicca **Connect** → l'app testa la connessione e naviga alla schermata di login

---

## 6. Architettura dell'app

L'app segue il pattern **MVVM** (Model-View-ViewModel) con:

```
UI (Composable)
    ↕ StateFlow/collectAsState
ViewModel
    ↕ suspend fun
Repository
    ↕ Retrofit / WebSocket
API (ESP32-P4)
```

### Stack tecnologico

| Layer | Libreria | Ruolo |
|-------|----------|-------|
| **UI** | Jetpack Compose + Material 3 | Schermate dichiarative |
| **Navigazione** | Navigation Compose | Gestione back stack |
| **DI** | Hilt 2.52 | Iniezione dipendenze |
| **Rete** | Retrofit 2.11 + OkHttp 4.12 | Chiamate REST API |
| **Serializzazione** | Moshi 1.15 | JSON ↔ Kotlin data class |
| **WebSocket** | OkHttp WebSocket | Aggiornamenti real-time (temperatura, fase) |
| **Persistenza** | DataStore Preferences | Salva host/porta/HTTPS tra i riavvii |
| **Async** | Kotlin Coroutines + Flow | Threading non bloccante |

### Flusso di navigazione

```
ConnectScreen → LoginScreen → HomeScreen
                                ├── LedScreen
                                ├── TempScreen
                                ├── AutomationsScreen
                                └── SettingsScreen
```

---

## 7. Schermate e funzionalità

### ConnectScreen
- Campo hostname/IP e porta
- Switch HTTPS/HTTP
- **Scan mDNS**: cerca dispositivi `_aquarium._tcp` sulla rete locale
- Salva l'ultimo host in DataStore

### LoginScreen
- Inserisce username e password
- POST `/api/login` → cookie di sessione salvato in `SessionCookieJar`

### HomeScreen (Dashboard)
- **Temperatura real-time** via WebSocket (`wss://<host>/ws`)
- **System Status**: WiFi, RSSI, IP, uptime, heap, partizione OTA, NTP
- **Quick Controls**: toggle ON/OFF dei 4 relè
- **Feeding Mode**: avvia/ferma la modalità alimentazione

### LedScreen
- Slider luminosità (0–255)
- Color picker RGB
- Toggle schedule alba/tramonto
- Attivazione scene animate (SUNRISE, SUNSET, MOONLIGHT, STORM, CLOUDS)

### TempScreen
- Grafico storico 24 h con campioni DS18B20
- Valore corrente con indicatore OK/Allarme
- Pulsante esporta CSV

### AutomationsScreen
- Configurazione **Heater** (termostato): target, isteresi, runaway protection
- Configurazione **CO₂**: pre-anticipo e post-ritardo rispetto alle luci
- Schedule orario dei 4 **relè**
- Impostazioni **Feeding**: durata, dimmer LED

### SettingsScreen
- **Telegram**: bot token, chat ID, soglie allarme temperatura
- **DuckDNS**: dominio e token per DNS dinamico
- **Daily Cycle**: ciclo giornaliero con calcolo alba/tramonto NOAA (coordinate GPS)
- **Timezone**: stringa POSIX del fuso orario
- **OTA**: avvia aggiornamento firmware via URL
- **mDNS**: hostname locale
- Logout / Factory Reset

---

## 8. Personalizzare l'app

### 8.1 – Cambiare il tema (colori)

Modifica `ui/theme/Color.kt` e `ui/theme/Theme.kt`. L'app usa **Material 3 Dynamic Color** con fallback su palette custom.

```kotlin
// Color.kt
val Primary = Color(0xFF1FA3FF)   // blu acqua
val Secondary = Color(0xFF2ECC71) // verde OK
val Error = Color(0xFFE74C3C)     // rosso allarme
```

### 8.2 – Aggiungere una nuova schermata

1. Crea `ui/miaFeature/MiaScreen.kt` e `MiaViewModel.kt`
2. Aggiungi la destinazione in `ui/nav/Navigation.kt`:
   ```kotlin
   object MiaFeature : Screen("mia_feature")
   ```
   e nel `NavHost`:
   ```kotlin
   composable(Screen.MiaFeature.route) {
       MiaScreen(navController = navController)
   }
   ```
3. Aggiungi un item nella `NavigationBar` di `HomeScreen.kt`

### 8.3 – Aggiungere un endpoint REST

1. Aggiungi il metodo in `data/api/AquariumApi.kt`:
   ```kotlin
   @GET("api/mio_endpoint")
   suspend fun getMioEndpoint(): Response<MioResponse>
   ```
2. Aggiungi il modello in `data/model/Models.kt`:
   ```kotlin
   @JsonClass(generateAdapter = true)
   data class MioResponse(val valore: Int)
   ```
3. Chiama il metodo nel `AquariumRepository.kt` e poi nel ViewModel

### 8.4 – Modificare le soglie di allarme temperatura

Nel file `data/network/WebSocketManager.kt`:

```kotlin
private const val TEMP_HIGH = 28.0   // °C soglia alta
private const val TEMP_LOW = 22.0    // °C soglia bassa
```

### 8.5 – Localizzare in italiano

Crea `res/values-it/strings.xml` con le traduzioni dei testi.  
In `res/values/strings.xml` trovi tutte le chiavi da tradurre.

---

## 9. Generare l'APK firmato per la distribuzione

### Passo 9.1 – Creare un keystore (solo la prima volta)

Vai su **Build → Generate Signed Bundle/APK → APK → Create new...**

Compila:
- **Key store path**: scegli dove salvare il file `.jks`
- **Password**: password del keystore
- **Alias**: nome della chiave (es. `aquarium_key`)
- **Validity**: 25 anni (default)
- **Name, Organization**: i tuoi dati

> ⚠️ **Conserva il file `.jks` e le password in un posto sicuro.** Senza di essi non puoi aggiornare l'app sugli stessi dispositivi.

### Passo 9.2 – Generare l'APK firmato

1. **Build → Generate Signed Bundle/APK → APK**
2. Seleziona il keystore e inserisci le credenziali
3. Scegli variante **release**
4. Clicca **Finish**

L'APK si trova in: `android/app/release/app-release.apk`

### Passo 9.3 – Installare l'APK sul telefono

```bash
adb install android/app/release/app-release.apk
```

Oppure copia il file sull'Android e installalo con un file manager (richiede "Installa da fonti sconosciute" nelle impostazioni di sicurezza).

---

## 10. Risoluzione problemi comuni

### ❌ "SSL handshake failed" / "CERTIFICATE_VERIFY_FAILED"

**Causa:** Il certificato in `assets/server.crt` non corrisponde a quello dell'ESP32.

**Soluzione:** Riesporta il certificato dall'ESP32 (vedi [Passo 4.1](#passo-41--estrarre-il-certificato-dallESP32)) e sostituisci `assets/server.crt`.

---

### ❌ "Unable to resolve host aquarium.local"

**Causa:** La risoluzione mDNS non funziona (problema comune su Android 12+).

**Soluzione:** Usa l'indirizzo IP diretto oppure:
- Usa il pulsante **"Scan mDNS"** nell'app che interroga `/api/mdns`
- Aggiungi una voce statica nel router (DHCP reservation + hostname)

---

### ❌ L'app si connette ma le API restituiscono 401

**Causa:** La sessione è scaduta o il cookie non viene inviato.

**Soluzione:** Torna alla schermata login (Settings → Logout) e ri-autenticati.

---

### ❌ Gradle sync fallisce con "Could not resolve..."

**Causa:** Problema di connessione a Maven Central.

**Soluzione:**
1. Verifica la connessione internet
2. Rimuovi la cache Gradle: `rm -rf ~/.gradle/caches`
3. Riprova con **File → Sync Project with Gradle Files**

> **Nota:** `settings.gradle.kts` contiene un mirror locale `http://127.0.0.1:18080/` usato nell'ambiente di sviluppo CI. In locale questa URL fallirà silenziosamente e Gradle userà automaticamente Maven Central come fallback.

---

### ❌ Il WebSocket non si connette

**Causa:** Il firmware non risponde su `/ws` oppure il timeout di OkHttp è troppo basso.

**Soluzione:** Verifica che:
1. L'ESP32 sia raggiungibile tramite ping
2. Il firmware stia girando (LED status bar sul display)
3. La porta HTTPS sia aperta (prova `curl -k https://<IP>/api/health`)

---

### ❌ Build error: "Execution failed for task ':app:kspDebugKotlin'"

**Causa:** Problema con Moshi KSP codegen.

**Soluzione:**
```bash
cd android
./gradlew clean
./gradlew assembleDebug
```

---

## Appendice: API REST principali

| Endpoint | Metodo | Descrizione |
|----------|--------|-------------|
| `/api/login` | POST | Autenticazione (username/password) |
| `/api/status` | GET | Stato sistema (WiFi, heap, uptime…) |
| `/api/health` | GET | Diagnostica rapida |
| `/api/leds` | GET/POST | Stato e controllo LED |
| `/api/temperature` | GET | Temperatura corrente DS18B20 |
| `/api/temperature_history` | GET | Storico 24h |
| `/api/relays` | GET/POST | Stato e toggle relè |
| `/api/heater` | GET/POST | Configurazione termostato |
| `/api/co2` | GET/POST | Configurazione CO₂ |
| `/api/feeding` | GET/POST | Modalità alimentazione |
| `/api/telegram` | GET/POST | Configurazione bot Telegram |
| `/api/ota` | POST | Avvia aggiornamento firmware OTA |
| `/ws` | WebSocket | Push real-time (temp, fase, heap) |

Per la documentazione completa di tutti gli endpoint vedi il codice sorgente in `main/web_server.c`.
