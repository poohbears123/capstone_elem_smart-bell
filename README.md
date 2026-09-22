# FCU Smart Bell System

ESP32-based smart school bell and attendance monitoring system for FCU Elementary. The firmware is implemented in the Arduino/ESP32 environment and runs as a multi-task FreeRTOS application with a web dashboard, RTC and NTP timing, WiFi management, I2S audio playback, and attendance detection.

Current firmware version: `2.2.0`

## Project Overview

This project combines:

- ESP32 web server and API
- RTC-based bell schedule engine
- I2S audio playback through MAX98357A
- WiFi access point + station mode configuration
- SD card and SPIFFS data storage
- Passive attendance monitoring using WiFi probe requests
- Session-based authentication for the web dashboard
- Activity/event logging to persistent storage

## What Changed in This Update

This README was updated to reflect the current implementation instead of the older v1.7 documentation notes. The system has evolved into a more complete smart bell platform.

### 1. Firmware architecture is now FreeRTOS-based
The latest firmware uses multiple tasks for separate responsibilities:

- `vWebTask` handles the HTTP web dashboard and API
- `vSnifferTask` monitors WiFi probe frames for attendance capture
- `vAudioTask` manages I2S audio playback and bell queueing
- `vSystemEngineTask` handles RTC/NTP, schedule evaluation, and system timing

This makes the project much more stable and easier to expand than a single-loop Arduino sketch.

### 2. Real I2S audio playback is implemented
The audio system uses the MAX98357A amplifier and ESP32 I2S driver.

- `I2S_LRC_PIN = 27`
- `I2S_BCLK_PIN = 26`
- `I2S_DIN_PIN = 25`
- Audio files are read from SD and played as WAV data
- Large buffering is avoided to stay memory-friendly on ESP32
- Bell events can be triggered by schedule time, manual action, or emergency audio

### 3. SD card storage is now a primary runtime storage system
The current code uses SD for the main UI and data files, with SPIFFS as a fallback or helper storage layer.

- UI pages served from `SD:/ui/...`
- Schedule stored in `SD:/data/schedule.json`
- Activity logs stored in `SD:/data/activity.csv`
- Network config saved to SPIFFS at `/config.json`

### 4. Attendance monitoring is active in the firmware
The sniffer task monitors management frames and records device probe requests when attendance mode is enabled.

- Tracks MAC address, RSSI, and timestamp
- Deduplicates repeated events
- Saves results to `/data/logs.csv`
- Exposes attendance data via `/api/attendance` and `/api/attendance/status`

### 5. RTC + NTP time syncing is built in
Time is kept by a DS3231 RTC and synchronized to NTP when the ESP32 is connected to WiFi.

- `NTP_TZ_OFFSET_SEC = 28800` for Asia/Manila time
- Automatic retry schedule for station connectivity
- Time is used for bell scheduling and event timestamps

### 6. Login and web auth are session-based
The dashboard uses cookie-based authentication with a derived session token.

- Cookie name: `FCU_SESS`
- Default credentials are set in firmware as:
  - Username: `admin`
  - Password: `admin123`
- The system checks auth per route using `isAuthed()` and `guard()`

### 7. API routes now match the running application
The project exposes a modern set of endpoints for telemetry, schedule management, attendance data, activity log retrieval, and network configuration.

## System Diagram

```text
                         ┌──────────────────────────────┐
                         │        Browser / Client      │
                         │  login page + dashboard UI   │
                         └──────────────┬───────────────┘
                                        │ HTTP
                                        ▼
                         ┌──────────────────────────────┐
                         │        ESP32 Firmware        │
                         │   AsyncWebServer + FreeRTOS  │
                         │                              │
                         │  ┌──────────────────────┐    │
                         │  │ Web / Auth Layer     │    │
                         │  │ /, /handleLogin      │    │
                         │  │ /api/telemetry       │    │
                         │  │ /api/config          │    │
                         │  │ /api/schedule        │    │
                         │  │ /api/activity        │    │
                         │  └──────────────────────┘    │
                         │                              │
                         │  ┌──────────────────────┐    │
                         │  │ Timing + Schedule    │    │
                         │  │ RTC + NTP + engine   │    │
                         │  │ Bell trigger logic   │    │
                         │  └──────────────────────┘    │
                         │                              │
                         │  ┌──────────────────────┐    │
                         │  │ Audio + Storage      │    │
                         │  │ I2S MAX98357A        │    │
                         │  │ SD card + SPIFFS     │    │
                         │  │ WAV chime playback   │    │
                         │  └──────────────────────┘    │
                         │                              │
                         │  ┌──────────────────────┐    │
                         │  │ Attendance Monitor   │    │
                         │  │ WiFi probe sniffer  │    │
                         │  │ RSSI + MAC capture   │    │
                         │  └──────────────────────┘    │
                         └──────────────┬───────────────┘
                                        │
                                        ▼
                         ┌──────────────────────────────┐
                         │      External Components     │
                         │  - MAX98357A Speaker         │
                         │  - DS3231 RTC                │
                         │  - WiFi AP / Station         │
                         │  - SD storage               │
                         │  - Probe-request devices    │
                         └──────────────────────────────┘
```

