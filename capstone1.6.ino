#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <FS.h>
#include <SD.h>
#include <Wire.h>
#include <RTClib.h>
#include <mbedtls/md5.h>
#include <esp_wifi.h>
#include "time.h"
#include <driver/i2s.h>

const int MAX98357A_DIN_PIN = 25;
const int MAX98357A_BCLK_PIN = 26;
const int MAX98357A_LRC_PIN = 27;

const i2s_port_t I2S_PORT = I2S_NUM_0;
const int I2S_SAMPLE_RATE = 16000;
const int I2S_BITS_PER_SAMPLE = 16;
const int I2S_BUFFER_SIZE = 512;

const int SD_CS_PIN = 5;

const char* SUPERADMIN_USER = "superadmin";
const char* SUPERADMIN_HASH = "9bf18948eb1ce2c87fb32fc26d46779d";

String admin_user = "admin";
String admin_hash = "482c811da5d5b4bc6d497ffa98491e38";

String user_user = "user";
String user_hash = "482c811da5d5b4bc6d497ffa98491e38";

int failedAttempts = 0;
unsigned long lockoutTimeStart = 0;
const unsigned long LOCKOUT_DURATION = 300000;
const int MAX_ATTEMPTS = 4;

String ap_ssid = "FCU_Secure_Bell";
String sta_ssid = "";
String sta_pass = "";
String syncStatus = "Not Connected";

enum WiFiConnState {
  WIFI_IDLE,
  WIFI_CONNECTING,
  WIFI_CONNECTED,
  WIFI_SYNCING_NTP,
  WIFI_SYNCED,
  WIFI_FAILED
};
WiFiConnState wifiConnState = WIFI_IDLE;
unsigned long wifiConnStartTime = 0;
unsigned long wifiNtpSyncStartTime = 0;
const unsigned long WIFI_CONNECT_TIMEOUT = 10000;
const unsigned long WIFI_NTP_TIMEOUT = 5000;
String wifiTargetSSID = "";
String wifiTargetPass = "";

String scheduleList[20];
int scheduleCount = 0;

const int MAX_SESSIONS = 10;
const unsigned long SESSION_TIMEOUT = 600000;

struct UserSession {
  String token;
  String role;
  unsigned long lastActivity;
};
UserSession sessions[MAX_SESSIONS];

const byte DNS_PORT = 53;
IPAddress apIP(172, 217, 28, 1);
DNSServer dnsServer;
WebServer server(80);
RTC_DS3231 rtc;

const long gmtOffset_sec = 28800;
const int daylightOffset_sec = 0;

void logSecurityEvent(String user, String action);

String getNow() {
  struct tm timeinfo;
  char buf[60];
  if (WiFi.status() == WL_CONNECTED && getLocalTime(&timeinfo)) {
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S (NTP)", &timeinfo);
    return String(buf);
  }
  if (rtc.begin()) {
    DateTime now = rtc.now();
    sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d (RTC)", now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
    return String(buf);
  }
  unsigned long totalSeconds = millis() / 1000;
  unsigned long seconds = totalSeconds % 60;
  unsigned long minutes = (totalSeconds / 60) % 60;
  unsigned long hours = (totalSeconds / 3600) % 24;
  sprintf(buf, "2026-01-01 %02lu:%02lu:%02lu (Internal)", hours, minutes, seconds);
  return String(buf);
}

String getHHMM() {
  struct tm timeinfo;
  char buf[10];
  if (WiFi.status() == WL_CONNECTED && getLocalTime(&timeinfo)) {
    strftime(buf, sizeof(buf), "%H:%M", &timeinfo);
    return String(buf);
  }
  if (rtc.begin()) {
    DateTime now = rtc.now();
    sprintf(buf, "%02d:%02d", now.hour(), now.minute());
    return String(buf);
  }
  return "";
}

String generateSessionToken() {
  String chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  String token = "";
  for (int i = 0; i < 16; i++) token += chars[random(0, chars.length())];
  return token;
}

String createSession(String role) {
  String newToken = generateSessionToken();
  unsigned long currentTime = millis();
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].token != "" && (currentTime - sessions[i].lastActivity > SESSION_TIMEOUT)) {
      sessions[i].token = "";
      sessions[i].role = "";
    }
  }
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].token == "") {
      sessions[i].token = newToken;
      sessions[i].role = role;
      sessions[i].lastActivity = currentTime;
      return newToken;
    }
  }
  sessions[0].token = newToken;
  sessions[0].role = role;
  sessions[0].lastActivity = currentTime;
  return newToken;
}

