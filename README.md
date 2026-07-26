# capstone_elem_smart-bell — v1.7

The **latest stable release** of the FCU Smart Bell system.

## What's New in v1.7

### I2S Audio via MAX98357A Amplifier
Replaced the simple GPIO-based bell mechanism with a full I2S audio driver for the **MAX98357A Class D amplifier**. The system now generates audio tones through an external speaker using:
- **BCLK** (GPIO 26), **LRC** (GPIO 27), **DIN** (GPIO 25)
- 16-bit, 16kHz mono audio playback
- `i2sPlayTone()` — plays a configurable frequency tone for a given duration
- `triggerPhysicalBell()` — dispatches a bell ring with 1-second or multi-pulse pattern (500ms on / 100ms off)
- `setupI2S()` — initializes the I2S driver with DMA buffers on boot

### SD Card Support for UI Files
The web UI (`login.html`, `dashboard.html`) is now served from an **SD card** (CS pin GPIO 5) instead of SPIFFS, enabling larger, richer HTML pages. The `streamSDFile()` helper automatically falls back to SPIFFS if the SD card is unavailable.

### Completely Redesigned Login Page (`ui/login.html`)
A polished, professional login interface featuring:
- **FCU logo** image displayed prominently
- **Building background** overlay photo
- **Portal title bar** — "FCU ELEMENTARY SCHOOL BELL SYSTEM" with orange accent
- **Styled input fields** — larger, cleaner design
- **reCAPTCHA placeholder** text
- **SIGN IN button** — dark blue with hover effect
- **Footer link** — "Don't have an account? Sign Here"
- **Fetch-based login** — the form now uses JavaScript `fetch()` to POST credentials, then handles 401 (invalid), 423 (locked), and 303 (success) responses client-side — no page reload on error
- **Modal error popup** — styled error dialog with OK button for invalid credentials

### Completely Redesigned Dashboard (`ui/dashboard.html`)
A modern, sidebar-based dashboard with multiple views:

| View | Description |
|---|---|
| **Dashboard** | Telemetry cards (Device Time, Next Bell, Network Status, Connected SSID), System Controls (Manual Override, Emergency Detection, Ring Bell 3s), bell schedule table with Edit/Delete |
| **Schedule** | Read-only bell timetable with Next Bell and Scheduled Events counters |
| **User Management** | Static staff account table (Name, Role, Status, Last Login, Actions) |
| **Activity Logs** | Security logs textarea with Refresh and Upload (file picker) buttons |
| **Network Settings** | WiFi config form (AP SSID, Station SSID/Password) with Save & Sync Time button |
| **Manual** | Quick-start guide, FAQ cards, API reference list |

### Schedule Modal with Time-Dial Spinners
Added an interactive **time-picker modal** for schedule CRUD operations:
- **Hour spinner** (1–12) with smooth scrolling
- **Minute spinner** (00–59) with smooth scrolling
- **AM/PM toggle buttons** with active state highlighting
- Live preview of the selected 24-hour time
- Works for both Add and Edit operations

### Improved Login Error Handling
- 401 responses show a **styled modal dialog** on the login page (no more fallback HTML page)
- 423 (lockout) responses show a "temporarily locked" message
- Network errors show a user-friendly message
- The login page stays intact on failure — no page redirect

### Expanded System Controls
- **Manual Override button** — triggers a manual override event
- **Emergency Detection button** — activates emergency alert mode
- **Ring Bell (3s) button** — triggers a physical bell ring via I2S audio

### Other Enhancements
- **Sidebar navigation** with role display (`SuperAdmin`, `Admin`, `User`)
- **Sign Out button** in sidebar footer
- **Logs view** automatically fetches on navigation to Activity Logs
- **15-second auto-refresh** interval for logs
- **5-second telemetry polling** for live dashboard updates
- **Upload log file** functionality with file picker in Activity Logs
- **README updated** to document all v1.7 features

## Event Logging Coverage

`logSecurityEvent()` logs ALL system events:

| Event | Description |
|---|---|
| `SYSTEM_BOOT` | System startup |
| `WIFI_CONNECTION_STARTED_SSID_xxx` | WiFi connection initiated |
| `WIFI_CONNECTED_SSID_xxx` | WiFi connection successful |
| `WIFI_CONNECTION_TIMEOUT_SSID_xxx` | WiFi connection timed out |
| `NTP_SYNC_SUCCESS_SSID_xxx` | NTP time sync succeeded |
| `NTP_SYNC_TIMEOUT_SSID_xxx` | NTP time sync failed |
| `FAILED_LOGIN: USERNAME_xxx` | Failed login attempt |
| `BRUTE_FORCE_LOCKOUT_TRIGGERED` | Lockout activated after max attempts |
| `LOCKOUT_ACTIVE_REJECTED_LOGIN_ATTEMPT` | Rejected attempt during lockout |
| `SESSION_TIMED_OUT` | User session expired |
| `USER_LOGOUT` | User logged out |
| `SUCCESSFUL_LOGIN` | Successful authentication |
| `ADDED_SCHEDULE_xx:xx` | Bell schedule added |
| `REMOVED_SCHEDULE_xx:xx` | Bell schedule deleted |
| `EDITED_SCHEDULE_FROM_x_TO_y` | Bell schedule edited |
| `UPDATED_NETWORK_CONFIG` | Network settings saved |
| `TRIGGERED_MANUAL_BELL` | Manual bell triggered |
| `SCHEDULE_TRIGGERED_AT_xx:xx` | Automated schedule bell fired |

