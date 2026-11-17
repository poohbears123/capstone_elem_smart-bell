# capstone_elem_smart-bell (FCU Smart Bell & Alarm Controller)

[![Version](https://img.shields.io/badge/version-3.1.0-blue.svg)](https://github.com/)
[![Platform](https://img.shields.io/badge/platform-ESP32--S3-orange.svg)](https://www.espressif.com/)
[![Framework](https://img.shields.io/badge/framework-Arduino%20%2F%20FreeRTOS-green.svg)](https://www.freertos.org/)

## 📌 Overview

**capstone_elem_smart-bell** is an ESP32-S3-based smart bell and alarm controller featuring:

* 📶 **Async WiFi Networking:** WPA2-secured SoftAP (`FCU_Alarm_Bell`) with wildcard DNS captive portal + non-blocking STA uplink for NTP time synchronization.
* 🔊 **Digital I2S Audio Output:** High-priority MP3/WAV playback via **MAX98357A** DAC (`Audio.h`) and on-the-fly 880 Hz emergency tone generation.
* 💾 **Hybrid Storage Architecture:** Internal ESP32-S3 flash (**LittleFS**) for audio files, schedules, users, and WiFi configs + **MicroSD (FAT32)** for append-only AAA audit logs.
* 🖥️ **Flash-Embedded Web UI (`ui_embed.h`):** Responsive single-page dashboard requiring zero external SD/SPIFFS files to serve.
* 🕒 **Triple-Redundant Scheduling:** DS3231 Hardware RTC (primary), hourly NTP sync (`GMT+8`) (secondary), and `bootTime + millis()` software anchor (fallback) using the Sakamoto day-of-week algorithm.
* 🔐 **AAA Security & RBAC:** MD5 credential hashing, `Admin` & `Staff` role separation, `HttpOnly` session cookies (`10-min` timeout), brute-force lockout, and CSV audit trails.
* 🎛️ **Configurable Physical Controls:** Debounced Play button (`GPIO 4`), Push-to-Talk (`GPIO 14`), and Emergency Call Alert (`GPIO 21`).
* ⚙️ **FreeRTOS Multi-Tasking:** Prioritized task execution (`vWebTask`, `vSystemEngineTask`, `vAudioTask`) protected by thread-safe queues and mutexes.

---

## 🧭 Full Evolution Timeline (v1.3 $\rightarrow$ v3.1.0)

| Version | Milestone / Edition | Hardware & Core Stack | Key Capabilities & Architectural Shifts |
| :--- | :--- | :--- | :--- |
| **v1.3** | Standalone Embedded Bell | ESP32-WROOM-32, Relay Bell, DS3231 | Initial single-loop prototype with local DS3231 RTC scheduling and physical bell triggering. |
| **v1.7** | Interactive IoT & AAA Web Server | ESP32-WROOM-32, `WebServer.h`, SPIFFS + SD (`GPIO 5`), Relay (`GPIO 25`) | Synchronous HTTP server, `ESPSESSIONID` cookie auth, 3-tier RBAC (`SuperAdmin`, `Admin`, `User`), MD5 hashing, brute-force lockout (4 attempts), plaintext `/schedule.txt` & `/config.txt` on SPIFFS, SD-streamed `/ui/` HTML pages, and physical bell relay (`GPIO 25`). |
| **v2.1.0** | FreeRTOS Dual-Core Prototype | ESP32-WROOM-32, FreeRTOS (Core 0 + Core 1) | Introduced dual-core FreeRTOS task split (`vSnifferTask` on Core 0; `vWebTask`, `vAudioTask`, `vSystemEngineTask` on Core 1), thread-safe `xSdMutex` access, and auto-toggling attendance sniffer. |
| **v2.3.0-S3** | ESP32-S3 Hardware & I2S Migration | **ESP32-S3**, `ESPAsyncWebServer`, `driver/i2s_std.h`, SPIFFS + SD (`GPIO 10`) | Migrated pinout to ESP32-S3; replaced physical relay (`GPIO 25`) with native ESP-IDF `i2s_std` WAV audio output (`BCLK=15`, `LRC=16`, `DIN=7`); added physical `PTT` (`GPIO 14`) and `Call Alert` (`GPIO 21`) buttons; JSON schedules (`/data/schedule.json`), activity ring buffer (`/data/activity.csv`), and SHA1 IP-bound session cookies. |
| **v3.0.0** | Smart Alarm Controller Pivot | ESP32-S3, `Audio.h` (MP3/WAV), `ui_embed.h`, SD-centric | Deprecated passive WiFi probe-sniffing attendance (`vSnifferTask`, `xLogQueue`) to focus on dedicated alarm scheduling; introduced `AlarmEntry_t` model, flash-embedded UI (`ui_embed.h`), Sakamoto DOW calculation, non-blocking WiFi STA state machine, multi-speed SD initialization (`4MHz` $\rightarrow$ `200kHz`), and CSV AAA audit logging (`/security_log.txt`). |
| **v3.1.0** | **Hybrid Flash & Multi-User Edition** *(Current)* | ESP32-S3, **LittleFS + SD**, `GPIO 4` Play Button | Migrated alarms, users, WiFi configs, and audio files from SD to internal **LittleFS** (`xLfsMutex`) to eliminate SPI bus contention; added physical **Play Button (`GPIO 4`)** with web configuration; added database-backed multi-user RBAC (`Admin` / `Staff`); improved WiFi STA auto-recovery (`wifiSTAPoll`) and alarm `lastFired` logic. |

---

## 🧠 Architecture (v3.1.0 — FreeRTOS Edition)

### Core 1 (Application + Real-Time)
* `vWebTask` (Priority 1): `DNSServer` captive-portal processing, non-blocking WiFi STA polling (`wifiSTAPoll`), and periodic NTP re-synchronization.
* `vSystemEngineTask` (Priority 3): DS3231 RTC schedule evaluation, debounced physical button handling (`GPIO 4`, `14`, `21`), and periodic SD card health monitoring.
* `vAudioTask` (Priority 5 — Highest): High-priority I2S audio streaming from LittleFS via `Audio.h` and on-the-fly 880 Hz emergency WAV synthesis.

### Shared Synchronization Primitives
* `xAudioQueue`: Thread-safe queue dispatching audio playback requests from the system engine and web endpoints to `vAudioTask`.
* `xI2CMutex` / `xAlarmMutex` / `xLfsMutex` / `xSdMutex`: Dedicated mutexes serializing access to the DS3231 RTC, in-memory alarm array, internal LittleFS flash, and external SPI SD card.

---

## 🔌 Hardware Pinout (ESP32-S3 — v2.3.0-S3 through v3.1.0)

| Peripheral | Signal | ESP32-S3 GPIO | Notes |
| :--- | :--- | :--- | :--- |
| **I2S Audio (MAX98357A)** | `DIN` | **GPIO 7** | Serial audio data |
| | `BCLK` | **GPIO 15** | Bit clock |
| | `LRC` | **GPIO 16** | Left/Right word select |
| **I2C RTC (DS3231)** | `SDA` | **GPIO 8** | I2C data |
| | `SCL` | **GPIO 9** | I2C clock |
| **SPI MicroSD Card** | `CS` | **GPIO 10** | Chip select (FSPI) |
| | `MOSI` | **GPIO 11** | Master Out Slave In |
| | `SCK` | **GPIO 12** | Serial clock |
| | `MISO` | **GPIO 13** | Master In Slave Out |
| **Physical Buttons** | `Play Button` | **GPIO 4** | *(Added in v3.1.0)* Configurable chime trigger (`INPUT_PULLUP`) |
| | `PTT Button` | **GPIO 14** | Push-to-Talk input (`INPUT_PULLUP`) |
| | `Call Alert` | **GPIO 21** | Emergency 880 Hz alert trigger (`INPUT_PULLUP`) |

*(Note: Legacy v1.3–v1.7 builds targeted ESP32-WROOM-32 using `GPIO 25` for a physical bell relay and `GPIO 5` for SD CS.)*

---

## ✨ Key Features (v3.1.0)

### 🔐 Authentication & AAA Security
* Multi-user authentication backed by `/data/users.json` (`Admin` and `Staff` roles) with MD5 password hashing.
* `HttpOnly` session cookies (`10-minute` idle timeout, up to 8 concurrent sessions).
* Brute-force protection (5-minute lockout after 5 failed login attempts).
* Role-based access control (`requireAdmin()`) protecting user management, WiFi settings, and audit logs.
* Append-only CSV security audit trail (`/security_log.txt` on SD).

### 🔊 Audio System & Physical Controls
* **Play Button (`GPIO 4`):** Configurable physical button (and `/api/play-button` web trigger) that plays any uploaded `.mp3`/`.wav` file assigned via `/api/play-button-config`.
* **Emergency Call Alert (`GPIO 21`):** Generates and plays an 880 Hz square-wave alert tone (`/_alarm880.wav`).
* **Push-to-Talk (`GPIO 14`):** Debounced hardware input logged to the audit trail.

### 💾 Hybrid Storage Layout
* **Internal Flash (`LittleFS`):** Stores alarm schedules (`/data/alarms.json`), user accounts (`/data/users.json`), WiFi STA credentials (`/data/wifi.json`), uploaded audio files (`/chime.mp3`, etc.), and generated emergency tones—eliminating SPI bus contention during playback.
* **MicroSD Card (`FAT32`):** Dedicated to append-only AAA audit logs (`/security_log.txt`) and activity logs (`/data/activity.csv`), protected by automatic failure-backoff remounting.

### 🕒 Scheduling & Timekeeping
* Up to 20 recurring alarms (`AlarmEntry_t`) with 7-bit day-of-week bitmasks (`Sun`–`Sat`) calculated via the Sakamoto algorithm.
* Smart `lastFired` tracking prevents duplicate triggers within the same day and avoids unintended ringing when creating past-time alarms or toggling labels/states.
* Triple-redundant clock: DS3231 RTC $\rightarrow$ hourly NTP (`pool.ntp.org`, GMT+8) $\rightarrow$ `bootTime + millis()` fallback.

---

## 🔧 Libraries & Dependencies

| Library | Version | Purpose |
| :--- | :--- | :--- |
| ESP32 Arduino Core | 3.3.x | ESP32-S3 platform support |
| ESPAsyncWebServer | 3.5.0 | Async HTTP server & captive portal |
| AsyncTCP | 3.x (matching version) | Async TCP backend |
| Audio.h (schreibfaul1) | latest | I2S MP3/WAV playback from LittleFS |
| RTClib (Adafruit) | latest | DS3231 I2C RTC driver |
| ArduinoJson | 7.x | JSON serialization/deserialization |
| LittleFS (core) | — | Internal flash storage for configs & audio |
| SD / SPI (core) | — | External SD storage for audit logs |
| DNSServer (core) | — | Captive portal DNS redirection |
| mbedtls/md5.h (core) | — | Password hashing |

> ⚠️ **Note:** `ESPAsyncWebServer` and `AsyncTCP` must come from the same source and matching versions—a mismatch causes a `const`-qualifier compilation error in `AsyncServer::status()`.

---

## 🚀 Getting Started

1. Flash `capstone2.2.ino` (firmware **v3.1.0**) to the **ESP32-S3**.
2. **Set `AP_PASSWORD` and `ADMIN_PASS`** in the sketch before production deployment (defaults: `ChangeMe123!` and `admin` / `admin123`).
3. Prepare storage:
   * **Internal Flash (`LittleFS`):** Auto-formats on first boot and creates `/data/users.json`. Upload `/chime.mp3` and custom audio files via the web dashboard (`/api/upload`).
   * **MicroSD Card (optional, FAT32):** Insert an SD card to record `/security_log.txt` and `/data/activity.csv` (the system operates normally even if no SD card is present).
4. Power on — the ESP32-S3 broadcasts SoftAP `FCU_Alarm_Bell` (`172.217.28.1`).
5. Connect via phone or PC; the captive portal opens the dashboard automatically.

---

## 📝 Detailed Changelog

* **v3.1.0 (Current — Hybrid Flash & Multi-User Edition)**
  * Migrated alarms, users, WiFi credentials, and audio files from SD card to internal ESP32-S3 **LittleFS** (`xLfsMutex`) to eliminate SPI bus contention.
  * Added physical **Play Button (`GPIO 4`)** with web configuration (`/api/play-button-config`) and remote trigger (`/api/play-button`).
  * Added database-backed multi-user authentication (`managedUserAuth`) supporting `Admin` and `Staff` roles, account status checks, and blank-password protection.
  * Refactored WiFi STA into a continuous polling state machine (`wifiSTAPoll`) with automatic disconnect recovery and immediate credential updates.
  * Improved alarm `lastFired` logic to prevent false triggers on creation/edits and added flexible day-mask/array parsing.
  * Tuned SD card health checks (60s interval, 120s remount cooldown, 3-strike auto-suspend) and fixed mutex initialization order.
* **v3.0.0 (Smart Alarm Controller Pivot & Hardening)**
  * Deprecated passive WiFi probe-sniffing attendance modules (`vSnifferTask`, `xLogQueue`) to focus on dedicated alarm scheduling and emergency alerting.
  * Replaced `driver/i2s_std.h` WAV-only playback with `Audio.h` MP3/WAV decoder and dynamic 880 Hz emergency WAV synthesis.
  * Embedded web UI directly into flash (`ui_embed.h`), added Sakamoto DOW math, non-blocking WiFi STA connection, multi-speed SD initialization (`4MHz` $\rightarrow$ `200kHz`), and CSV AAA audit logging (`/security_log.txt`).
* **v2.3.0-S3 (ESP32-S3 Hardware & Native I2S Migration)**
  * Migrated hardware target from ESP32-WROOM-32 to **ESP32-S3** with updated SPI (`10–13`), I2C (`8–9`), I2S (`7, 15, 16`), and button (`14, 21`) pinouts.
  * Replaced physical bell relay (`GPIO 25`) with ESP-IDF standard I2S WAV audio output (`driver/i2s_std.h`) and chunked playback (`WAV_CHUNK = 4096`).
  * Migrated web server to `ESPAsyncWebServer` + `AsyncTCP` with JSON schedule APIs (`/api/schedule`), activity ring-buffer logging (`/data/activity.csv`), and SHA1 IP-bound session cookies.
* **v2.1.0 (FreeRTOS Dual-Core Prototype)**
  * Migrated to FreeRTOS dual-core task architecture (`vSnifferTask` on Core 0; `vWebTask`, `vAudioTask`, `vSystemEngineTask` on Core 1), thread-safe `xSdMutex` access, WPA2-secured AP, and chunk-safe JSON parsing.
* **v1.7 (Interactive IoT & AAA Web Server)**
  * Built synchronous `WebServer` dashboard with `ESPSESSIONID` cookie auth, 3-tier RBAC (`SuperAdmin`, `Admin`, `User`), MD5 hashing, 4-attempt brute-force lockout, SPIFFS plaintext schedule/config storage (`/schedule.txt`, `/config.txt`), SD-streamed HTML UI (`/ui/login.html`, `/ui/dashboard.html`), and physical relay bell (`GPIO 25`).
* **v1.3 (Standalone Embedded Device)**
  * Initial prototype featuring DS3231 RTC scheduling and standalone physical bell triggering.