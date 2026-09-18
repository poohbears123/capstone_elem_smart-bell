# capstone_elem_smart-bell

## 📌 Overview

**capstone_elem_smart-bell** is an ESP32-based smart bell system featuring:

- 📶 WiFi connectivity with async handling (AP + captive portal, WPA2 secured)
- 🔊 Digital audio output (I2S amplifier, MAX98357A)
- 💾 SD card audio playback with thread-safe access
- 🖥️ Web-based dashboard and login system
- 🕒 RTC-based scheduling (DS3231)
- 🔐 Authentication and event logging
- 🎓 Autonomous attendance logging (WiFi probe sniffing)
- ⚙️ FreeRTOS dual-core task architecture

The system evolves from a **standalone embedded device (v1.3)** into a **fully interactive IoT system (v1.7)**, and now runs on a **FreeRTOS dual-core architecture (v2.1.0)**.

---

## 🧠 Architecture (v2.1.0 — FreeRTOS Edition)

### Core 0 (Protocol)
- `vSnifferTask` (Priority 2): attendance dedup + CSV logging. Promiscuous mode auto-toggles around attendance windows so the AP stays functional outside class times.

### Core 1 (Application + Real-time)
- `vWebTask` (Priority 1): DNSServer captive-portal processing
- `vAudioTask` (Priority 5): I2S audio streaming via `Audio.h`
- `vSystemEngineTask` (Priority 3): RTC schedule matching, edge-detected button inputs, sniffer enable/disable control

### Shared resources
- `xLogQueue` / `xAudioQueue`: ISR-safe inter-task messaging
- `xI2CMutex` / `xScheduleMutex` / `xSdMutex`: thread-safe access to the RTC, schedule data, and SD card

---

## ✨ Features

### 🔐 Authentication System
- Login with session handling
- Modal-based error feedback
- Fetch-based authentication (`401`, `423`, `303`)

### 📊 Dashboard
- Sidebar navigation (6 views)
- Telemetry cards (`/api/telemetry`, incl. time, connection status, attendance window, sniffer state)
- Interactive scheduling interface
- Time-dial input controls

### 🔊 Audio System
- I2S audio via **MAX98357A** using the `Audio.h` library
- SD-card playback with queue-based task handling:
  - Scheduled chimes (`/audio/chime.mp3`)
  - Emergency alert (`/audio/alert.mp3`)
  - Per-entry start/end chime paths

### 💾 Storage
- SD card support for audio playback and logs
- SPIFFS for the web UI (`/ui/`)
- `/data/schedule.json` for configuration and schedules
- `/data/logs.csv` for attendance records (real RTC timestamps)
- All SD access serialized through `xSdMutex` for thread safety

---

### 📡 Networking
- WPA2-secured AP (`FCU_Secure_Bell`) with captive portal (DNSServer redirects all DNS to the ESP32)
- Async WiFi connection handling (optional STA uplink)
- `/api/schedule` (GET/POST, chunk-safe JSON parsing), `/api/attendance`, `/api/attendance/status`, `/api/config`, `/api/telemetry` endpoints
- Captive-portal detection for iOS, Android, and Windows clients
- Real-time status feedback

### 🕒 Scheduling
- RTC (DS3231) integration, initialized once at boot
- Day-of-week bitmask matching (Sun–Sat) per schedule entry (up to 20 entries)
- Scheduled bell triggering with exactly-once-per-day firing
- Manual trigger support

### 📜 Logging System
Tracks:
- Attendance records (MAC address, RSSI, real RTC timestamp, status)
- Login attempts and lockouts
- System events
- Schedule actions
- Network updates

---

## 🔧 Libraries & Dependencies

| Library | Version | Purpose |
|---|---|---|
| ESP32 Arduino Core | 3.3.x | Platform |
| ESPAsyncWebServer | 3.5.0 | Web server + captive portal |
| AsyncTCP | 3.x (must match web server) | Async TCP backend |
| Audio.h (schreibfaul1) | latest | I2S MP3 playback from SD |
| RTClib | latest | DS3231 RTC |
| ArduinoJson | 7.x | Schedule/config parsing |
| DNSServer (core) | — | Captive portal |
| SD / SPIFFS (core) | — | Storage |

> ⚠️ **Note:** ESPAsyncWebServer and AsyncTCP must come from the same source and matching versions — a mismatch causes a `const`-qualifier compilation error in `AsyncServer::status()`.

---

## 🚀 Getting Started

1. Flash `capstone2.1.ino` (firmware v2.1.0) to the ESP32-WROOM-32
2. **Set `AP_PASSWORD`** in the sketch (placeholder must be changed before deploying)
3. Prepare the SD card:
   - `/audio/chime.mp3` and `/audio/alert.mp3`
   - `/data/` (schedule + logs)
   - `/ui/` (optional web UI)
4. Power on — the ESP32 broadcasts AP `FCU_Secure_Bell`
5. Connect and the captive portal opens the dashboard automatically

---

## 📝 Changelog

- **v2.1.0 (working prototype)** — FreeRTOS edition: dual-core task architecture, day-of-week fix, exactly-once chime firing, thread-safe SD access, WPA2-secured AP, real RTC timestamps in attendance CSV, edge-detected buttons, chunk-safe JSON parsing, auto-toggling attendance sniffer
- **v1.7** — Fully interactive IoT system: web dashboard, authentication, scheduling
- **v1.3** — Standalone embedded device: bell triggering, RTC scheduling
