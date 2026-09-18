```markdown
# capstone_elem_smart-bell — Migration & Evolution Report (v1.7 → v2.1.0)

A comprehensive breakdown of the architectural rewrite, feature transitions, bug fixes, and system improvements across the development phases of the FCU Elementary Smart Bell system.

---

## Executive Overview

The evolution of **capstone_elem_smart-bell** spans two major development phases:

1. **Phase 1 (v1.7 → v2.0.6):** Complete architectural rewrite. The firmware shifted from a single-threaded, linear Arduino loop to a dual-core, event-driven FreeRTOS task model with integrated WiFi probe sniffing and an upgraded audio framework.
2. **Phase 2 (v2.0.6 → v2.1.0):** Stabilization, bug fixes, and safety hardening. Resolved core timing and scheduling bugs, thread-safety issues, memory leaks, and missing functionality to deliver a production-ready capstone build.

---

## Phase 1: Architectural Rewrite (v1.7 → v2.0.6)

### 1. FreeRTOS Dual-Core Task Model
* **v1.7 Architecture:** Ran as a single-threaded linear Arduino sketch using standard `loop()` delays and direct function calls.
* **v2.0.6 Architecture:** Fully restructured into four dedicated FreeRTOS tasks distributed across both ESP32 cores:
  * `vSnifferTask` (**Core 0**, Priority 2): Background WiFi probe request sniffing and attendance logging.
  * `vWebTask` (**Core 1**, Priority 1): Asynchronous web server handling and captive portal DNS processing.
  * `vSystemEngineTask` (**Core 1**, Priority 3): Hardware button polling, edge detection, and real-time DS3231 RTC schedule matching.
  * `vAudioTask` (**Core 1**, Priority 5 - Highest): High-priority I2S audio decoding and playback.
* **Main Loop Modification:** `loop()` was emptied and replaced with `vTaskDelay(portMAX_DELAY)`, leaving all system execution to FreeRTOS tasks.

### 2. Inter-Task Communication & Thread Synchronization
* **v1.7 Implementation:** Shared global variables accessed directly without mutexes or thread guards.
* **v2.0.6 Implementation:** Introduced FreeRTOS concurrency primitives to eliminate race conditions and memory corruption:
  * **Queues:** `xLogQueue` (event and attendance logging) and `xAudioQueue` (audio dispatch requests) for thread-safe inter-task communication.
  * **Mutexes:** `xI2CMutex` (RTC access), `xScheduleMutex` (JSON schedule memory access), and `xSdMutex` (SPI SD card access).

### 3. Audio Processing Upgrade
* **v1.7 Audio Engine:** Used direct hardware I2S calls (`setupI2S()`, `i2sPlayTone()`, `triggerPhysicalBell()`) to synthesize raw square wave/sine wave tones.
* **v2.0.6 Audio Engine:** Migrated to the `Audio.h` library by schreibfaul1. Streams high-quality digital MP3 audio directly from the SD card through a dedicated audio queue handled by `vAudioTask` on Core 1.

### 4. Autonomous Attendance Sniffing Engine
* **v1.7 Implementation:** Non-existent.
* **v2.0.6 Implementation:** Added background promiscuous-mode WiFi sniffing (`vSnifferTask`). Captures incoming mobile device Probe Requests, de-duplicates MAC addresses, and writes timestamps directly to CSV storage.

### 5. Captive Portal Network Engine
* **v1.7 Implementation:** Standard AP / Station network hosting without DNS redirection.
* **v2.0.6 Implementation:** Integrated `DNSServer` with wildcards to catch and redirect incoming network traffic automatically across iOS, Android, and Windows devices.

### 6. Schedule Data Structure Migration
* **v1.7 Schedule Storage:** Plain-text line-based configuration (`/data/schedule.txt`) storing flat schedule times.
* **v2.0.6 Schedule Storage:** Structured JSON format (`/data/schedule.json`) powered by `ArduinoJson 7.x`. Supports bitmask day-of-week selection and per-entry MP3 file paths.

---

## Phase 2: Refinement & Stabilization (v2.0.6 → v2.1.0)

Phase 2 resolved critical behavioral bugs, hardware race conditions, and stability bottlenecks identified in v2.0.6.

### Detailed Fix Analysis

#### 1. Day-of-Week Index Mapping Correction
* **Problem:** Schedules fired on incorrect days of the week.
* **Cause:** Redundant custom conversion function `dowToIndex()` corrupted the day calculation. The hardware RTC library `RTClib` natively returns `0 = Sunday` through `6 = Saturday`.
* **Fix:** Removed `dowToIndex()`, aligning day-of-week bitmask evaluation directly with `rtc.now().dayOfTheWeek()`.

#### 2. Exactly-Once-Per-Day Chime Guard
* **Problem:** Scheduled chimes fired multiple times per minute.
* **Cause:** The 1-second system engine evaluation tick repeatedly matched schedule entry conditions within a multi-second evaluation window.
* **Fix:** Added per-entry tracking arrays (`lastFiredStart[]` and `lastFiredEnd[]`). Stores the epoch timestamp of the trigger event and locks duplicate firings until the day rolls over or the schedule reloads.

#### 3. Schedule Mutex Deadlock / Stall Fix
* **Problem:** System froze for 100 ms during schedule reloads.
* **Cause:** A non-recursive mutex `xScheduleMutex` was requested twice sequentially before releasing the initial lock.
* **Fix:** Re-ordered memory unlock routines so schedule reloads execute outside the write-lock boundary.

#### 4. Real-Time Attendance CSV Timestamps
* **Problem:** Captured attendance entries recorded fake placeholder timestamps (`2026-01-01 00:00:00`).
* **Cause:** `vSnifferTask` attempted to perform I2C RTC reads directly inside interrupt context or without reading valid clock registers.
* **Fix:** Moved RTC reads into `vSnifferTask`'s normal execution loop, querying real-time clock data under `xI2CMutex` locks.

#### 5. Hardware Button Debouncing & Edge Detection
* **Problem:** Holding the physical emergency button triggered continuous alert fires every second.
* **Cause:** Level-triggered GPIO state checking without state memory.
* **Fix:** Implemented rising/falling edge detection with state memory. Alerts fire exactly once per press and re-arm only upon full release.

#### 6. SD Card Thread Serialization (`xSdMutex`)
* **Problem:** Intermittent file system corruption and SPI crashes.
* **Cause:** `vAudioTask`, `vSnifferTask`, and `vWebTask` accessed the shared SPI SD card simultaneously without synchronization.
* **Fix:** Wrapped all file system calls in safe helper routines (`sdExists()`, `sdReadFile()`, `appendCSVLine()`) guarded strictly by `xSdMutex`.

#### 7. Access Point WPA2 Security Layer
* **Problem:** Wi-Fi Access Point remained unencrypted despite using the `FCU_Secure_Bell` name.
* **Fix:** Added `AP_PASSWORD` configuration checks to enforce WPA2 PSK encryption across all wireless client connections.

#### 8. Chunk-Safe HTTP POST Parsing (`/api/schedule`)
* **Problem:** Large schedule upload requests failed or crashed the web server.
* **Cause:** `ESPAsyncWebServer` delivered large JSON bodies in multi-part buffer chunks, which were truncated during parsing.
* **Fix:** Replaced single-buffer reads with a multi-chunk aggregator. Accumulates body chunks up to 4 KB, validates completeness, and returns HTTP 413 or 400 upon buffer overflow or malformed payloads.

#### 9. Optimized RTC Hardware Initialization
* **Problem:** I2C bus congestion and high latency during web request processing.
* **Cause:** `rtc.begin()` ran bus scans continuously inside secondary polling loops.
* **Fix:** Restricted `rtc.begin()` to a single initialization pass during `setup()`, tracking operational state with a global `rtcOK` boolean flag.

#### 10. Non-Blocking Flag-Driven Schedule Persistence
* **Problem:** Web server dropped incoming TCP packets when saving schedule modifications.
* **Cause:** File system writes executed directly inside asynchronous web handler callback threads.
* **Fix:** Handlers update memory structures and set a `schedulePendingSave` flag. `vSystemEngineTask` processes disk persistence in the background.

#### 11. Autonomous Sniffer Auto-Toggling
* **Problem:** Attendance sniffer remained inactive (`snifferActive = false`).
* **Fix:** Connected `vSnifferTask` execution to class time schedules. Promiscuous mode automatically turns **ON** during configured attendance windows and **OFF** during regular hours to preserve access point bandwidth.

---

# capstone_elem_smart-bell — v2.1.0

The **FreeRTOS & Autonomous Attendance Edition** of the FCU Elementary Smart Bell system.

## What's New in v2.1.0

### FreeRTOS Dual-Core Architecture
Migrated the entire core system from a single-threaded blocking loop to an event-driven, multi-tasking **FreeRTOS dual-core task architecture**. Workloads are distributed across both cores to guarantee zero audio stutter and real-time network responsiveness:
- **Core 0 (Protocol Core)**: Runs `vSnifferTask` (Priority 2) for promiscuous mode packet capture and passive attendance logging.
- **Core 1 (Application Core)**: Runs web, audio, and system automation tasks:
  - `vWebTask` (Priority 1) — Manages `DNSServer` captive portal processing and async HTTP endpoints.
  - `vSystemEngineTask` (Priority 3) — Real-time DS3231 RTC evaluation, edge-detected button inputs, and sniffer window toggling.
  - `vAudioTask` (Priority 5 - Highest) — High-priority I2S MP3 streaming directly from the SD card using `Audio.h`.
- **Thread Safety & Inter-Task Communication**:
  - **Queues (`xLogQueue`, `xAudioQueue`)** — Thread-safe and ISR-safe messaging between protocol tasks and hardware output drivers.
  - **Mutexes (`xSdMutex`, `xI2CMutex`, `xScheduleMutex`)** — Serializes access to shared hardware resources (SD card, RTC, and JSON schedule structures) across threads.

### Autonomous Attendance Logging (WiFi Probe Sniffing)
Integrated background attendance tracking into `vSnifferTask`:
- **Passive Sniffing** — Captures WiFi Probe Requests to register student/staff devices without requiring them to connect to the network.
- **Smart Windowing** — Promiscuous mode auto-toggles around scheduled attendance windows so the Access Point stays fully functional outside class times.
- **Deduplication & Storage** — De-duplicates MAC addresses and logs attendance directly to `/data/logs.csv` with real RTC timestamps.

### Upgraded I2S MP3 Audio Engine (`Audio.h`)
Replaced basic tone synthesis (`i2sPlayTone()`) with full MP3 stream decoding:
- Streams high-quality audio files from SD storage directly to the **MAX98357A Class D amplifier**.
- **Queue-Driven Audio Pipeline** — `vAudioTask` listens on `xAudioQueue` for dispatch requests:
  - Scheduled class chimes (`/audio/chime.mp3`)
  - Emergency alert broadcasts (`/audio/alert.mp3`)
  - Per-entry start/end chime paths

### Captive Portal & Network Upgrades
- **WPA2-Secured Access Point** — Broadcasts `FCU_Secure_Bell` under WPA2 encryption (`AP_PASSWORD`).
- **Captive Portal Engine** — Uses `DNSServer` to catch and redirect DNS requests from iOS, Android, and Windows devices directly to the system login interface.
- **Chunk-Safe JSON API** — Upgraded POST handlers for `/api/schedule` to support chunked transfer encoding up to 4 KB, preventing memory overflows during schedule uploads.

### Scheduling & Hardware Engine Enhancements
- **Day-of-Week Bitmask Matching** — Schedules support day-of-week bitmasks (Sunday–Saturday) across up to 20 schedule entries in `/data/schedule.json`.
- **Exactly-Once Firing Guard** — Implements per-entry state tracking (`lastFiredStart[]`, `lastFiredEnd[]`) to ensure scheduled bells fire exactly once per configured minute window.
- **Hardware Edge Detection** — Physical buttons use edge detection to prevent continuous alert re-triggering while held down.

## Event Logging Coverage

All system events, attendance entries, and security logs pass through `xLogQueue` to `vSnifferTask` or designated file-writing helper functions:

| Log Type | Format / Event Tag | Description |
|---|---|---|
| **Attendance** | `MAC, RSSI, Timestamp, Status` | WiFi probe capture written to `/data/logs.csv` |
| **Authentication** | `FAILED_LOGIN`, `LOCKOUT_ACTIVE` | Failed logins and brute-force lockouts |
| **System** | `SYSTEM_BOOT`, `SNIFFER_STATE_CHANGE` | Boot state and promiscuous mode toggles |
| **Schedule** | `SCHEDULE_TRIGGERED_AT_xx:xx` | Automated audio dispatch events |
| **Audio** | `AUDIO_QUEUE_DISPATCH: filename` | File playback requests handled by `vAudioTask` |

## Hardware Pin Configuration

| Component | Pin | Description |
|---|---|---|
| MAX98357A DIN | GPIO 25 | Audio data output |
| MAX98357A BCLK | GPIO 26 | Bit clock |
| MAX98357A LRC | GPIO 27 | Left/Right clock (Word select) |
| SD Card CS | GPIO 5 | SPI Chip Select for SD storage |
| RTC DS3231 | I2C (SDA / SCL) | Real-time clock (guarded by `xI2CMutex`) |

## Prerequisites

Verify the SD card contains the following structure before booting:

```text
/
├── audio/
│   ├── chime.mp3
│   └── alert.mp3
├── data/
│   ├── schedule.json
│   └── logs.csv
└── ui/
    ├── login.html
    └── dashboard.html

