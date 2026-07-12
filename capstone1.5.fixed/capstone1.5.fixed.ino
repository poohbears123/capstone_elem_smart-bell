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

// --- HARDWARE CONFIGURATION ---
// MAX98357A (I2S) pin mapping
// DIN  -> D25
// BCLK -> D26
// LRC  -> D27
const int MAX98357A_DIN_PIN = 25;
const int MAX98357A_BCLK_PIN = 26;
const int MAX98357A_LRC_PIN = 27;

// Legacy: keep BELL_PIN for the rest of the project if needed.
// NOTE: If this firmware is purely for audio bell, BELL_PIN is no longer used.
const int BELL_PIN = MAX98357A_DIN_PIN;

const int SD_CS_PIN = 5;


// --- SECURITY STORAGE & AAA CONFIG ---
// Roles:
// - SuperAdmin: logs + full schedule/network + ring
// - Admin: schedule/network + ring (no logs)
// - User: schedule/network + ring (no logs)
// Passwords are checked via MD5 for everyone (fixed accounts).

const char* SUPERADMIN_USER = "superadmin";
const char* SUPERADMIN_HASH = "9bf18948eb1ce2c87fb32fc26d46779d"; // MD5("pooh21abc")

String admin_user = "admin";
String admin_hash = "482c811da5d5b4bc6d497ffa98491e38"; // MD5("password123")

String user_user = "user";
String user_hash = "482c811da5d5b4bc6d497ffa98491e38"; // MD5("password123")

// Brute-Force Defense Variables
int failedAttempts = 0;
unsigned long lockoutTimeStart = 0;
const unsigned long LOCKOUT_DURATION = 300000;
const int MAX_ATTEMPTS = 4;

// Dynamic Configuration Variables
String ap_ssid = "FCU_Secure_Bell";
String sta_ssid = "";
String sta_pass = "";
String syncStatus = "Not Connected";

// Schedule Storage Arrays
String scheduleList[20];
int scheduleCount = 0;

// --- COOKIE-TOKEN SESSION DATABASE & TIMEOUT CONFIG ---
const int MAX_SESSIONS = 10;
const unsigned long SESSION_TIMEOUT = 600000; // 10 minutes

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

// Philippine Time Parameters (GMT+8)
const long gmtOffset_sec = 28800;
const int daylightOffset_sec = 0;

void logSecurityEvent(String user, String action);

// --- TIME FORMATTING UTILITY ---
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

// --- SECURE SESSION ENGINE ---
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

// --- STORAGE ---
void loadConfiguration() {
  if (SPIFFS.exists("/config.txt")) {
    File configFile = SPIFFS.open("/config.txt", FILE_READ);
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
  File configFile = SPIFFS.open("/config.txt", FILE_WRITE);
  if (configFile) {
    configFile.println(newAP);
    configFile.println(newStationSSID);
    configFile.println(newStationPass);
    configFile.close();
  }
}

void loadSchedules() {
  scheduleCount = 0;
  if (SPIFFS.exists("/schedule.txt")) {
    File schedFile = SPIFFS.open("/schedule.txt", FILE_READ);
    while (schedFile.available() && scheduleCount < 20) {
      String line = schedFile.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) scheduleList[scheduleCount++] = line;
    }
    schedFile.close();
  }
}

void saveSchedules() {
  File schedFile = SPIFFS.open("/schedule.txt", FILE_WRITE);
  for (int i = 0; i < scheduleCount; i++) schedFile.println(scheduleList[i]);
  schedFile.close();
}

void logSecurityEvent(String user, String action) {
  // Only SuperAdmin identity gets logged to /data/security_log.txt
  // (Admins/Users won't write logs).
  if (user != SUPERADMIN_USER) return;

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

bool connectAndSyncTime(String ssid, String pass) {
  if (ssid == "" || ssid == "Network Name") return false;
  syncStatus = "Connecting to " + ssid + "...";
  WiFi.begin(ssid.c_str(), pass.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 15) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    syncStatus = "Connected! Syncing Time...";
    configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org");

    struct tm timeinfo;
    for (int i = 0; i < 6; i++) {
      if (getLocalTime(&timeinfo)) {
        if (rtc.begin()) rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
        syncStatus = "Synced successfully via " + ssid;
        return true;
      }
      delay(500);
    }
  }

  syncStatus = "Connection Failed. Running on Local Modules.";
  WiFi.disconnect();
  return false;
}

void triggerPhysicalBell(int durationSeconds) {
  digitalWrite(BELL_PIN, HIGH);
  delay(durationSeconds * 1000);
  digitalWrite(BELL_PIN, LOW);
}

