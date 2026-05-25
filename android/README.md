# Aquarium Controller – Android App

App Android companion per il controller acquario ESP32-P4.

## Funzionamento

L'app è un **WebView wrapper** che carica esattamente la stessa Web UI servita dall'ESP32 tramite DuckDNS.  
Non esiste duplicazione della logica: tutto il controllo avviene nella Web UI già integrata nel firmware.

### Primo avvio

Al primo avvio viene mostrata una schermata di configurazione dove inserire il link DuckDNS:

```
https://mioaquario.duckdns.org
```

L'URL viene salvato in locale e usato ad ogni avvio successivo.

### Cambio URL

Dal menu (⋮) → **Cambia URL** è possibile aggiornare l'indirizzo DuckDNS in qualsiasi momento.

### Funzionalità WebView

| Feature | Stato |
|---------|-------|
| JavaScript | ✅ abilitato |
| DOM Storage / localStorage | ✅ abilitato |
| Certificato SSL self-signed ESP32 | ✅ accettato |
| Swipe-to-refresh | ✅ |
| Navigazione back nel WebView | ✅ |
| Zoom | ✅ |

## Build

### Prerequisiti

- Android Studio Hedgehog (2023.1.1) o superiore
- Android SDK 34
- JDK 17

### Compilazione

```bash
cd android
./gradlew assembleDebug
```

L'APK viene generato in `app/build/outputs/apk/debug/app-debug.apk`.

### Release

```bash
cd android
./gradlew assembleRelease
```

Firmare l'APK con il proprio keystore prima della distribuzione.

## Requisiti dispositivo

- Android 7.0 (API 24) o superiore
- Connessione Internet (per raggiungere DuckDNS)

## Note di sicurezza

L'app accetta il certificato SSL self-signed dell'ESP32 (equivalente al "procedi comunque" del browser).  
Questo è previsto per design: il certificato è generato localmente e l'URL è inserito manualmente dall'utente.