```

## Frontend to Backend Integration

The frontend communicates asynchronously with the backend over HTTP via `ESPAsyncWebServer` running within a dual-core FreeRTOS context.

### 1. Dual-Core Request & Audio Flow

When a user connects or an event is triggered, tasks across both cores coordinate execution:

```cpp
// Audio dispatch posted to FreeRTOS queue from web or system tasks
AudioRequest req;
strncpy(req.filename, "/audio/chime.mp3", sizeof(req.filename));
xQueueSend(xAudioQueue, &req, portMAX_DELAY);

```

**Flow:** Browser / Event Trigger → `vWebTask` / `vSystemEngineTask` (Core 1) → Pushes request to `xAudioQueue` → `vAudioTask` (Core 1, Priority 5) unblocks and streams MP3 from SD card to MAX98357A I2S amplifier via `Audio.h`.

### 2. Autonomous Attendance Pipeline

```cpp
// Core 0 WiFi promiscuous mode callback pushes captured MACs to logging queue
void IRAM_ATTR promiscuous_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    // Extract MAC, RSSI, and dispatch to xLogQueue safely
}

```

**Flow:** Core 0 (`vSnifferTask`) captures WiFi Probe Requests → de-duplicates MAC addresses → pushes log structure to `xLogQueue` → serialized write to `/data/logs.csv` using `xSdMutex`.

### 3. API Call Summary from Dashboard JavaScript

The dashboard (`ui/dashboard.html`) communicates with the ESP32 backend using non-blocking `fetch()` calls:

#### `loadTelemetry()` — Device Status & Sniffer State

```javascript
async function loadTelemetry() {
  const res = await fetch('/api/telemetry', { cache: 'no-store' });
  if (!res.ok) { window.location.href = '/'; return; }
  const data = await res.json();
  // Update UI: time, connection status, sniffer state, active window
}

