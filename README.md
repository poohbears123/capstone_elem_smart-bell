# capstone_elem_smart-bell — v1.6

The **latest stable release** of the FCU Smart Bell system.

## What's New in v1.6

### Async WiFi/NTP State Machine
Replaced the old synchronous blocking `connectAndSyncTime()` (with `delay(500)` loops freezing the server for 7.5s) with a non-blocking `WiFiConnState` enum. The web server now stays responsive during connection attempts.

### New `/api/config` Endpoint
Returns `{"ap_ssid", "sta_ssid", "sta_pass" (masked)}` so the dashboard pre-populates network settings automatically — no need to retype everything.

### `/saveNetwork` Returns JSON
No longer uses HTTP 303 redirect. Returns `{"status":"saved","message":"..."}` and starts async WiFi, preventing browser timeouts.

### Default SSID Fix
`data/config.txt` station SSID changed from `"Network Name"` (bogus network causing boot connection failures) to **empty string**. The ESP now skips WiFi if no network is configured.

### Connection Progress Feedback
- Sync dot turns **yellow** during connection attempts
- `saveNetwork()` shows real-time status messages
- `try/catch` error handling added for network saves

### Expanded Logging Coverage
`logSecurityEvent()` now logs **ALL** events (removed SuperAdmin-only filter):

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

### `/backend/logs` Auth Relaxed
Both **Admin** and **SuperAdmin** can now view security logs (previously SuperAdmin only).

### Login Page Modal Error Popup
`ui/login.html` now features a styled modal error dialog with OK button, plus `?error=` query parameter handling via `showModal()`, `hideModal()`, and `getQueryParam()`.

### AP SSID in Telemetry
`/api/telemetry` JSON response now includes the `"ap_ssid"` field.

## Prerequisites

Ensure the following directories and files are available on the SD card:

- `/ui/dashboard.html`
- `/ui/login.html`
- `/data/config.txt`
- `/data/schedule.txt`

If `/data/` is missing, the ESP32 will auto-generate it in the `/root` directory.

## Frontend to Backend Integration

The frontend (HTML/JS files on the SD card) communicates with the backend (ESP32 firmware) entirely over HTTP. Here's how each mechanism works:

### 1. Serving the Pages

When you connect to the ESP32 (either via its AP `FCU_Secure_Bell` or its station IP), the firmware's `WebServer` handles requests:

```cpp
// In capstone1.6.ino — routes are registered in setupRoutes()
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
| **Subsequent requests** | Browser automatically sends `Cookie: ESPSESSIONID=<token>` with every request → `checkSessionRole()` reads the cookie, looks up the session, returns the role (`SuperAdmin`, `Admin`, `User`, or `Guest`) |
| **Logout** | `/logout` clears the cookie and invalidates the session |
| **Timeout** | Sessions expire after 10 minutes (`SESSION_TIMEOUT = 600000ms`) of inactivity |

**In the firmware:**
```cpp
String checkSessionRole() {
  if (!server.hasHeader("Cookie")) return "Guest";
  String cookieHeader = server.header("Cookie");
  // Extract ESPSESSIONID token...
  // Validate against in-memory sessions array...
  // If expired → log "SESSION_TIMED_OUT" → return "Guest"
  // Otherwise → update lastActivity → return role
}
```

### 3. API Calls from Dashboard JavaScript

The dashboard (`ui/dashboard.html`) uses `fetch()` to call backend API endpoints. The session cookie is automatically included by the browser.

#### `loadTelemetry()` — Polling device status
```javascript
// Called on page load, then every 5 seconds via setInterval()
async function loadTelemetry() {
  const res = await fetch('/api/telemetry', { cache: 'no-store' });
  if (!res.ok) { window.location.href = '/'; return; }  // Redirect if session expired
  const data = await res.json();
  // Update UI: time, status, SSID, role pill, sync dot color
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
  // Collect form values
  const res = await fetch('/saveNetwork', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'ap_name=...&station_ssid=...&station_pass=...'
  });
  const data = await res.json();  // Parse JSON response
  msg.textContent = data.message; // "Network config saved. Attempting connection..."
  // Sync dot turns yellow to show connection in progress
  document.getElementById('syncDot').style.background = '#f0a500';
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
// Add schedule
await fetch('/addSchedule', {
  method: 'POST',
  headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
  body: 'sched_time=' + encodeURIComponent('08:00')
});

// Edit schedule
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

#### `ringBell()` — Trigger manual bell
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

### 4. Login Form Submission

The login page (`ui/login.html`) uses a standard HTML form POST (not fetch):

```html
<form action="/handleLogin" method="POST" autocomplete="off">
  <input type="text" name="username" required />
  <input type="password" name="password" required />
  <button type="submit">Authenticate</button>
</form>
```

**Flow:** Browser submits form → ESP receives credentials → validates against MD5 hashes → if success: sets session cookie and redirects (303) to `/` → if fail: returns `401` with `fallbackLoginPage("Invalid username or password")` HTML showing error modal.

### 5. Error Handling & Redirects

The backend uses **HTTP status codes** to signal authentication state to the frontend:

| Status Code | Meaning | Frontend Reaction |
|---|---|---|
| **403** | `Guest` (not logged in) trying to access a protected endpoint | Dashboard redirects to `/` (login) |
| **423** | Account is locked out due to brute-force protection | ESP returns lockout page |
| **303** | Successful login/logout | Browser follows redirect to `/` |
| **401** | Login failed | ESP re-renders login page with error modal |

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
│  ui/dashboard.html   │   POST /handleLogin    │  SPIFSS + SD Card         │
│                      │   GET /api/telemetry   │  - reads HTML files       │
│  JavaScript (fetch)  │   GET /api/config      │  - reads/writes data/     │
│  - loadTelemetry()   │   POST /saveNetwork    │  - reads/writes logs      │
│  - loadConfig()      │   GET /api/schedules   │                           │
│  - saveNetwork()     │   POST /addSchedule    │  Session Manager          │
│  - refreshSchedules()│   POST /editSchedule   │  - in-memory sessions[]   │
│  - ringBell()        │   POST /deleteSchedule │  - ESPSESSIONID cookie    │
│  - refreshLogs()     │   GET /ring            │  - 10-minute timeout      │
│                      │   GET /backend/logs    │                           │
│  Cookie:             │   GET /logout          │  RTC + NTP                │
│  ESPSESSIONID=xxx    │                        │  - time tracking          │
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
| `/ring` | GET | Cookie (session) | Trigger manual bell (3s) |
| `/backend/logs` | GET | Admin/SuperAdmin | View security logs |
| `/backend/logs/upload` | POST | SuperAdmin | Upload/replace security logs |
| `/logout` | GET | Cookie (session) | End session |