// --- STREAM SAFELY FROM SD CARD ---
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
  // Matches the visual style of ui/login.html, but adds a popup/modal-style error.
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
    // Inline preview (also helps if modal is blocked)
    html += "<div class='error'>" + errorMsg + "</div>";
  }

  html += "<form action='/handleLogin' method='POST' autocomplete='off'>"
          "<input type='text' name='username' placeholder='Username' required />"
          "<input type='password' name='password' placeholder='Password' required />"
          "<button type='submit'>Authenticate</button>"
          "</form>"
          "<div class='hint'>After login, you’ll be redirected to the dashboard.</div>"
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
    if (role == "Guest") {
      server.send(403, "text/plain", "Forbidden");
      return;
    }
    String json = "{";
    json += "\"time\":\"" + getNow() + "\",";
    json += "\"status\":\"" + syncStatus + "\",";
    json += "\"role\":\"" + role + "\",";
    json += "\"ssid\":\"" + sta_ssid + "\"";
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/api/schedules", []() {
    String role = checkSessionRole();
    if (role == "Guest") {
      server.send(403, "text/plain", "Forbidden");
      return;
    }
    String json = "[";
    for (int i = 0; i < scheduleCount; i++) {
      json += "\"" + scheduleList[i] + "\"";
      if (i < scheduleCount - 1) json += ",";
    }
    json += "]";
    server.send(200, "application/json", json);
  });

  server.on("/backend/logs", []() {
    if (checkSessionRole() != "SuperAdmin") {
      server.send(403, "text/plain", "Forbidden");
      return;
    }
    server.send(200, "text/plain", readSecurityLogs());
  });

  // SuperAdmin can upload/replace security log file contents.
  // Body is plain text; firmware overwrites /data/security_log.txt
  server.on("/backend/logs/upload", HTTP_POST, []() {
    if (checkSessionRole() != "SuperAdmin") {
      server.send(403, "text/plain", "Forbidden");
      return;
    }

    String content = server.arg("plain");
    // WebServer's arg("plain") works for urlencoded; for text/plain some versions won't.
    // So we also read raw body from server.arg("body")-like support via hasArg("plain").
    if (content == "") {
      content = server.arg("body");
    }

    // Fallback: if still empty, try first form field (some browsers may send it differently)
    if (content == "") {
      if (server.args() > 0) content = server.arg(0);
    }

    File logFile = SPIFFS.open("/data/security_log.txt", FILE_WRITE);
    if (!logFile) {
      server.send(500, "text/plain", "Failed to open log file");
      return;
    }
    if (content.length() > 0) logFile.print(content);
    logFile.close();

    server.send(200, "text/plain", "OK");
  });


  server.on("/handleLogin", HTTP_POST, []() {
    if (failedAttempts >= MAX_ATTEMPTS) {
      if (millis() - lockoutTimeStart < LOCKOUT_DURATION) {
        server.send(423, "text/plain", "Locked out. Cooldown active.");
        return;
      } else {
        failedAttempts = 0;
      }
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
      if (failedAttempts >= MAX_ATTEMPTS) lockoutTimeStart = millis();
      // Re-render login page with an inline error message (no browser back needed)
      server.send(401, "text/html", fallbackLoginPage("Invalid username or password"));
    }
  });

  server.on("/addSchedule", HTTP_POST, []() {
    String role = checkSessionRole();
    if (role != "Admin" && role != "SuperAdmin" && role != "User") {
      server.send(403, "text/plain", "Unauthorized");
      return;
    }

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
    if (role != "Admin" && role != "SuperAdmin" && role != "User") {
      server.send(403, "text/plain", "Unauthorized");
      return;
    }

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

  // Edit schedule time at a given index (CRUD: Create/Read/Update/Delete)
  server.on("/editSchedule", HTTP_POST, []() {
    String role = checkSessionRole();
    if (role != "Admin" && role != "SuperAdmin" && role != "User") {
      server.send(403, "text/plain", "Unauthorized");
      return;
    }

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
    if (role != "Admin" && role != "SuperAdmin" && role != "User") {
      server.send(403, "text/plain", "Unauthorized");
      return;
    }

    ap_ssid = server.arg("ap_name");
    sta_ssid = server.arg("station_ssid");
    sta_pass = server.arg("station_pass");

    saveConfiguration(ap_ssid, sta_ssid, sta_pass);
    logSecurityEvent(role, "UPDATED_NETWORK_CONFIG");

    server.sendHeader("Location", "/");
    server.send(303);

    connectAndSyncTime(sta_ssid, sta_pass);
  });

  server.on("/ring", []() {
    String currentRole = checkSessionRole();
    if (currentRole == "Guest") {
      server.send(403, "text/plain", "Forbidden");
      return;
    }

    logSecurityEvent(currentRole, "TRIGGERED_MANUAL_BELL");
    server.send(200, "text/plain", "Bell Dispatched.");
    triggerPhysicalBell(3);
  });

  server.on("/logout", []() {
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

void setup() {
  Serial.begin(115200);
  pinMode(BELL_PIN, OUTPUT);
  digitalWrite(BELL_PIN, LOW);

  Wire.begin();
  rtc.begin();
  SPIFFS.begin(true);

  if (!SD.begin(SD_CS_PIN)) Serial.println("SD Card Initialization Error!");
  else Serial.println("SD Card Loaded.");

  loadConfiguration();
  loadSchedules();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(ap_ssid.c_str());
  if (sta_ssid != "") connectAndSyncTime(sta_ssid, sta_pass);

  dnsServer.start(DNS_PORT, "*", apIP);
  setupRoutes();
  server.begin();
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

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