String checkSessionRole() {
  if (!server.hasHeader("Cookie")) return "Guest";
  String cookieHeader = server.header("Cookie");
  int index = cookieHeader.indexOf("ESPSESSIONID=");
  if (index == -1) return "Guest";
  String token = cookieHeader.substring(index + 13);
  int endSpace = token.indexOf(';');
  if (endSpace != -1) token = token.substring(0, endSpace);
  token.trim();
  unsigned long currentTime = millis();
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].token == token && token != "") {
      if (currentTime - sessions[i].lastActivity > SESSION_TIMEOUT) {
        sessions[i].token = "";
        sessions[i].role = "";
        logSecurityEvent("FIREWALL", "SESSION_TIMED_OUT");
        return "Guest";
      }
      sessions[i].lastActivity = currentTime;
      return sessions[i].role;
    }
  }
  return "Guest";
}

void invalidateSession() {
  if (!server.hasHeader("Cookie")) return;
  String cookieHeader = server.header("Cookie");
  int index = cookieHeader.indexOf("ESPSESSIONID=");
  if (index == -1) return;
  String token = cookieHeader.substring(index + 13);
  int endSpace = token.indexOf(';');
  if (endSpace != -1) token = token.substring(0, endSpace);
  token.trim();
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].token == token) {
      sessions[i].token = "";
      sessions[i].role = "";
      return;
    }
  }
}

void loadConfiguration() {
  if (SPIFFS.exists("/data/config.txt")) {
    File configFile = SPIFFS.open("/data/config.txt", FILE_READ);
    if (configFile) {
      ap_ssid = configFile.readStringUntil('\n');
      sta_ssid = configFile.readStringUntil('\n');
      sta_pass = configFile.readStringUntil('\n');
      ap_ssid.trim();
      sta_ssid.trim();
      sta_pass.trim();
      configFile.close();
    }
  }
}

void saveConfiguration(String newAP, String newStationSSID, String newStationPass) {
  File configFile = SPIFFS.open("/data/config.txt", FILE_WRITE);
  if (configFile) {
    configFile.println(newAP);
    configFile.println(newStationSSID);
    configFile.println(newStationPass);
    configFile.close();
  }
}

void loadSchedules() {
  scheduleCount = 0;
  if (SPIFFS.exists("/data/schedule.txt")) {
    File schedFile = SPIFFS.open("/data/schedule.txt", FILE_READ);
    while (schedFile.available() && scheduleCount < 20) {
      String line = schedFile.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) scheduleList[scheduleCount++] = line;
    }
    schedFile.close();
  }
}

void saveSchedules() {
  File schedFile = SPIFFS.open("/data/schedule.txt", FILE_WRITE);
  for (int i = 0; i < scheduleCount; i++) schedFile.println(scheduleList[i]);
  schedFile.close();
}

void logSecurityEvent(String user, String action) {
  File logFile = SPIFFS.open("/data/security_log.txt", FILE_APPEND);
  if (logFile) {
    String logEntry = "[" + getNow() + "] Identity: " + user + " | Action: " + action;
    logFile.println(logEntry);
    logFile.close();
  }
}

String readSecurityLogs() {
  if (!SPIFFS.exists("/data/security_log.txt")) return "No system logs generated yet.";
  File logFile = SPIFFS.open("/data/security_log.txt", FILE_READ);
  if (!logFile) return "Error reading security log module.";
  String output = "";
  while (logFile.available()) {
    String line = logFile.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) output = line + "\n" + output;
  }
  logFile.close();
  return output;
}

String computeMD5(String input) {
  unsigned char digest[16];
  mbedtls_md5_context ctx;
  mbedtls_md5_init(&ctx);
  mbedtls_md5_starts(&ctx);
  mbedtls_md5_update(&ctx, (const unsigned char*)input.c_str(), input.length());
  mbedtls_md5_finish(&ctx, digest);
  mbedtls_md5_free(&ctx);
  char buf[33];
  for (int i = 0; i < 16; i++) sprintf(&buf[i * 2], "%02x", digest[i]);
  return String(buf);
}

void startWiFiConnection(String ssid, String pass) {
  if (ssid == "" || ssid == "Network Name") {
    syncStatus = "No valid network configured.";
    wifiConnState = WIFI_FAILED;
    return;
  }
  WiFi.disconnect(true);
  delay(100);
  wifiTargetSSID = ssid;
  wifiTargetPass = pass;
  wifiConnState = WIFI_CONNECTING;
  wifiConnStartTime = millis();
  syncStatus = "Connecting to " + ssid + "...";
  logSecurityEvent("SYSTEM", "WIFI_CONNECTION_STARTED_SSID_" + ssid);
  WiFi.begin(ssid.c_str(), pass.c_str());
}