## Hardware Pin Configuration

| Component | Pin | Description |
|---|---|---|
| MAX98357A DIN | GPIO 25 | Audio data output |
| MAX98357A BCLK | GPIO 26 | Bit clock |
| MAX98357A LRC | GPIO 27 | Left/Right clock (Word select) |
| SD Card CS | GPIO 5 | Chip select for SD card |
| RTC DS3231 | I2C (SDA/SCL) | Real-time clock |

## Prerequisites

Ensure the following directories and files are available:

- `/ui/dashboard.html` (SD card or SPIFFS)
- `/ui/login.html` (SD card or SPIFFS)
- `/data/config.txt`
- `/data/schedule.txt`

If `/data/` is missing, the ESP32 will auto-generate it in SPIFFS `/root` directory.

## Frontend to Backend Integration

The frontend (HTML/JS files on the SD card) communicates with the backend (ESP32 firmware) entirely over HTTP.

### 1. Serving the Pages

When you connect to the ESP32 (either via its AP `FCU_Secure_Bell` or its station IP), the firmware's `WebServer` handles requests:

```cpp
server.on("/", []() {
  String userRole = checkSessionRole();
  if (userRole == "Guest") {
    // Serve login page from SD card
    if (!streamSDFile("/ui/login.html", "text/html"))
      server.send(200, "text/html", fallbackLoginPage());
  } else {
    // Serve dashboard from SD card
    if (!streamSDFile("/ui/dashboard.html", "text/html"))
      server.send(404, "text/plain", "Error: /ui/dashboard.html missing from SD Card.");
  }
});
```

**Flow:** Browser → HTTP GET `/` → ESP32 reads `login.html` or `dashboard.html` from SD card → sends HTML back to browser.

### 2. Session Authentication (Cookie-based)

The backend tracks logged-in users with **HTTP-only cookies**:

| Step | What Happens |
|---|---|
| **Login** | Browser POSTs username+password to `/handleLogin` → ESP validates via MD5 hash → creates a server-side session → sets `Set-Cookie: ESPSESSIONID=<random_token>; Path=/; HttpOnly` response header |
| **Fetch-based login (v1.7)** | The login page intercepts form submit with `fetch()`, handles 401/423/303 responses client-side, shows modal on error, redirects on success |
| **Subsequent requests** | Browser automatically sends `Cookie: ESPSESSIONID=<token>` with every request → `checkSessionRole()` reads the cookie, looks up the session, returns the role (`SuperAdmin`, `Admin`, `User`, or `Guest`) |
| **Logout** | `/logout` clears the cookie and invalidates the session |
| **Timeout** | Sessions expire after 10 minutes (`SESSION_TIMEOUT = 600000ms`) of inactivity |

### 3. API Calls from Dashboard JavaScript

The dashboard (`ui/dashboard.html`) uses `fetch()` to call backend API endpoints.

#### `loadTelemetry()` — Polling device status
```javascript
// Called on page load, then every 5 seconds via setInterval()
async function loadTelemetry() {
  const res = await fetch('/api/telemetry', { cache: 'no-store' });
  if (!res.ok) { window.location.href = '/'; return; }
  const data = await res.json();
  // Update UI: time, status, SSID, role pill
}
```

#### `loadConfig()` — Pre-populate network fields
```javascript
async function loadConfig() {
  const res = await fetch('/api/config', { cache: 'no-store' });
  const data = await res.json();
  document.getElementById('ap_name').value = data.ap_ssid || '';
  document.getElementById('station_ssid').value = data.sta_ssid || '';
  document.getElementById('station_pass').value = data.sta_pass || '';
}
```

#### `saveNetwork()` — Save WiFi settings
```javascript
async function saveNetwork() {
  const res = await fetch('/saveNetwork', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'ap_name=...&station_ssid=...&station_pass=...'
  });
  const data = await res.json();
  msg.textContent = data.message; // "Network config saved. Attempting connection..."
}
```

#### `refreshSchedules()` — Load bell schedule
```javascript
async function refreshSchedules() {
  const res = await fetch('/api/schedules', { cache: 'no-store' });
  const list = await res.json();
  // Render table rows with Edit/Delete buttons
}
```

#### Schedule CRUD operations
```javascript
// Add schedule (via modal)
await fetch('/addSchedule', {
  method: 'POST',
  headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
  body: 'sched_time=' + encodeURIComponent('08:00')
});

// Edit schedule (via modal)
await fetch('/editSchedule', {
  method: 'POST',
  body: 'index=0&sched_time=' + encodeURIComponent('09:00')
});

// Delete schedule
await fetch('/deleteSchedule', {
  method: 'POST',
  body: 'index=' + encodeURIComponent(index)
});
```