## Hardware Configuration

| Component | Pin | Description |
|---|---|---|
| SD CS | GPIO 5 | SD card chip select |
| SD SCK | GPIO 18 | SD clock |
| SD MISO | GPIO 19 | SD data in |
| SD MOSI | GPIO 23 | SD data out |
| I2S DIN | GPIO 25 | Audio data |
| I2S BCLK | GPIO 26 | Audio bit clock |
| I2S LRC | GPIO 27 | Audio word clock |
| I2C SDA | GPIO 21 | RTC communication |
| I2C SCL | GPIO 22 | RTC communication |
| PTT Button | GPIO 14 | Manual or trigger input |
| CALL Alert | GPIO 13 | Alert output/use |

## Core Features

- Smart bell schedule engine with RTC-driven timing
- Manual bell trigger and emergency audio path
- Network configuration through the web UI
- Web dashboard served from the device itself
- File uploads and chime management from the dashboard
- Automatic or manual NTP synchronization
- Attendance detection using WiFi management frames
- Logging of system events and activity history

## Project Structure

```text
capstone2.1P/
├── README.md
├── audio/
├── capstone2.1.0P/
│   └── capstone2.1.0P.ino
├── data/
│   ├── logs.csv
│   └── schedule.json
├── new UI/
│   ├── activity-logs.html
│   ├── app.js
│   ├── index.html
│   ├── manual.html
│   ├── schedule.html
│   ├── style.css
│   └── user-management.html
├── ui/
│   ├── index.html
│   ├── login.html
│   ├── script.js
│   └── style.css
└── .gitignore
```

## Web API Summary

| Endpoint | Method | Notes |
|---|---|---|
| `/` | GET | Serves the login page or dashboard |
| `/test` | GET | Basic health check |
| `/api/telemetry` | GET | Returns device time, network state, attendance and schedule info |
| `/api/config` | GET | Reads saved network config |
| `/api/config` | POST | Saves and applies WiFi configuration |
| `/api/schedule` | GET | Returns JSON schedule |
| `/api/schedule` | POST | Saves schedule changes |
| `/api/attendance` | GET | Returns captured attendance CSV |
| `/api/attendance/status` | GET | Returns active/inactive status |
| `/api/activity` | GET | Returns recent activity events |
| `/api/time/sync` | POST | Manually sync RTC from NTP |
| `/handleLogin` | POST | Authenticates user and sets session cookie |
| `/logout` | GET | Clears authentication |
| `/ring` | GET | Trigger a bell ring |

## Authentication and Security

The web app uses a cookie-based session scheme.

- Cookie name: `FCU_SESS`
- Token generation is based on a deterministic SHA1 value derived from the username, password, and remote client IP
- Protected routes call `guard()` before allowing access
- Failed login attempts are counted and can temporarily lock the account
- All important events are logged and persisted to `/data/activity.csv`

## Data Files

| File | Purpose |
|---|---|
| `/config.json` | Saved AP and station WiFi settings in SPIFFS |
| `/data/schedule.json` | Bell schedule data |
| `/data/logs.csv` | Attendance/probe log records |
| `/data/activity.csv` | Activity and event log |

## Deployment Notes

Before deploying to a real device:

1. Change the AP password from the default value in the firmware.
2. Change the admin username/password if needed.
3. Ensure the SD card contains the required UI files in `/ui/`.
4. Verify the `MAX98357A` speaker wiring and RTC wiring are correct.
5. Confirm WiFi credentials for station mode are valid.

## Conclusion

The system is now a practical smart bell controller with a web-based admin dashboard, scheduled bell logic, live attendance monitoring, dynamic WiFi configuration, RTC/NTP synchronization, and audio management. The current README reflects the actual codebase and the current v2.2.0 feature set.

