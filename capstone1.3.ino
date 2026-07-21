#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <Wire.h>
#include <RTClib.h>
#include <mbedtls/md5.h>
#include <esp_wifi.h>
#include "time.h"

// --- HARDWARE CONFIGURATION ---
const int BELL_PIN = 25; 

// --- SECURITY STORAGE & AAA CONFIG ---
const char* ADMIN_USER = "admin";
const char* ADMIN_HASH = "482c811da5d5b4bc6d497ffa98491e38"; // MD5 for "password123"

String staff_user = "staff";
String staff_pass = "staffpassword"; 

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
const unsigned long SESSION_TIMEOUT = 600000; // 10 minutes in milliseconds

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

// --- SECURE SESSION AND TIMEOUT ENGINE ---
String generateSessionToken() {
  String chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  String token = "";
  for (int i = 0; i < 16; i++) {
    token += chars[random(0, chars.length())];
  }
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
        logSecurityEvent("FIREWALL", "SESSION_TIMED_OUT_FOR_INACTIVITY");
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

// --- FILE SYSTEM CONFIG & SCHEDULE MANAGEMENT ---
void loadConfiguration() {
  if (SPIFFS.exists("/config.txt")) {
    File configFile = SPIFFS.open("/config.txt", FILE_READ);
    if (configFile) {
      ap_ssid = configFile.readStringUntil('\n');
      sta_ssid = configFile.readStringUntil('\n');
      sta_pass = configFile.readStringUntil('\n');
      ap_ssid.trim(); sta_ssid.trim(); sta_pass.trim();
      configFile.close();
    }
  }
  if (SPIFFS.exists("/staff.txt")) {
    File staffFile = SPIFFS.open("/staff.txt", FILE_READ);
    if (staffFile) { staff_pass = staffFile.readStringUntil('\n'); staff_pass.trim(); staffFile.close(); }
  }
}

void saveConfiguration(String newAP, String newStationSSID, String newStationPass) {
  File configFile = SPIFFS.open("/config.txt", FILE_WRITE);
  if (configFile) { configFile.println(newAP); configFile.println(newStationSSID); configFile.println(newStationPass); configFile.close(); }
}

void loadSchedules() {
  scheduleCount = 0;
  if (SPIFFS.exists("/schedule.txt")) {
    File schedFile = SPIFFS.open("/schedule.txt", FILE_READ);
    while (schedFile.available() && scheduleCount < 20) {
      String line = schedFile.readStringUntil('\n'); line.trim();
      if (line.length() > 0) { scheduleList[scheduleCount] = line; scheduleCount++; }
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
  File logFile = SPIFFS.open("/security_log.txt", FILE_APPEND);
  if (logFile) {
    String logEntry = "[" + getNow() + "] Identity: " + user + " | Action: " + action;
    logFile.println(logEntry);
    logFile.close();
  }
}

String readSecurityLogs() {
  if (!SPIFFS.exists("/security_log.txt")) return "No system logs generated yet.";
  File logFile = SPIFFS.open("/security_log.txt", FILE_READ);
  if (!logFile) return "Error reading security log module.";
  String output = "";
  while (logFile.available()) {
    String line = logFile.readStringUntil('\n'); line.trim();
    if(line.length() > 0) output = line + "\n" + output; 
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
  for(int i = 0; i < 16; i++) sprintf(&buf[i*2], "%02x", digest[i]);
  return String(buf);
}

bool connectAndSyncTime(String ssid, String pass) {
  if(ssid == "" || ssid == "Network Name") return false;
  syncStatus = "Connecting to " + ssid + "...";
  WiFi.begin(ssid.c_str(), pass.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 15) { delay(500); attempts++; }
  if(WiFi.status() == WL_CONNECTED) {
    syncStatus = "Connected! Syncing Time...";
    configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org");
    struct tm timeinfo;
    for(int i=0; i<6; i++) {
      if(getLocalTime(&timeinfo)) {
        if(rtc.begin()) rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
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

// --- SECURED UI VIEWS ---
String loginPage(String errorMsg = "") {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif; text-align:center; background:#f4f4f4; padding-top:50px;}";
  html += ".card{background:white; padding:30px; border-radius:12px; display:inline-block; box-shadow:0 4px 6px rgba(0,0,0,0.1); border-top:8px solid #800000;}";
  html += "input[type=text], input[type=password]{width:90%; padding:10px; margin:10px 0; border:1px solid #ccc; border-radius:4px;}";
  html += "button{background:#800000; color:white; padding:10px 20px; border:none; border-radius:4px; cursor:pointer; width:95%;}</style></head><body>";
  html += "<div class='card'><h2>FCU Smart Bell Login</h2>";
  if(errorMsg != "") html += "<p style='color:red;'>" + errorMsg + "</p>";
  
  if (failedAttempts >= MAX_ATTEMPTS) {
    html += "<p>Please try again after the cooldown period expires.</p>";
  } else {
    html += "<form action='/handleLogin' method='POST'>";
    html += "<input type='text' name='username' placeholder='Username' required><br>";
    html += "<input type='password' name='password' placeholder='Password' required><br><br>";
    html += "<button type='submit'>Authenticate Device</button></form>";
  }
  html += "</div></body></html>";
  return html;
}

String adminDashboard(String userRole) {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif; text-align:center; background:#f4f4f4; padding: 20px;}";
  html += ".card{background:white; padding:20px; border-radius:15px; display:inline-block; box-shadow:0 4px 8px rgba(0,0,0,0.1); border-top: 10px solid #800000; width: 100%; max-width: 450px; text-align: left;}";
  html += "h1, h3{color:#800000; text-align: center;} #time-display{font-size:1.4rem; font-weight:bold; color:#222; text-align:center; margin-bottom:5px;}";
  html += "#status-display{font-size:0.9rem; text-align:center; color:#555; margin-bottom:15px; font-style:italic;}";
  html += "input[type=text], input[type=password], input[type=time]{width:94%; padding:8px; margin:8px 0; border:1px solid #ccc; border-radius:4px;}";
  html += ".btn{background:#800000; color:white; padding:12px; border:none; border-radius:5px; font-weight:bold; cursor:pointer; width:100%; margin-top:10px;}";
  html += ".btn-secondary{background:#555; color:white; padding:10px; border:none; border-radius:5px; cursor:pointer; width:100%; margin-top:5px;}";
  html += ".log-box{background:#222; color:#00ff00; font-family:monospace; font-size:11px; padding:10px; height:120px; overflow-y:scroll; border-radius:5px; white-space:pre-wrap;}";
  html += ".sched-item{background:#eee; padding:8px; margin:5px 0; border-radius:4px; display:flex; justify-content:space-between; align-items:center;}";
  html += ".logout{color:red; text-decoration:none; display:block; text-align:center; margin-top:20px;}</style>";
  
  html += "<script>setInterval(function(){ ";
  html += "  fetch('/api/time').then(r => { if(r.status==403) { window.location.reload(); } return r.text(); }).then(d => { if(d) document.getElementById('time-display').innerText = d; }); ";
  html += "  fetch('/api/status').then(r => r.text()).then(d => { if(d) document.getElementById('status-display').innerText = d; }); ";
  html += "}, 1000);</script></head><body>";
  
  html += "<div class='card'><h1>FCU Panel (" + userRole + ")</h1>";
  html += "<div id='time-display'>" + getNow() + "</div>";
  html += "<div id='status-display'>" + syncStatus + "</div>";
  html += "<button class='btn' onclick=\"location.href='/ring'\">TRIGGER BELL MANUALLY</button><hr>";
  
  html += "<h3>Active Bell Schedules</h3>";
  if (scheduleCount == 0) {
    html += "<p style='color:#777; font-style:italic;'>No active schedules configured.</p>";
  } else {
    for (int i = 0; i < scheduleCount; i++) {
      html += "<div class='sched-item'><span>⏰ <strong>" + scheduleList[i] + "</strong></span>";
      if (userRole == "Admin") {
        html += "<form action='/deleteSchedule' method='POST' style='margin:0;'>";
        html += "<input type='hidden' name='index' value='" + String(i) + "'>";
        html += "<button type='submit' style='background:none; border:none; color:red; cursor:pointer; font-weight:bold;'>[Delete]</button></form>";
      }
      html += "</div>";
    }
  }

  if (userRole == "Admin") {
    html += "<form action='/addSchedule' method='POST' style='margin-top:10px;'>";
    html += "<input type='time' name='sched_time' required>";
    html += "<button type='submit' class='btn-secondary' style='background:#2E7D32;'>Add Automation Slot</button>";
    html += "</form><hr>";

    // --- NETWORK SETTINGS ---
    html += "<h3>Network Configuration</h3>";
    html += "<form action='/saveNetwork' method='POST'>";
    html += "<label style='font-size:0.85rem; font-weight:bold;'>Device Hotspot SSID:</label><br>";
    html += "<input type='text' name='ap_name' value='" + ap_ssid + "' required><br>";

    html += "<label style='font-size:0.85rem; font-weight:bold;'>WiFi SSID (NTP Sync):</label><br>";
    html += "<input type='text' name='station_ssid' value='" + sta_ssid + "'><br>";

    html += "<label style='font-size:0.85rem; font-weight:bold;'>WiFi Password:</label><br>";
    html += "<input type='password' name='station_pass' value='" + sta_pass + "'><br>";

    html += "<button type='submit' class='btn-secondary'>Save Network</button>";
    html += "</form><hr>";

    // --- PASSWORD SETTINGS ---
    html += "<h3>Change Staff Password</h3>";
    html += "<form action='/changePassword' method='POST'>";
    html += "<label style='font-size:0.85rem; font-weight:bold; color:#b22222;'>New Staff Password:</label><br>";
    html += "<input type='password' name='new_staff_pass' placeholder='Enter new password' required><br>";

    html += "<button type='submit' class='btn-secondary' style='background:#b22222;'>Update Password</button>";
    html += "</form><hr>";

    html += "<h3>Security & Audit Logs</h3>";
    html += "<div class='log-box'>" + readSecurityLogs() + "</div>";
    html += "<form action='/clearLogs' method='POST' style='margin-top:5px;'>";
    html += "<button type='submit' class='btn-secondary' style='background:#b22222;' onclick=\"return confirm('Purge audit database logs?');\">Clear Log History</button>";
    html += "</form>";
  } else {
    html += "<p style='color:#777; font-style:italic;'>Notice: Rules of Least Privilege apply. Schedule management requires Admin clearance.</p>";
  }
  html += "<a class='logout' href='/logout'>Terminate Session</a></div></body></html>";
  return html;
}

// --- BACKEND ENDPOINTS & AAA ENFORCEMENT ---
void setupRoutes() {
  const char* headerkeys[] = {"Cookie"};
  size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
  server.collectHeaders(headerkeys, headerkeyssize);
  // ===== SAVE NETWORK ONLY =====
server.on("/saveNetwork", HTTP_POST, []() {
  if (checkSessionRole() != "Admin") {
    server.send(403, "text/plain", "Forbidden");
    return;
  }

  ap_ssid = server.arg("ap_name");
  sta_ssid = server.arg("station_ssid");
  sta_pass = server.arg("station_pass");

  saveConfiguration(ap_ssid, sta_ssid, sta_pass);

  logSecurityEvent("Admin", "UPDATED_NETWORK_CONFIGURATION");

  server.sendHeader("Location", "/");
  server.send(303);

  // reconnect WiFi
  connectAndSyncTime(sta_ssid, sta_pass);
});
// ===== CHANGE PASSWORD ONLY =====
server.on("/changePassword", HTTP_POST, []() {
  if (checkSessionRole() != "Admin") {
    server.send(403, "text/plain", "Forbidden");
    return;
  }

  String newPass = server.arg("new_staff_pass");
  newPass.trim();

  if (newPass.length() < 4) {
    server.send(200, "text/plain", "Password too short");
    return;
  }

  staff_pass = newPass;

  File staffFile = SPIFFS.open("/staff.txt", FILE_WRITE);
  if (staffFile) {
    staffFile.println(staff_pass);
    staffFile.close();
  }

  logSecurityEvent("Admin", "CHANGED_STAFF_PASSWORD");

  server.sendHeader("Location", "/");
  server.send(303);
});

  server.on("/", []() {
    String userRole = checkSessionRole();
    if (userRole == "Guest") server.send(200, "text/html", loginPage());
    else server.send(200, "text/html", adminDashboard(userRole));
  });

  server.on("/api/time", []() { 
    if (checkSessionRole() == "Guest") { server.send(403, "text/plain", "Forbidden"); return; }
    server.send(200, "text/plain", getNow()); 
  });
  
  server.on("/api/status", []() { 
    if (checkSessionRole() == "Guest") { server.send(403, "text/plain", "Forbidden"); return; }
    server.send(200, "text/plain", syncStatus); 
  });

  server.on("/handleLogin", HTTP_POST, []() {
    if (failedAttempts >= MAX_ATTEMPTS) {
      if (millis() - lockoutTimeStart < LOCKOUT_DURATION) {
        unsigned long remainingTime = (LOCKOUT_DURATION - (millis() - lockoutTimeStart)) / 1000;
        server.send(200, "text/html", loginPage("SYSTEM LOCKED. Cooldown remaining: " + String(remainingTime) + "s"));
        return;
      } else {
        failedAttempts = 0;
        logSecurityEvent("SYSTEM", "LOCKOUT_COOLDOWN_EXPIRED");
      }
    }

    String reqUser = server.arg("username");
    String reqPass = server.arg("password");
    
    if (reqUser == ADMIN_USER && computeMD5(reqPass) == ADMIN_HASH) {
      failedAttempts = 0; 
      String token = createSession("Admin");
      server.sendHeader("Set-Cookie", "ESPSESSIONID=" + token + "; Path=/; HttpOnly");
      logSecurityEvent(reqUser, "SUCCESSFUL_LOGIN_SESSION_STARTED");
      server.sendHeader("Location", "/"); server.send(303);
    } 
    else if (reqUser == staff_user && reqPass == staff_pass) {
      failedAttempts = 0;
      String token = createSession("Staff");
      server.sendHeader("Set-Cookie", "ESPSESSIONID=" + token + "; Path=/; HttpOnly");
      logSecurityEvent(reqUser, "SUCCESSFUL_LOGIN_SESSION_STARTED");
      server.sendHeader("Location", "/"); server.send(303);
    } 
    else {
      failedAttempts++;
      logSecurityEvent(reqUser != "" ? reqUser : "Unknown", "FAILED_LOGIN_ATTEMPT_" + String(failedAttempts));
      if (failedAttempts >= MAX_ATTEMPTS) {
        lockoutTimeStart = millis();
        logSecurityEvent("FIREWALL", "BRUTE_FORCE_LOCKOUT_TRIGGERED");
        server.send(200, "text/html", loginPage("CRITICAL ERROR: Locked out for 5 minutes."));
      } else {
        int remainingAttempts = MAX_ATTEMPTS - failedAttempts;
        server.send(200, "text/html", loginPage("Invalid Credentials. Attempts remaining: " + String(remainingAttempts)));
      }
    }
  });

  server.on("/addSchedule", HTTP_POST, []() {
    if (checkSessionRole() != "Admin") { server.send(403, "text/plain", "Forbidden: Admin Rights Required"); return; }
    
    String newTime = server.arg("sched_time"); newTime.trim();
    if (scheduleCount < 20 && newTime.length() > 0) {
      scheduleList[scheduleCount] = newTime; scheduleCount++; saveSchedules();
      logSecurityEvent("Admin", "ADDED_SCHEDULE_" + newTime);
    }
    server.sendHeader("Location", "/"); server.send(303);
  });

  server.on("/deleteSchedule", HTTP_POST, []() {
    if (checkSessionRole() != "Admin") { server.send(403, "text/plain", "Forbidden: Admin Rights Required"); return; }
    
    int indexToDelete = server.arg("index").toInt();
    if (indexToDelete >= 0 && indexToDelete < scheduleCount) {
      String removedTime = scheduleList[indexToDelete];
      for (int i = indexToDelete; i < scheduleCount - 1; i++) scheduleList[i] = scheduleList[i + 1];
      scheduleCount--; saveSchedules();
      logSecurityEvent("Admin", "REMOVED_SCHEDULE_" + removedTime);
    }
    server.sendHeader("Location", "/"); server.send(303);
  });

  server.on("/saveSettings", HTTP_POST, []() {
    if (checkSessionRole() != "Admin") { server.send(403, "text/plain", "Forbidden: Admin Rights Required"); return; }
    
    ap_ssid = server.arg("ap_name"); 
    sta_ssid = server.arg("station_ssid"); 
    sta_pass = server.arg("station_pass");
    String newStaffPass = server.arg("new_staff_pass");
    
    newStaffPass.trim();
    if(newStaffPass.length() > 0) {
      staff_pass = newStaffPass;
      File staffFile = SPIFFS.open("/staff.txt", FILE_WRITE);
      if(staffFile) {
        staffFile.println(staff_pass);
        staffFile.close();
        logSecurityEvent("Admin", "UPDATED_STAFF_ROLE_CREDENTIALS");
      }
    }

    saveConfiguration(ap_ssid, sta_ssid, sta_pass);
    logSecurityEvent("Admin", "UPDATED_SYSTEM_NETWORK_SETTINGS");
    server.sendHeader("Location", "/"); server.send(303);
    connectAndSyncTime(sta_ssid, sta_pass);
  });

  server.on("/clearLogs", HTTP_POST, []() {
    if (checkSessionRole() != "Admin") { server.send(403, "text/plain", "Forbidden"); return; }
    SPIFFS.remove("/security_log.txt");
    logSecurityEvent("Admin", "PURGED_AUDIT_LOGS");
    server.sendHeader("Location", "/"); server.send(303);
  });

  server.on("/ring", []() {
    String currentRole = checkSessionRole();
    if (currentRole == "Guest") { server.send(403, "text/plain", "Forbidden"); return; }
    
    logSecurityEvent(currentRole, "TRIGGERED_MANUAL_BELL");
    server.send(200, "text/plain", "Bell Action Dispatched.");
    triggerPhysicalBell(3); 
  });

  server.on("/logout", []() {
    logSecurityEvent(checkSessionRole(), "TERMINATED_SESSION_LOGOUT");
    invalidateSession();
    server.sendHeader("Set-Cookie", "ESPSESSIONID=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
    server.sendHeader("Location", "/"); server.send(303);
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
  
  randomSeed(analogRead(0)); 
  Wire.begin();
  rtc.begin();
  SPIFFS.begin(true);

  loadConfiguration();
  loadSchedules(); 

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(ap_ssid.c_str());

  if(sta_ssid != "") connectAndSyncTime(sta_ssid, sta_pass);

  logSecurityEvent("SYSTEM", "SECURE_BOOT_COMPLETED");
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
        logSecurityEvent("AUTOMATION_ENGINE", "SCHEDULE_MATCHED_TRIGGERING_BELL_AT_" + currentTimeHHMM);
        triggerPhysicalBell(5); 
        break; 
      }
    }
  }
}