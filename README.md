# capstone_elem_smart-bell

## Overview

The **latest version (v1.6)** is currently the most stable release.

---

## Versions

### v1.3

* Runs independently without requiring additional files or directories.

---

### v1.5

* Requires external resources to function properly:

  * HTML files from `/ui`
  * Data files from `/data`
* If the `/data` directory is not available, the ESP32 will automatically generate a new one in the `/root` directory.

---

### v1.5fixed

* A **semi-working variant** with improvements over v1.5
* Includes an **updated UI**
* Features **improved data handling and system management**
* Still requires:

  * `/ui` for interface files
  * `/data` for storage (auto-generated if missing)

---

### 🚀 v1.6 (Latest — Most Stable)

* **Async WiFi/NTP State Machine**

  * Replaced blocking `connectAndSyncTime()` (which used `delay(500)` loops and froze the server for ~7.5 seconds)
  * Introduced a non-blocking `WiFiConnState` enum
  * ✅ Web server remains responsive during connection attempts

* **New `/api/config` Endpoint**

  * Returns:

    ```json
    {
      "ap_ssid": "...",
      "sta_ssid": "...",
      "sta_pass": "****"
    }
    ```
  * Allows the dashboard to automatically pre-populate network settings

* **Improved `/saveNetwork` Behavior**

  * Removed HTTP 303 redirect (which caused browser timeouts)
  * Now returns:

    ```json
    { "status": "saved", "message": "..." }
    ```
  * Starts WiFi connection asynchronously

* **Default SSID Fix (`data/config.txt`)**

  * Previous default `"Network Name"` caused failed connection attempts on boot
  * Now uses an empty string `""`
  * ✅ Device skips WiFi if no network is configured

* **Connection Feedback Enhancements**

  * Sync indicator turns **yellow** during connection attempts
  * `saveNetwork()` displays real-time status messages
  * Added `try/catch` for better error handling

* **Expanded Logging System**

  * `logSecurityEvent()` now logs **all events** (no longer limited to SuperAdmin)
  * New events include:

    * `SYSTEM_BOOT`
    * `WIFI_CONNECTION_STARTED`, `WIFI_CONNECTED`, `WIFI_TIMEOUT`
    * `NTP_SYNC_SUCCESS`, `NTP_TIMEOUT`
    * `FAILED_LOGIN`
    * `BRUTE_FORCE_LOCKOUT`
    * `LOCKOUT_ACTIVE_REJECTED`
    * `SESSION_TIMED_OUT`
    * `USER_LOGOUT`

* **Updated Logs Access Control**

  * `/backend/logs` is now accessible by both **Admin** and **SuperAdmin**
  * (Previously restricted to SuperAdmin only)

* **Login Page Improvements**

  * Added modal-based error popup in `ui/login.html`
  * Uses:

    * `?error=` query parameter
    * `showModal()` and `hideModal()` functions
  * Provides clearer user feedback on login issues

* **Telemetry Update**

  * `/api/telemetry` now includes:

    ```json
    {
      "ap_ssid": "..."
    }
    ```

---

## Prerequisites

Ensure the following directories and components are available:

* `/ui/dashboard`
* `/ui/login`
* `/data/config`
* `/data/schedule`

---