```

#### `fetchSchedule()` — Load JSON Bell Schedule

```javascript
async function fetchSchedule() {
  const res = await fetch('/api/schedule', { cache: 'no-store' });
  const data = await res.json();
  // Render schedule table rows with time, days bitmask, and chime paths
}

```

#### `saveSchedule()` — Chunk-Safe Schedule POST

```javascript
async function saveSchedule(scheduleJson) {
  const res = await fetch('/api/schedule', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(scheduleJson)
  });
  if (res.ok) alert('Schedule updated successfully!');
}

```

#### `fetchAttendance()` — Retrieve Attendance Records

```javascript
async function fetchAttendance() {
  const res = await fetch('/api/attendance', { cache: 'no-store' });
  const csvText = await res.text();
  // Parse and display MAC attendance logs
}

```

#### `triggerManualChime()` — Manual Bell Control

```javascript
async function triggerManualChime() {
  const res = await fetch('/ring', { method: 'GET' });
  if (res.ok) console.log('Chime dispatch queued.');
}

```

### Summary Diagram

```text
┌──────────────────────┐         HTTP (Async)         ┌─────────────────────────────────────────┐
│   Browser / Client   │ ◄──────────────────────────► │           ESP32 (Core 0 & Core 1)       │
│                      │                              │                                         │
│ - Captive Portal UI  │   GET /                      │  Core 1: vWebTask (Priority 1)          │
│ - JS fetch() polling │   GET /api/telemetry         │  - DNSServer Captive Portal             │
│ - Schedule Editor    │   POST /api/schedule         │  - ESPAsyncWebServer                    │
│                      │   GET /api/attendance        │  - Cookie Session Auth                  │
└──────────────────────┘                              └───────────────────┬─────────────────────┘
                                                                          │
                                                                   xAudioQueue / xLogQueue
                                                                          │
                                                      ┌───────────────────▼─────────────────────┐
                                                      │           FreeRTOS Hardware Tasks       │
                                                      │                                         │
                                                      │  Core 1: vAudioTask (Priority 5)        │
                                                      │  - Reads MP3s from SD via xSdMutex     │
                                                      │  - Drives MAX98357A via Audio.h         │
                                                      │                                         │
                                                      │  Core 1: vSystemEngineTask (Priority 3) │
                                                      │  - RTC schedule evaluation              │
                                                      │                                         │
                                                      │  Core 0: vSnifferTask (Priority 2)      │
                                                      │  - WiFi Probe Request capture           │
                                                      │  - Writes /data/logs.csv via xSdMutex   │
                                                      └─────────────────────────────────────────┘

```

## API Endpoints

| Endpoint | Method | Auth Required | Description |
| --- | --- | --- | --- |
| `/` | GET | Cookie (Session) | Serves captive portal `login.html` or `dashboard.html` |
| `/api/telemetry` | GET | Cookie (Session) | Device time, WiFi mode, sniffer status, and connection state |
| `/api/schedule` | GET / POST | Cookie (Session) | Fetches or updates JSON schedule configuration (chunk-safe) |
| `/api/attendance` | GET | Cookie (Session) | Downloads parsed attendance logs from `/data/logs.csv` |
| `/api/attendance/status` | GET | Cookie (Session) | Returns current promiscuous mode / sniffer window state |
| `/api/config` | GET / POST | Cookie (Session) | Manages WiFi AP/Station credentials |
| `/handleLogin` | POST | None | Authenticates session credentials via client `fetch()` |
| `/ring` | GET | Cookie (Session) | Pushes manual chime request to `xAudioQueue` |
| `/logout` | GET | Cookie (Session) | Clears session cookie and invalidates session token |

```

```