void handleWiFiConnection() {
  unsigned long now = millis();
  switch (wifiConnState) {
    case WIFI_IDLE:
    case WIFI_SYNCED:
      break;
    case WIFI_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        syncStatus = "Connected to " + wifiTargetSSID + "! Syncing time...";
        wifiConnState = WIFI_CONNECTED;
        wifiNtpSyncStartTime = now;
        configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org");
        logSecurityEvent("SYSTEM", "WIFI_CONNECTED_SSID_" + wifiTargetSSID);
      } else if (now - wifiConnStartTime > WIFI_CONNECT_TIMEOUT) {
        syncStatus = "Connection timed out to " + wifiTargetSSID + ".";
        wifiConnState = WIFI_FAILED;
        WiFi.disconnect();
        logSecurityEvent("SYSTEM", "WIFI_CONNECTION_TIMEOUT_SSID_" + wifiTargetSSID);
      }
      break;
    case WIFI_CONNECTED:
    case WIFI_SYNCING_NTP: {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
          if (rtc.begin()) {
            rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
          }
          syncStatus = "Synced successfully via " + wifiTargetSSID;
          wifiConnState = WIFI_SYNCED;
          logSecurityEvent("SYSTEM", "NTP_SYNC_SUCCESS_SSID_" + wifiTargetSSID);
        } else if (now - wifiNtpSyncStartTime > WIFI_NTP_TIMEOUT) {
          syncStatus = "Connected to " + wifiTargetSSID + " (NTP unavailable)";
          wifiConnState = WIFI_SYNCED;
          logSecurityEvent("SYSTEM", "NTP_SYNC_TIMEOUT_SSID_" + wifiTargetSSID);
        }
      }
      break;
    case WIFI_FAILED:
      break;
  }
}