#### `ringBell()` — Trigger manual bell via I2S audio
```javascript
async function ringBell() {
  const res = await fetch('/ring', { method: 'GET' });
  if (!res.ok) { msg.textContent = 'Failed (not authorized?)'; return; }
  msg.textContent = 'Bell Dispatched.';
}
```

#### `refreshLogs()` — Fetch security logs
```javascript
async function refreshLogs() {
  const res = await fetch('/backend/logs', { cache: 'no-store' });
  const text = await res.text();
  document.getElementById('logs').value = text || '';
}
```

### 4. Login Form Submission (v1.7 Fetch-based)

The login page (`ui/login.html`) now uses JavaScript `fetch()` instead of HTML form POST:

```javascript
document.getElementById('login-form').addEventListener('submit', async function(e) {
  e.preventDefault();
  const res = await fetch('/handleLogin', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'username=' + encodeURIComponent(username) + '&password=' + encodeURIComponent(password)
  });
  // 401 → showModal('Invalid username or password')
  // 423 → showModal('Account temporarily locked...')
  // 303 → window.location.href = location header
});
```

### 5. Error Handling & Redirects

| Status Code | Meaning | Frontend Reaction |
|---|---|---|
| **403** | `Guest` (not logged in) trying to access a protected endpoint | Dashboard redirects to `/` (login) |
| **423** | Account is locked out due to brute-force protection | ESP returns lockout page |
| **303** | Successful login/logout | Browser follows redirect to `/` |
| **401** | Login failed | ESP returns 401, login page shows fetch()-handled error modal (no page reload) |

### 6. The Data Files

| File | Read by | Written by | Purpose |
|---|---|---|---|
| `data/config.txt` | `loadConfiguration()` on boot | `/saveNetwork` endpoint | Persists AP SSID, station SSID, station password |
| `data/schedule.txt` | `loadSchedules()` on boot | `/addSchedule`, `/editSchedule`, `/deleteSchedule` endpoints | Persists bell schedule times |
| `data/security_log.txt` | `/backend/logs` endpoint | `logSecurityEvent()` | Persists all security/system events |

### Summary Diagram

```
┌──────────────────────┐         HTTP          ┌───────────────────────────┐
│   Browser (Client)   │ ◄──────────────────►   │    ESP32 (Server/Self)    │
│                      │                        │                           │
│  ui/login.html       │   GET /                │  WebServer (port 80)      │
│  ui/dashboard.html   │   POST /handleLogin    │  SD Card + SPIFFS         │
│                      │   GET /api/telemetry   │  - reads HTML files       │
│  JavaScript (fetch)  │   GET /api/config      │  - reads/writes data/     │
│  - loadTelemetry()   │   POST /saveNetwork    │  - reads/writes logs      │
│  - loadConfig()      │   GET /api/schedules   │                           │
│  - saveNetwork()     │   POST /addSchedule    │  I2S Audio (MAX98357A)    │
│  - refreshSchedules()│   POST /editSchedule   │  - triggerPhysicalBell()  │
│  - ringBell()        │   POST /deleteSchedule │  - i2sPlayTone()          │
│  - refreshLogs()     │   GET /ring            │                           │
│                      │   GET /backend/logs    │  Session Manager          │
│  Cookie:             │   GET /backend/logs/   │  - in-memory sessions[]   │
│  ESPSESSIONID=xxx    │        upload          │  - ESPSESSIONID cookie    │
│                      │   GET /logout          │  - 10-minute timeout      │
│                      │                        │                           │
│  fetch()-based       │                        │  RTC + NTP                │
│  login (no reload)   │                        │  - time tracking          │
│                      │                        │                           │
└──────────────────────┘                        └───────────────────────────┘
```

## API Endpoints

| Endpoint | Method | Auth Required | Description |
|---|---|---|---|
| `/` | GET | Cookie (session) | Serves login or dashboard |
| `/api/telemetry` | GET | Cookie (session) | Device time, status, SSID, AP SSID, role |
| `/api/config` | GET | Cookie (session) | Network configuration |
| `/api/schedules` | GET | Cookie (session) | Bell schedule list |
| `/handleLogin` | POST | None | Username/password authentication |
| `/saveNetwork` | POST | Cookie (session) | Save WiFi config and connect |
| `/addSchedule` | POST | Cookie (session) | Add a bell schedule time |
| `/editSchedule` | POST | Cookie (session) | Edit an existing schedule |
| `/deleteSchedule` | POST | Cookie (session) | Delete a schedule |
| `/ring` | GET | Cookie (session) | Trigger manual bell (3s) via I2S audio |
| `/backend/logs` | GET | SuperAdmin | View security logs |
| `/backend/logs/upload` | POST | SuperAdmin | Upload/replace security logs |
| `/logout` | GET | Cookie (session) | End session |

