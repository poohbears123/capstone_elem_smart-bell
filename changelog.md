# Changelog

All notable changes to this project will be documented in this file.

---

## [1.7.0] - 2026-07-26

### Added
- Redesigned login page with FCU branding and modal-based error feedback
- Redesigned dashboard:
  - Sidebar navigation with 6 views
  - Telemetry cards and improved layout
  - Schedule modal with time-dial spinners
- Expanded event logging:
  - `SUCCESSFUL_LOGIN`
  - `ADDED_SCHEDULE`
  - `REMOVED_SCHEDULE`
  - `EDITED_SCHEDULE`
  - `UPDATED_NETWORK_CONFIG`
  - `TRIGGERED_MANUAL_BELL`
  - `SCHEDULE_TRIGGERED_AT`
- Hardware pin configuration documentation

### Changed
- Authentication flow migrated from HTML form POST to `fetch()`-based login
- Improved frontend/backend interaction using structured HTTP responses (401, 423, 303)
- General UI/UX improvements across login and dashboard

### Removed
- Legacy v1.6-specific changelog details replaced by updated system design

---

## [1.6.0] - 2026-07-21

### Added
- Full async WiFi/NTP state machine (`WiFiConnState`)
- `/api/config` endpoint for retrieving network configuration
- JSON response for `/saveNetwork` endpoint
- Connection progress feedback (UI indicators and status messages)
- Expanded logging system with additional events:
  - `SYSTEM_BOOT`
  - `WIFI_CONNECTION_STARTED`, `WIFI_CONNECTED`, `WIFI_TIMEOUT`
  - `NTP_SYNC_SUCCESS`, `NTP_TIMEOUT`
  - `FAILED_LOGIN`
  - `BRUTE_FORCE_LOCKOUT`
  - `LOCKOUT_ACTIVE_REJECTED`
  - `SESSION_TIMED_OUT`
  - `USER_LOGOUT`
- AP SSID included in `/api/telemetry`
- Login modal error handling using `?error=` query parameter

### Changed
- Replaced blocking `connectAndSyncTime()` with non-blocking implementation
- Logging system expanded beyond SuperAdmin-only visibility
- `/backend/logs` now accessible to Admin and SuperAdmin
- Improved error handling with `try/catch`

### Fixed
- Default SSID issue in `data/config.txt`
  - Removed `"Network Name"` fallback
  - Prevented failed WiFi connection attempts on boot

---

## [1.5.1] - 2026-07-12

### Added
- Updated UI components
- Improved system and data handling logic

### Changed
- Enhanced stability compared to v1.5
- Improved file and resource management

---

## [1.5.0] - 2026-07-09

### Added
- I2S audio support via **MAX98357A**
  - Implemented `setupI2S()`, `i2sPlayTone()`, `triggerPhysicalBell()`
  - Configured GPIO 25 (BCLK), 26 (LRC), 27 (DIN)
- SD card support for audio playback
  - Added `streamSDFile()` helper
  - Configured CS pin on GPIO 5
- External resource dependency system:
  - `/ui` for HTML interface
  - `/data` for persistent storage

### Changed
- System now depends on external directory structure

### Notes
- Automatically creates `/data` directory if missing (in `/root`)

---

## [1.3.0] - 2026-05-12

### Added
- DS3231 RTC integration
  - Added required dependencies
- Fully standalone system with no external dependencies

### Notes
- Simplest and most portable version
- Limited feature set compared to later releases

---

## Versioning Notes

This project follows **Semantic Versioning (SemVer)**:
- **MAJOR** version for incompatible changes
- **MINOR** version for new features (backward-compatible)
- **PATCH** version for bug fixes and minor improvements

### Version Mapping
- v1.3 → `1.3.0`
- v1.5 → `1.5.0`
- v1.5fixed → `1.5.1`
- v1.6 → `1.6.0`
- v1.7 → `1.7.0`