void i2sPlayTone(int frequency, int durationMs) {
  int16_t sample_high = 16000;
  int16_t sample_low  = -16000;
  int samplesPerCycle = I2S_SAMPLE_RATE / frequency;
  int halfCycleSamples = samplesPerCycle / 2;
  if (halfCycleSamples < 1) halfCycleSamples = 1;
  int totalSamples = (I2S_SAMPLE_RATE * durationMs) / 1000;
  int samplesWritten = 0;
  const int CHUNK = 256;
  int16_t buf[CHUNK];
  while (samplesWritten < totalSamples) {
    int remaining = totalSamples - samplesWritten;
    int chunkSize = (remaining < CHUNK) ? remaining : CHUNK;
    for (int i = 0; i < chunkSize; i++) {
      int posInCycle = (samplesWritten + i) % samplesPerCycle;
      buf[i] = (posInCycle < halfCycleSamples) ? sample_high : sample_low;
    }
    size_t bytesWritten = 0;
    i2s_write(I2S_PORT, buf, chunkSize * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
    samplesWritten += bytesWritten / sizeof(int16_t);
  }
}

void triggerPhysicalBell(int durationSeconds) {
  if (durationSeconds <= 1) {
    i2sPlayTone(1000, durationSeconds * 1000);
  } else {
    int remainingMs = durationSeconds * 1000;
    while (remainingMs > 0) {
      int onMs = (remainingMs > 500) ? 500 : remainingMs;
      i2sPlayTone(1000, onMs);
      remainingMs -= onMs;
      if (remainingMs > 100) {
        i2sPlayTone(1, 100);
        remainingMs -= 100;
      }
    }
  }
}

bool streamSDFile(String path, String contentType) {
  if (SD.exists(path)) {
    File file = SD.open(path, FILE_READ);
    server.streamFile(file, contentType);
    file.close();
    return true;
  }
  return false;
}

String fallbackLoginPage(String errorMsg = "") {
  String html = "<!doctype html><html><head>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>"
    "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;text-align:center;background:#f4f4f4;padding-top:50px;margin:0;}"
    ".card{background:#fff;padding:28px;border-radius:12px;display:inline-block;box-shadow:0 4px 6px rgba(0,0,0,0.1);border-top:8px solid #800000;min-width:min(420px,92vw);}"
    "input[type=text],input[type=password]{width:92%;padding:10px;margin:10px 0;border:1px solid #ccc;border-radius:8px;font-size:14px;}"
    "button{background:#800000;color:#fff;padding:11px 18px;border:none;border-radius:10px;cursor:pointer;width:95%;font-size:14px;font-weight:800;}"
    ".hint{color:#666;font-size:12px;margin-top:14px;line-height:1.3;}"
    ".error{color:#b00020;font-weight:800;margin:8px 0 0;min-height:18px;}"
    ".modal{position:fixed;left:0;top:0;width:100%;height:100%;background:rgba(0,0,0,0.35);display:flex;align-items:center;justify-content:center;padding:18px;z-index:9999;}"
    ".modal .box{background:#fff;border-radius:14px;box-shadow:0 14px 40px rgba(0,0,0,0.25);width:min(460px,92vw);padding:18px 18px 14px;border-top:8px solid #b00020;}"
    ".modal h3{margin:0 0 6px;font-size:16px;}"
    ".modal p{margin:0;color:#333;font-size:14px;}"
    ".modal .actions{margin-top:14px;text-align:right;}"
    ".modal .ok{background:#b00020;color:#fff;border:0;border-radius:10px;padding:10px 14px;font-weight:900;cursor:pointer;}"
    "</style></head><body>"
    "<div class='card'><h2 style='margin:0 0 6px;'>FCU Smart Bell Login</h2>";
  bool showModal = (errorMsg != "");
  if (showModal) {
    html += "<div class='error'>" + errorMsg + "</div>";
  }
  html += "<form action='/handleLogin' method='POST' autocomplete='off'>"
    "<input type='text' name='username' placeholder='Username' required />"
    "<input type='password' name='password' placeholder='Password' required />"
    "<button type='submit'>Authenticate</button>"
    "</form>"
    "<div class='hint'>After login, you'll be redirected to the dashboard.</div>"
    "</div>";
  if (showModal) {
    html += "<div class='modal' role='dialog' aria-modal='true' tabindex='-1'>"
      "<div class='box'>"
      "<h3>Login failed</h3>"
      "<p>" + errorMsg + "</p>"
      "<div class='actions'><button class='ok' onclick='document.querySelector(\".modal\").style.display=\"none\";'>OK</button></div>"
      "</div>"
      "</div>"
      "<script>setTimeout(()=>{document.querySelector('.modal')?.focus?.();},0);</script>";
  }
  html += "</body></html>";
  return html;
}

void setupRoutes() {
  const char* headerkeys[] = {"Cookie"};
  size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
  server.collectHeaders(headerkeys, headerkeyssize);

  server.on("/", []() {
    String userRole = checkSessionRole();
    if (userRole == "Guest") {
      if (!streamSDFile("/ui/login.html", "text/html")) server.send(200, "text/html", fallbackLoginPage());
    } else {
      if (!streamSDFile("/ui/dashboard.html", "text/html")) server.send(404, "text/plain", "Error: /ui/dashboard.html missing from SD Card.");
    }
  });

  server.on("/api/telemetry", []() {
    String role = checkSessionRole();
    if (role == "Guest") { server.send(403, "text/plain", "Forbidden"); return; }
    String json = "{";
    json += "\"time\":\"" + getNow() + "\",";
    json += "\"status\":\"" + syncStatus + "\",";
    json += "\"role\":\"" + role + "\",";
    json += "\"ssid\":\"" + sta_ssid + "\",";
    json += "\"ap_ssid\":\"" + ap_ssid + "\"";
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/api/config", []() {
    String role = checkSessionRole();
    if (role == "Guest") { server.send(403, "text/plain", "Forbidden"); return; }
    String json = "{";
    json += "\"ap_ssid\":\"" + ap_ssid + "\",";
    json += "\"sta_ssid\":\"" + sta_ssid + "\",";
    json += "\"sta_pass\":\"";
    if (sta_pass.length() > 0) { json += sta_pass.substring(0, 2) + "***"; }
    json += "\"";
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/api/schedules", []() {
    String role = checkSessionRole();
    if (role == "Guest") { server.send(403, "text/plain", "Forbidden"); return; }
    String json = "[";
    for (int i = 0; i < scheduleCount; i++) {
      json += "\"" + scheduleList[i] + "\"";
      if (i < scheduleCount - 1) json += ",";
    }
    json += "]";
    server.send(200, "application/json", json);
  });

  server.on("/backend/logs", []() {
    String role = checkSessionRole();
    if (role != "SuperAdmin" && role != "Admin") { server.send(403, "text/plain", "Forbidden"); return; }
    server.send(200, "text/plain", readSecurityLogs());
  });

  server.on("/backend/logs/upload", HTTP_POST, []() {
    if (checkSessionRole() != "SuperAdmin") { server.send(403, "text/plain", "Forbidden"); return; }
    String content = server.arg("plain");
    if (content == "") { content = server.arg("body"); }
    if (content == "") { if (server.args() > 0) content = server.arg(0); }
    File logFile = SPIFFS.open("/data/security_log.txt", FILE_WRITE);
    if (!logFile) { server.send(500, "text/plain", "Failed to open log file"); return; }
    if (content.length() > 0) logFile.print(content);
    logFile.close();
    server.send(200, "text/plain", "OK");
  });

  server.on("/handleLogin", HTTP_POST, []() {
    if (failedAttempts >= MAX_ATTEMPTS) {
      if (millis() - lockoutTimeStart < LOCKOUT_DURATION) {
        logSecurityEvent("FIREWALL", "LOCKOUT_ACTIVE_REJECTED_LOGIN_ATTEMPT");
        server.send(423, "text/plain", "Locked out. Cooldown active.");
        return;
      } else { failedAttempts = 0; }
    }
    String reqUser = server.arg("username");
    String reqPass = server.arg("password");
    String reqPassHash = computeMD5(reqPass);
    if (reqUser == SUPERADMIN_USER && reqPassHash == SUPERADMIN_HASH) {
      failedAttempts = 0;
      String token = createSession("SuperAdmin");
      server.sendHeader("Set-Cookie", "ESPSESSIONID=" + token + "; Path=/; HttpOnly");
      logSecurityEvent(reqUser, "SUCCESSFUL_LOGIN");
      server.sendHeader("Location", "/");
      server.send(303);
    } else if (reqUser == admin_user && reqPassHash == admin_hash) {
      failedAttempts = 0;
      String token = createSession("Admin");
      server.sendHeader("Set-Cookie", "ESPSESSIONID=" + token + "; Path=/; HttpOnly");
      logSecurityEvent(reqUser, "SUCCESSFUL_LOGIN");
      server.sendHeader("Location", "/");
      server.send(303);
    } else if (reqUser == user_user && reqPassHash == user_hash) {
      failedAttempts = 0;
      String token = createSession("User");
      server.sendHeader("Set-Cookie", "ESPSESSIONID=" + token + "; Path=/; HttpOnly");
      logSecurityEvent(reqUser, "SUCCESSFUL_LOGIN");
      server.sendHeader("Location", "/");
      server.send(303);
    } else {
      failedAttempts++;
      logSecurityEvent("FAILED_LOGIN", "USERNAME_" + reqUser);
      if (failedAttempts >= MAX_ATTEMPTS) {
        lockoutTimeStart = millis();
        logSecurityEvent("FIREWALL", "BRUTE_FORCE_LOCKOUT_TRIGGERED_" + String(MAX_ATTEMPTS) + "_FAILED_ATTEMPTS");
      }
      server.send(401, "text/html", fallbackLoginPage("Invalid username or password"));
    }
  });

  server.on("/addSchedule", HTTP_POST, []() {
    String role = checkSessionRole();
    if (role != "Admin" && role != "SuperAdmin" && role != "User") { server.send(403, "text/plain", "Unauthorized"); return; }
    String newTime = server.arg("sched_time");
    newTime.trim();
    if (scheduleCount < 20 && newTime.length() > 0) {
      scheduleList[scheduleCount++] = newTime;
      saveSchedules();
      logSecurityEvent(role, "ADDED_SCHEDULE_" + newTime);
    }
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/deleteSchedule", HTTP_POST, []() {
    String role = checkSessionRole();
    if (role != "Admin" && role != "SuperAdmin" && role != "User") { server.send(403, "text/plain", "Unauthorized"); return; }
    int indexToDelete = server.arg("index").toInt();
    if (indexToDelete >= 0 && indexToDelete < scheduleCount) {
      String removedTime = scheduleList[indexToDelete];
      for (int i = indexToDelete; i < scheduleCount - 1; i++) scheduleList[i] = scheduleList[i + 1];
      scheduleCount--;
      saveSchedules();
      logSecurityEvent(role, "REMOVED_SCHEDULE_" + removedTime);
    }
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/editSchedule", HTTP_POST, []() {
    String role = checkSessionRole();
    if (role != "Admin" && role != "SuperAdmin" && role != "User") { server.send(403, "text/plain", "Unauthorized"); return; }
    int indexToEdit = server.arg("index").toInt();
    String newTime = server.arg("sched_time");
    newTime.trim();
    if (indexToEdit >= 0 && indexToEdit < scheduleCount && newTime.length() > 0) {
      String oldTime = scheduleList[indexToEdit];
      scheduleList[indexToEdit] = newTime;
      saveSchedules();
      logSecurityEvent(role, "EDITED_SCHEDULE_FROM_" + oldTime + "_TO_" + newTime);
    }
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/saveNetwork", HTTP_POST, []() {
    String role = checkSessionRole();
    if (role != "Admin" && role != "SuperAdmin" && role != "User") { server.send(403, "text/plain", "Unauthorized"); return; }
    ap_ssid = server.arg("ap_name");
    sta_ssid = server.arg("station_ssid");
    sta_pass = server.arg("station_pass");
    saveConfiguration(ap_ssid, sta_ssid, sta_pass);
    logSecurityEvent(role, "UPDATED_NETWORK_CONFIG");
    String json = "{\"status\":\"saved\",\"message\":\"Network config saved. Attempting connection...\"}";
    server.send(200, "application/json", json);
    startWiFiConnection(sta_ssid, sta_pass);
  });

  server.on("/ring", []() {
    String currentRole = checkSessionRole();
    if (currentRole == "Guest") { server.send(403, "text/plain", "Forbidden"); return; }
    logSecurityEvent(currentRole, "TRIGGERED_MANUAL_BELL");
    server.send(200, "text/plain", "Bell Dispatched.");
    triggerPhysicalBell(3);
  });

  server.on("/logout", []() {
    String role = "Unknown";
    if (server.hasHeader("Cookie")) {
      String cookieHeader = server.header("Cookie");
      int index = cookieHeader.indexOf("ESPSESSIONID=");
      if (index != -1) {
        String token = cookieHeader.substring(index + 13);
        int endSpace = token.indexOf(';');
        if (endSpace != -1) token = token.substring(0, endSpace);
        token.trim();
        for (int i = 0; i < MAX_SESSIONS; i++) {
          if (sessions[i].token == token) { role = sessions[i].role; break; }
        }
      }
    }
    logSecurityEvent(role, "USER_LOGOUT");
    invalidateSession();
    server.sendHeader("Set-Cookie", "ESPSESSIONID=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.onNotFound([]() {
    server.sendHeader("Location", "http://172.217.28.1/");
    server.send(302, "text/plain", "");
  });
}

void setupI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = I2S_SAMPLE_RATE,
    .bits_per_sample = (i2s_bits_per_sample_t)I2S_BITS_PER_SAMPLE,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = I2S_BUFFER_SIZE,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  i2s_pin_config_t pin_config = {
    .bck_io_num = MAX98357A_BCLK_PIN,
    .ws_io_num = MAX98357A_LRC_PIN,
    .data_out_num = MAX98357A_DIN_PIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) { Serial.println("I2S driver install failed!"); return; }
  err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK) { Serial.println("I2S set pin failed!"); return; }
  i2s_set_clk(I2S_PORT, I2S_SAMPLE_RATE, (i2s_bits_per_sample_t)I2S_BITS_PER_SAMPLE, I2S_CHANNEL_MONO);
  Serial.println("I2S initialized for MAX98357A");
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  rtc.begin();
  SPIFFS.begin(true);
  if (!SD.begin(SD_CS_PIN)) Serial.println("SD Card Initialization Error!");
  else Serial.println("SD Card Loaded.");
  loadConfiguration();
  loadSchedules();
  logSecurityEvent("SYSTEM", "SYSTEM_BOOT");
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(ap_ssid.c_str());
  if (sta_ssid != "") startWiFiConnection(sta_ssid, sta_pass);
  setupI2S();
  dnsServer.start(DNS_PORT, "*", apIP);
  setupRoutes();
  server.begin();
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  handleWiFiConnection();
  static String lastMatchedTime = "";
  String currentTimeHHMM = getHHMM();
  if (currentTimeHHMM != lastMatchedTime) {
    for (int i = 0; i < scheduleCount; i++) {
      if (currentTimeHHMM == scheduleList[i]) {
        lastMatchedTime = currentTimeHHMM;
        logSecurityEvent("AUTOMATION", "SCHEDULE_TRIGGERED_AT_" + currentTimeHHMM);
        triggerPhysicalBell(5);
        break;
      }
    }
  }
}
