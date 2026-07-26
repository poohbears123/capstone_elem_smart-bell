# capstone_elem_smart-bell

## 📌 Overview

**capstone_elem_smart-bell** is an ESP32-based smart bell system featuring:

- 📶 WiFi connectivity with async handling  
- 🔊 Digital audio output (I2S amplifier)  
- 💾 SD card audio playback  
- 🖥️ Web-based dashboard and login system  
- 🕒 RTC-based scheduling (DS3231)  
- 🔐 Authentication and event logging  

The system evolves from a **standalone embedded device (v1.3)** into a **fully interactive IoT system (v1.7)**.

---

## ✨ Features

### 🔐 Authentication System
- Login with session handling  
- Modal-based error feedback  
- Fetch-based authentication (`401`, `423`, `303`)  

### 📊 Dashboard
- Sidebar navigation (6 views)  
- Telemetry cards  
- Interactive scheduling interface  
- Time-dial input controls  

### 🔊 Audio System
- I2S audio via **MAX98357A**
- Functions:
  - `setupI2S()`
  - `i2sPlayTone()`
  - `triggerPhysicalBell()`

### 💾 Storage
- SD card support for audio playback  
- `/data` directory for configuration and schedules  

### 📡 Networking
- Async WiFi connection handling  
- `/api/config` and `/api/telemetry` endpoints  
- Real-time status feedback  

### 🕒 Scheduling
- RTC (DS3231) integration  
- Scheduled bell triggering  
- Manual trigger support  

### 📜 Logging System
Tracks:
- Login attempts and lockouts  
- System events  
- Schedule actions  
- Network updates  

---
