# FCU Smart Bell & Alarm Controller (`capstone_elem_alarm-clock`)

[![Version](https://img.shields.io/badge/version-3.1.0-blue.svg)](https://github.com/)
[![Platform](https://img.shields.io/badge/platform-ESP32--S3-orange.svg)](https://www.espressif.com/)
[![Framework](https://img.shields.io/badge/framework-Arduino%20%2F%20FreeRTOS-green.svg)](https://www.freertos.org/)
[![License](https://img.shields.io/badge/license-MIT-brightgreen.svg)](LICENSE)

A resilient, multi-core ESP32-S3 firmware transforming the FCU Alarm Clock into an automated schedule manager and emergency alert system. Features a responsive captive-portal web UI, FreeRTOS multi-threading, triple-redundant timekeeping, hybrid LittleFS/SD storage, I2S audio playback, and append-only AAA audit logging.

---

## 📋 Table of Contents
- [Executive Overview](#-executive-overview)
- [System Architecture Evolution](#-system-architecture-evolution)
  - [Phase 1: Architectural Rewrite (Original → v2.0.0)](#phase-1-architectural-rewrite-original--v200)
  - [Phase 2: Refinement & Hardening (v2.0.0 → v3.0.0)](#phase-2-refinement--hardening-v200--v300)
  - [Phase 3: Hybrid Storage, Multi-User Auth & Play Button (v3.0.0 → v3.1.0)](#phase-3-hybrid-storage-multi-user-auth--play-button-v300--v310)
- [Detailed Fix Analysis](#-detailed-fix-analysis)
- [v3.1.0 System Architecture](#-v310-system-architecture)
- [Hardware Configuration & Pinout](#-hardware-configuration--pinout)
- [Storage Directory Structure (LittleFS & SD)](#-storage-directory-structure-littlefs--sd)
- [Dependencies & Libraries](#-dependencies--libraries)
- [Installation & Setup](#-installation--setup)

---

## Executive Overview

The development of `capstone_elem_alarm-clock` spans three major architectural phases:

1. **Phase 1 (Original $\rightarrow$ v2.0.0): Core Architectural Rewrite**  
   Transitioned the firmware from a passive WiFi sniffer and attendance tracking tool into a phone-style alarm clock system with a captive portal web UI, FreeRTOS dual-core task scheduling, persistent storage, and security audit logging.
2. **Phase 2 (v2.0.0 $\rightarrow$ v3.0.0): Stabilization, Refinement & Hardening**  
   Focused on resolving critical timing and scheduling bugs, multi-core thread safety vulnerabilities, Task Watchdog Timer (TWDT) panics from blocking WiFi routines, SD card SPI bus mutex corruption, and read/write reliability.
3. **Phase 3 (v3.0.0 $\rightarrow$ v3.1.0): Hybrid Flash Storage, Multi-User Auth & Play Button**  
   Migrated critical configuration, user databases, and audio assets to internal ESP32-S3 Flash (**LittleFS**) to eliminate SPI bus contention, introduced database-backed multi-user role authentication, added a configurable physical **Play Button** (GPIO 4), and overhauled WiFi STA connection recovery.

---

## System Architecture Evolution

```
[ Original Firmware ]                  [ v3.1.0 Architecture ]
┌─────────────────────────┐            ┌──────────────────────────────────────────────┐
│  Single-Threaded Loop   │            │ FreeRTOS Core 1 Multitasking                 │
├─────────────────────────┤            ├──────────────────┬───────────────────────────┤
│ • WiFi Sniffer          │            │ vWebTask (Pri 1) │ Captive Portal & Web UI   │
│ • Attendance Engine     │ ─────────► ├──────────────────┼───────────────────────────┤
│ • Promiscuous Capture   │            │ vSystemEngine    │ RTC, Alarms, Debounce,    │
│ • MAC Logging           │            │ (Pri 3)          │ SD Health Monitoring      │
└─────────────────────────┘            ├──────────────────┼───────────────────────────┤
                                       │ vAudioTask (Pri 5│ I2S Audio from LittleFS   │
                                       └──────────────────┴───────────────────────────┘
```

### Phase 1: Architectural Rewrite (Original → v2.0.0)

- **Deprecation of Legacy Sniffer Infrastructure:**  
  Completely removed promiscuous-mode WiFi sniffing (`vSnifferTask`), MAC address logging, probe request capture pipelines, `LogEntry_t` structs, and the `xLogQueue` to dedicate system resources exclusively to scheduling, timekeeping, and audio alert pipelines.

- **Phone-Style Alarm Model (`AlarmEntry_t`):**  
  Introduced a unified data structure supporting unique IDs, custom text labels, 12-hour/24-hour conversions, enable/disable toggles, custom audio file paths, last-fired tracking, and a Sunday–Saturday bitmask for recurring schedules.

- **FreeRTOS Dual-Core Task Execution:**  
  Replaced the monolithic single-threaded `loop()` with three prioritized FreeRTOS tasks pinned to Core 1:
  - **`vWebTask` (Priority 1):** Captive portal DNS server handling, asynchronous HTTP requests, and background non-blocking WiFi STA connection processing.
  - **`vAudioTask` (Priority 5 - Highest):** High-priority decoder streaming MP3/WAV audio directly to the I2S DAC via `Audio.h`.
  - **`vSystemEngineTask` (Priority 3):** Real-time clock evaluation, schedule matching, physical button debouncing, and SPI SD card health checks.
  - **Main Loop:** Replaced with `vTaskDelay(portMAX_DELAY)`.

- **Thread Concurrency & Primitives:**  
  Implemented thread-safe communication primitives (`xAudioQueue`, `xI2CMutex`, `xAlarmMutex`, `xSdMutex`, and `xLfsMutex`) to prevent race conditions across tasks.

- **Captive Portal & Security (AAA):**  
  Integrated `DNSServer` for wildcard redirect handling (`/generate_204`, `/hotspot-detect.html`) operating on SoftAP `FCU_Alarm_Bell` at IP `172.217.28.1`. Web interfaces enforce session authentication (`HttpOnly` cookies, 5-attempt lockout) and Role-Based Access Control (`requireAdmin()`).

- **Redundant Timekeeping Network:**  
  Implemented a triple-layer time architecture:
  1. **Primary:** DS3231 Hardware RTC over I2C.
  2. **Secondary:** Network Time Protocol (NTP, `pool.ntp.org`, GMT+8 Manila) updated on boot and hourly.
  3. **Fallback:** Synthesized software clock using `bootTime + millis()` anchor.
  - Day-of-week index calculations utilize the Sakamoto algorithm ($0 = \text{Sunday}$).

---

### Phase 2: Refinement & Hardening (v2.0.0 → v3.0.0)

Phase 2 addressed critical edge cases, hardware race conditions, watchdog timeouts, and file system corruption identified in early production builds.

### Phase 3: Hybrid Storage, Multi-User Auth & Play Button (v3.0.0 → v3.1.0)

- **Hybrid Storage Split (LittleFS + SD Card):**  
  Migrated alarms, WiFi credentials, user databases, and audio files from the external SPI SD card to internal ESP32-S3 Flash (**LittleFS**) guarded by `xLfsMutex`. Dedicated the SD card exclusively to append-only AAA audit logs (`/security_log.txt`) and activity logs.
- **Database-Backed Multi-User Authentication (`managedUserAuth`):**  
  Expanded login validation beyond the hardcoded `admin` account to authenticate `Admin` and `Staff` users stored in `/data/users.json` using MD5 hashing, status validation (`Active`), and blank-password login protection.
- **Configurable Physical & Web Play Button:**  
  Added support for a dedicated physical Play button on **GPIO 4** (`PLAY_BUTTON_PIN`) alongside `/api/play-button-config` and `/api/play-button` endpoints to dynamically assign and trigger custom chimes.
- **Non-Blocking WiFi STA State Machine (`wifiSTAPoll`):**  
  Decoupled WiFi connection monitoring from connection initiation to detect mid-session disconnects, trigger automatic NTP re-syncs upon recovery, and immediately apply updated credentials via `/api/wifi` without a reboot.

---

## Detailed Fix Analysis

| Category | Issue / Failure Mode | Root Cause | Resolution (v3.0.0 $\rightarrow$ v3.1.0) |
| :--- | :--- | :--- | :--- |
| **Storage / Bus** | SPI bus contention during audio playback | Audio streaming and log writes shared the SPI SD bus. | Migrated audio files, alarms, users, and WiFi config to internal **LittleFS** (`xLfsMutex`); isolated SD card to audit logs. |
| **SD Reliability** | CPU stalls when SD card is missing/unresponsive | Aggressive 10s health checks and unlimited 30s remount loops. | Tuned health checks to 60s, remount cooldown to 120s, and added `sdFailCount` to suspend remounting after 3 failures. |
| **Authentication**| Staff accounts could not log in | `/api/login` only checked hardcoded `ADMIN_USER` credentials. | Implemented `managedUserAuth()` to verify `/data/users.json` hashes, `Active` status, and bind dynamic roles (`Admin`/`Staff`). |
| **Scheduler** | Unintended ringing on alarm creation/edit | Creating a past-time alarm or toggling `enabled` reset `lastFired = 0`. | Initialized `lastFired` for past-time alarms on creation; only re-arm `lastFired = 0` when `hour`, `minute`, or `days` change. |
| **Scheduler** | Incorrect firing days | Inconsistent RTC library `dayOfTheWeek()` outputs across toolchains. | Standardized on `computeDow()` (Sakamoto algorithm) and flexible `parseDays()` supporting both bitmasks and day arrays. |
| **WiFi / NTP** | Stale connection state after network drop | `connectWiFiSTA()` did not actively monitor post-connect link drops. | Added continuous `wifiSTAPoll()` state machine to detect disconnects, log events, and re-trigger NTP sync on reconnect. |
| **Concurrency** | Mutex race condition on boot | Tasks and setup routines accessed FS helpers before mutex creation. | Moved all FreeRTOS queue and mutex initializations to the start of `setup()` and removed stray braces in `vSystemEngineTask`. |
| **Hardware / API**| Lack of customizable quick-trigger chime | Only fixed emergency tone (`CALL_ALERT_PIN`) was supported. | Added `PLAY_BUTTON_PIN` (GPIO 4) and `/api/play-button` + `/api/play-button-config` REST endpoints. |

---

## v3.1.0 System Architecture

```
                                  ┌───────────────────────────┐
                                  │   DS3231 Real-Time Clock  │
                                  └─────────────┬─────────────┘
                                                │ (I2C Bus)
                                                ▼
┌───────────────────┐            ┌────────────────────────────┐            ┌───────────────────┐
│ Captive Portal    │            │ FreeRTOS Core 1 Task Model │            │ I2S Audio Out     │
│ async_tcp / Web   │◄───────────┤  vSystemEngineTask (Pri 3) ├───────────►│ (MAX98357A)       │
└─────────┬─────────┘            └───────┬────────────┬───────┘            └─────────▲─────────┘
          │                              │            │                              │
          ▼                              ▼            ▼                              │
┌───────────────────┐   ┌─────────────────────┐  ┌───────────────────────────────────┴───┐
│   NTP Time Sync   │   │  SPI SD Card (Logs) │  │   Internal Flash (LittleFS)           │
│  (GMT+8 Manila)   │   │  /security_log.txt  │  │   /data/*.json & Audio (*.mp3/*.wav)  │
└───────────────────┘   └─────────────────────┘  └───────────────────────────────────────┘
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
| **Physical Controls**| `Play Button` | **GPIO 4** | Configurable chime trigger active-low input |
| | `PTT Button` | **GPIO 14** | Push-to-Talk active-low input |
| | `Call Alert` | **GPIO 21** | Emergency manual alert input switch |

---

## Storage Directory Structure (LittleFS & SD)

Starting in **v3.1.0**, storage is split between internal ESP32-S3 Flash (**LittleFS**) for low-latency configuration/audio access and the external **MicroSD Card** for append-only audit logs:

### Internal Flash (`LittleFS`)
```text
/
├── chime.mp3              # Default notification & Play Button tone
├── _alarm880.wav          # Dynamically generated emergency alert tone
└── data/
    ├── alarms.json        # Alarm schedule entries (bitmask or day array)
    ├── users.json         # MD5-hashed accounts & role database
    └── wifi.json          # Cached WiFi STA credentials
```

### External MicroSD Card (`FAT32`)
```text
/
├── data/
│   └── activity.csv       # Runtime operation telemetry log
└── security_log.txt       # Append-only AAA security audit trail
```

---

## Dependencies & Libraries

To compile the firmware successfully using PlatformIO or Arduino IDE, install the following dependencies:

- **Core Drivers:** `WiFi.h`, `ESPAsyncWebServer`, `DNSServer`, `SPI.h`, `Wire.h`, `SD.h`, `LittleFS.h`
- **JSON Processing:** `ArduinoJson` (v7.x)
- **RTC Hardware Driver:** `RTClib` (Adafruit DS3231 library)
- **Audio Engine:** `schreibfaul1/ESP32-audioI2S` (`Audio.h`)
- **Cryptographic Support:** `mbedtls/md5.h`