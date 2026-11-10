# FCU Smart Bell & Alarm Controller (`capstone_elem_alarm-clock`)

[![Version](https://img.shields.io/badge/version-3.0.0-blue.svg)](https://github.com/)
[![Platform](https://img.shields.io/badge/platform-ESP32--S3-orange.svg)](https://www.espressif.com/)
[![Framework](https://img.shields.io/badge/framework-Arduino%20%2F%20FreeRTOS-green.svg)](https://www.freertos.org/)
[![License](https://img.shields.io/badge/license-MIT-brightgreen.svg)](LICENSE)

A resilient, multi-core ESP32-S3 firmware transforming the FCU Alarm Clock into an automated schedule manager and emergency alert system. Features a responsive captive-portal web UI, FreeRTOS multi-threading, triple-redundant timekeeping, I2S audio playback, and append-only AAA audit logging.

---

## 📋 Table of Contents
- [Executive Overview](#-executive-overview)
- [System Architecture Evolution](#-system-architecture-evolution)
  - [Phase 1: Architectural Rewrite (Original → v2.0.0)](#phase-1-architectural-rewrite-original--v200)
  - [Phase 2: Refinement & Hardening (v2.0.0 → v3.0.0)](#phase-2-refinement--hardening-v200--v300)
- [Detailed Fix Analysis](#-detailed-fix-analysis)
- [v3.0.0 System Architecture](#-v300-system-architecture)
- [Hardware Configuration & Pinout](#-hardware-configuration--pinout)
- [SD Card Directory Structure](#-sd-card-directory-structure)
- [Dependencies & Libraries](#-dependencies--libraries)
- [Installation & Setup](#-installation--setup)

---

## Executive Overview

The development of `capstone_elem_alarm-clock` spans two major architectural phases:

1. **Phase 1 (Original $\rightarrow$ v2.0.0): Core Architectural Rewrite**  
   Transitioned the firmware from a passive WiFi sniffer and attendance tracking tool into a phone-style alarm clock system with a captive portal web UI, FreeRTOS dual-core task scheduling, persistent SD card storage, and security audit logging.
2. **Phase 2 (v2.0.0 $\rightarrow$ v3.0.0): Stabilization, Refinement & Hardening**  
   Focused on resolving critical timing and scheduling bugs, multi-core thread safety vulnerabilities, Task Watchdog Timer (TWDT) panics from blocking WiFi routines, SD card SPI bus mutex corruption, user management authentication gaps, and overall SD card read/write reliability.

---

## System Architecture Evolution

```
[ Original Firmware ]                  [ v3.0.0 Architecture ]
┌─────────────────────────┐            ┌──────────────────────────────────────────────┐
│  Single-Threaded Loop   │            │ FreeRTOS Core 1 Multitasking                 │
├─────────────────────────┤            ├──────────────────┬───────────────────────────┤
│ • WiFi Sniffer          │            │ vWebTask (Pri 1) │ Captive Portal & Web UI   │
│ • Attendance Engine     │ ─────────► ├──────────────────┼───────────────────────────┤
│ • Promiscuous Capture   │            │ vSystemEngine    │ RTC, Alarms, Debounce,    │
│ • MAC Logging           │            │ (Pri 3)          │ SD Health Monitoring      │
└─────────────────────────┘            ├──────────────────┼───────────────────────────┤
                                       │ vAudioTask (Pri 5│ I2S MP3 Audio Playback    │
                                       └──────────────────┴───────────────────────────┘
```

### Phase 1: Architectural Rewrite (Original → v2.0.0)

* **Deprecation of Legacy Sniffer Infrastructure:**  
  Completely removed promiscuous-mode WiFi sniffing (`vSnifferTask`), MAC address logging, probe request capture pipelines, `LogEntry_t` structs, and the `xLogQueue` to dedicate system resources exclusively to scheduling, timekeeping, and audio alert pipelines.

* **Phone-Style Alarm Model (`AlarmEntry_t`):**  
  Introduced a unified data structure supporting unique IDs, custom text labels, 12-hour/24-hour conversions, enable/disable toggles, custom audio file paths, last-fired tracking, and a Sunday–Saturday bitmask for recurring schedules. Stores up to 20 alarms persistently in `/data/alarms.json`.

* **FreeRTOS Dual-Core Task Execution:**  
  Replaced the monolithic single-threaded `loop()` with three prioritized FreeRTOS tasks pinned to Core 1:
  * **`vWebTask` (Priority 1):** Captive portal DNS server handling, asynchronous HTTP requests, and background non-blocking WiFi STA connection processing.
  * **`vAudioTask` (Priority 5 - Highest):** High-priority decoder streaming MP3 audio directly from the SD card to the I2S DAC via `Audio.h`.
  * **`vSystemEngineTask` (Priority 3):** Real-time clock evaluation, schedule matching, physical button debouncing, and SPI SD card health checks.
  * **Main Loop:** Replaced with `vTaskDelay(portMAX_DELAY)`.

* **Thread Concurrency & Primitives:**  
  Implemented thread-safe communication primitives to prevent race conditions:
  * **Queue:** `xAudioQueue` (dispatches audio triggers from system engine to audio task).
  * **Mutexes:** `xI2CMutex` (DS3231 RTC), `xAlarmMutex` (in-memory alarm array), and `xSdMutex` (shared SPI SD card bus).

* **Captive Portal & Security (AAA):**  
  Integrated `DNSServer` for wildcard redirect handling (`/generate_204`, `/hotspot-detect.html`) operating on SoftAP `FCU_Alarm_Bell` at IP `172.217.28.1`. Web interfaces enforce session authentication (`HttpOnly` cookies, 5-attempt lockout) and Role-Based Access Control (`requireAdmin()`).

* **Redundant Timekeeping Network:**  
  Implemented a triple-layer time architecture:
  1. **Primary:** DS3231 Hardware RTC over I2C.
  2. **Secondary:** Network Time Protocol (NTP, `pool.ntp.org`, GMT+8 Manila) updated on boot and hourly.
  3. **Fallback:** Synthesized software clock using `bootTime + millis()` anchor.
  * Day-of-week index calculations utilize the Sakamoto algorithm ($0 = \text{Sunday}$).

---

### Phase 2: Refinement & Hardening (v2.0.0 → v3.0.0)

Phase 2 addressed critical edge cases, hardware race conditions, watchdog timeouts, and file system corruption identified in production builds of v2.0.0.

## Detailed Fix Analysis

| Category | Issue / Failure Mode | Root Cause | v3.0.0 Resolution |
| :--- | :--- | :--- | :--- |
| **Scheduler** | Incorrect firing days | Inconsistent RTC library `dayOfTheWeek()` outputs across toolchains. | Standardized on `computeDow()` (Sakamoto algorithm: $0 = \text{Sunday} \dots 6 = \text{Saturday}$). |
| **Scheduler** | Duplicate alarm triggers | Simple second equality check (`secs == alarmSecs`) refired when tasks were delayed. | Added `lastFired` tracking (`dayStart + alarmSecs`). Evaluates `secs >= alarmSecs && lastFired != fireKey`. |
| **Watchdog** | TWDT system reboot | Blocking 15s `while` loop in `connectWiFiSTA()` starved `async_tcp` on Core 1. | Replaced with non-blocking state machine checked on non-blocking 100ms task cycles. |
| **Concurrency**| FreeRTOS assertion crash | `sdRemount()` executed double `sdGive()` calls without prior `sdTake()`. | Enforced strict single `sdTake(500)` and `sdGive()` execution across all return paths. |
| **Storage** | Unreliable SD card mounts | High SPI clock rate (10MHz), missing bus retry logic, unhandled lockups. | Implemented multi-speed initialization fallback (4MHz $\rightarrow$ 200kHz), bus delays, auto-remount monitoring, and diagnostic logs. |
| **Web Server** | Buffer cross-talk | Shared `static String` buffers corrupted during simultaneous REST requests. | Isolated HTTP body buffers per endpoint domain and added strict mutex acquisition state checks (`tookSd`). |
| **User Access**| Missing credential hashing | User records lacked persistent password hashes and status toggles. | Added MD5 password hashing to `/api/users`, `Active/Disabled` states, and protected `admin` deletion. |
| **UI / UX** | Stale web interface data | Web components required full page reloads to show SD card changes. | Added manual **Refresh** triggers bound to `loadAudioFiles()` and `loadUsers()`. Single-page grid view deployed. |
| **Build System**| Out-of-sync embedded UI | `ui_embed.h` contained legacy HTML bytes missing modern dashboard changes. | Created PowerShell automation to generate `ui_embed.h` from `ui/index.html` (33,978 bytes, CRLF, 12 bytes/line). |

---

## v3.0.0 System Architecture

```
                                  ┌───────────────────────────┐
                                  │   DS3231 Real-Time Clock  │
                                  └─────────────┬─────────────┘
                                                │ (I2C Bus)
                                                ▼
┌───────────────────┐            ┌────────────────────────────┐            ┌───────────────────┐
│ Captive Portal    │            │ FreeRTOS Core 1 Task Model │            │ I2S Audio Out     │
│ async_tcp / Web   │◄───────────┤  vSystemEngineTask (Pri 3) ├───────────►│ (MAX98357A)       │
└─────────┬─────────┘            └──────────────┬─────────────┘            └───────────────────┘
          │                                     │
          ▼                                     ▼
┌───────────────────┐            ┌────────────────────────────┐
│   NTP Time Sync   │            │  SPI SD Card File System   │
│  (GMT+8 Manila)   │            │   /data/  /audio/  /logs/  │
└───────────────────┘            └────────────────────────────┘
```

---

## Hardware Configuration & Pinout

The system is configured for standard ESP32-S3 microcontrollers paired with an I2S DAC decoder, DS3231 RTC module, and standard SPI MicroSD card breakout.

| Peripheral | Functional Signal | ESP32-S3 GPIO | Description |
| :--- | :--- | :--- | :--- |
| **I2S Audio Output** | `DIN` | **GPIO 7** | Serial Data input to MAX98357A DAC |
| | `BCLK` | **GPIO 15** | Bit Clock |
| | `LRC` | **GPIO 16** | Word Select (Left/Right Channel Clock) |
| **I2C RTC Bus** | `SDA` | **GPIO 8** | DS3231 Serial Data |
| | `SCL` | **GPIO 9** | DS3231 Serial Clock |
| **SPI SD Card** | `CS` | **GPIO 10** | Chip Select |
| | `SCK` | **GPIO 12** | Serial Clock |
| | `MISO` | **GPIO 13** | Master In Slave Out |
| | `MOSI` | **GPIO 11** | Master Out Slave In |
| **Physical Controls**| `PTT Button` | **GPIO 14** | Push-to-Talk active-low input |
| | `Call Alert` | **GPIO 21** | Emergency manual alert input switch |

---

## SD Card Directory Structure

Ensure the MicroSD card (FAT32 formatted) is provisioned with the following initial layout before powering on the hardware:

```text
/
├── audio/
│   ├── chime.mp3          # Default notification tone
│   └── _alarm880.wav      # Dynamically generated emergency alert tone
├── data/
│   ├── alarms.json        # Alarm schedule entries
│   ├── users.json         # Encrypted accounts & authorization database
│   ├── wifi.json          # Cached Access Point STA configurations
│   └── activity.csv       # Runtime operation telemetry log
└── security_log.txt       # Append-only AAA security audit trail
```

---

## Dependencies & Libraries

To compile the firmware successfully using PlatformIO or Arduino IDE, install the following dependencies:

* **Core Drivers:** `WiFi.h`, `ESPAsyncWebServer`, `DNSServer`, `SPI.h`, `Wire.h`, `SD.h`
* **JSON Processing:** `ArduinoJson` (v7.x)
* **RTC Hardware Driver:** `RTClib` (Adafruit DS3231 library)
* **Audio Engine:** `schreibfaul1/ESP32-audioI2S` (`Audio.h`)
* **Cryptographic Support:** `mbedtls/md5.h`

---

## Installation & Setup

1. **Clone the repository:**
   ```bash
   git clone https://github.com/your-org/capstone_elem_alarm-clock.git
   cd capstone_elem_alarm-clock
   ```

2. **Prepare the Storage:**
   Format a MicroSD card to **FAT32** and copy the required directory tree from the `sd_card_template/` folder.

3. **Build Web Assets (Optional):**
   If modifying `ui/index.html`, regenerate the embedded header using the provided PowerShell utility:
   ```powershell
   .\scripts\generate_ui_embed.ps1
   ```

4. **Flash to Target Hardware:**
   Compile and flash to your ESP32-S3 target using PlatformIO or the Arduino IDE with partition scheme configured for `Default 4MB with spiffs` or larger.