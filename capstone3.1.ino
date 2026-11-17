#include <Arduino.h>
#include <esp_heap_caps.h>
#include "ui_embed.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>
#include <Wire.h>
#include <RTClib.h>
#include <ArduinoJson.h>
#include <mbedtls/md5.h>
#include <time.h>
#include "Audio.h"

#define FIRMWARE_VERSION "3.0.0"
#define FIRMWARE_NAME    "FCU Alarm Clock (FreeRTOS, NTP/RTC, AAA Audit, Audio)"

// ------------------------- Pin Definitions -------------------------
#define SD_CS_PIN       10
#define SD_SCK_PIN      12
#define SD_MISO_PIN     13
#define SD_MOSI_PIN     11

#define I2S_LRC_PIN     16
#define I2S_BCLK_PIN    15
#define I2S_DIN_PIN     7

#define I2C_SDA_PIN     8
#define I2C_SCL_PIN     9

#define PTT_BUTTON_PIN  14
#define CALL_ALERT_PIN  21
#define PLAY_BUTTON_PIN 4

// ------------------------- Constants -------------------------
#define ALARM_MAX_ENTRIES   20
#define MAC_ADDR_STR_LEN    18
#define CSV_LINE_MAX_LEN    128
#define MAX_SSID_LEN        32
#define AUDIO_PATH_MAX_LEN  64
#define DEBOUNCE_MS         50
#define AUDIO_QUEUE_SIZE    5
#define ALARM_FILE          "/data/alarms.json"
#define LOG_FILE            "/data/activity.csv"
#define SECURITY_LOG_FILE   "/security_log.txt"
#define USERS_FILE          "/data/users.json"
#define NTP_RESYNC_MS       3600000UL   // 1 hour
#define SESSION_TIMEOUT_MS  600000UL    // 10 minutes
#define MAX_SESSIONS        8
#define MAX_LOGIN_ATTEMPTS  5
#define LOCKOUT_MS          300000UL    // 5 minutes
#define JSON_BODY_MAX_LEN   4096
#define CAPTIVE_PORTAL_PORT 53
#define WIFI_FILE           "/data/wifi.json"
#define WIFI_CONNECT_TIMEOUT_MS 15000UL

const char* AP_PASSWORD = "ChangeMe123!";
const char* NTP_SERVER  = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 8 * 3600;
const int   DAYLIGHT_OFFSET_SEC = 0;

const char* ADMIN_USER = "admin";
const char* ADMIN_PASS = "admin123";
const char* ADMIN_PASS_HASH = "0192023a7bbd73250516f069df18b500"; // md5("admin123")

// ------------------------- Types -------------------------
typedef struct {
  uint32_t id;
  char label[32];
  uint8_t hour;
  uint8_t minute;
  uint8_t days;       // bitmask: bit0=Sun ... bit6=Sat
  bool  enabled;
  char  audioPath[AUDIO_PATH_MAX_LEN];
  uint32_t lastFired; // dayStart + alarmSecs, 0 = never fired today
} AlarmEntry_t;

typedef struct { char path[AUDIO_PATH_MAX_LEN]; } AudioMsg_t;

typedef struct {
  char token[40];
  char user[16];
  char role[16];
  uint32_t expiry;
  bool active;
} Session_t;

typedef struct {
  int lastState;
  uint32_t lastDebounceTime;
  bool stablePress;
} DebounceButton_t;

// ------------------------- Globals -------------------------
TaskHandle_t xWebTaskHandle = NULL, xAudioTaskHandle = NULL, xSystemEngineTaskHandle = NULL;
QueueHandle_t xAudioQueue = NULL;
SemaphoreHandle_t xI2CMutex = NULL, xAlarmMutex = NULL, xSdMutex = NULL, xLfsMutex = NULL;

RTC_DS3231 rtc;
AsyncWebServer server(80);
DNSServer dnsServer;
Audio audio;

SPIClass customSPI = SPIClass(FSPI);

char apSSID[MAX_SSID_LEN] = "FCU_Alarm_Bell";
char staSSID[MAX_SSID_LEN] = "";
char staPass[64] = "";
bool wifiSTAConnected = false;
bool wifiSTAConnecting = false;
bool ntpSynced = false;

AlarmEntry_t alarms[ALARM_MAX_ENTRIES];
int alarmCount = 0;
volatile bool alarmsNeedsReload = false;

DateTime bootTime(2026, 1, 1, 0, 0, 0); // anchored redundant time
uint32_t bootMillis = 0;
uint32_t lastNtpAttempt = 0;

bool rtcOK = false;

File uploadFile;

Session_t sessions[MAX_SESSIONS];
int failedAttempts = 0;
uint32_t lockoutUntil = 0;

volatile bool pttPressed = false;
volatile bool alarmRequested = false;

char playButtonAudioPath[AUDIO_PATH_MAX_LEN] = "/chime.mp3";

// ------------------------- SD Helpers -------------------------
static bool sdTake(uint32_t ms = 2000) {
  if (xSemaphoreTake(xSdMutex, pdMS_TO_TICKS(ms)) == pdTRUE) return true;
  Serial.printf("[SD] mutex timeout after %u ms\n", ms);
  return false;
}
static void sdGive(void) { xSemaphoreGive(xSdMutex); }

static bool sdExists(const char* path) {
  bool r = false;
  if (sdTake()) { r = SD.exists(path); sdGive(); }
  return r;
}

static void appendFileLine(const char* path, const char* line) {
  if (!sdTake()) return;
  // Ensure parent directory exists
  String p = path;
  int slash = p.lastIndexOf('/');
  if (slash > 0) {
    String dir = p.substring(0, slash);
    if (!SD.exists(dir.c_str())) SD.mkdir(dir.c_str());
  }
  File f = SD.open(path, "a");
  if (!f) f = SD.open(path, FILE_APPEND);
  if (f) { f.print(line); f.close(); }
  else Serial.printf("[SD] append failed: %s\n", path);
  sdGive();
}

static bool sdReadFile(const char* path, String& out) {
  if (!sdTake()) return false;
  File f = SD.open(path, FILE_READ);
  bool ok = (bool)f;
  if (ok) { out = ""; while (f.available()) out += (char)f.read(); f.close(); }
  else Serial.printf("[SD] read failed: %s\n", path);
  sdGive();
  return ok;
}

static bool sdRemount(void) {
  Serial.println("[SD] attempting re-mount...");

  // Release any mutex before the slow SPI re-init to avoid blocking other tasks
  delay(100);
  customSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  uint32_t speeds[] = { 2000000, 1000000, 400000, 200000 };
  bool mounted = false;
  for (int i = 0; i < 4 && !mounted; i++) {
    delay(300);
    Serial.printf("[SD] remount trying %u Hz...\n", speeds[i]);
    mounted = SD.begin(SD_CS_PIN, customSPI, speeds[i]);
  }

  if (!mounted) {
    Serial.println("[SD] re-mount FAILED");
    return false;
  }

  Serial.println("[SD] re-mounted OK");
  // Briefly take mutex only for directory creation
  if (sdTake()) {
    if (!SD.exists("/data")) SD.mkdir("/data");
    sdGive();
  }
  return true;
}

static bool sdHealthCheck(void) {
  // Quick check: try to stat a known file
  return sdExists("/data");
}

// ------------------------- LittleFS Helpers (Internal ESP32-S3 Flash) -------------------------
// Alarms, WiFi config, users, and audio files live here. Audit logs stay on SD.

static bool lfsTake(uint32_t ms = 2000) {
  if (xSemaphoreTake(xLfsMutex, pdMS_TO_TICKS(ms)) == pdTRUE) return true;
  Serial.printf("[LFS] mutex timeout after %u ms\n", ms);
  return false;
}
static void lfsGive(void) { xSemaphoreGive(xLfsMutex); }

// Ensure path starts with '/' for LittleFS compatibility
static String lfsPath(const char* path) {
  String p = path;
  if (p.length() > 0 && p[0] != '/') p = "/" + p;
  return p;
}

// LittleFS.exists() is unreliable on this core — check by opening instead.
static bool lfsExists(const char* path) {
  bool r = false;
  if (lfsTake()) {
    String p = lfsPath(path);
    File f = LittleFS.open(p.c_str(), FILE_READ);
    if (!f && p.length() > 1) f = LittleFS.open(p.substring(1).c_str(), FILE_READ);
    if (f) { r = true; f.close(); }
    lfsGive();
  }
  return r;
}

static bool lfsReadFile(const char* path, String& out) {
  if (!lfsTake()) return false;
  String p = lfsPath(path);
  File f = LittleFS.open(p.c_str(), FILE_READ);
  if (!f && p.length() > 1) f = LittleFS.open(p.substring(1).c_str(), FILE_READ);
  bool ok = (bool)f;
  if (ok) { out = ""; while (f.available()) out += (char)f.read(); f.close(); }
  else Serial.printf("[LFS] read failed: %s\n", path);
  lfsGive();
  return ok;
}

static bool lfsWriteFile(const char* path, const char* content) {
  if (!lfsTake()) return false;
  File f = LittleFS.open(lfsPath(path).c_str(), FILE_WRITE);
  bool ok = false;
  if (f) { f.print(content); f.close(); ok = true; }
  else Serial.printf("[LFS] write failed: %s\n", path);
  lfsGive();
  return ok;
}

static bool lfsRemove(const char* path) {
  bool r = false;
  if (lfsTake()) {
    String p = lfsPath(path);
    r = LittleFS.remove(p.c_str());
    if (!r && p.length() > 1) r = LittleFS.remove(p.substring(1).c_str());
    lfsGive();
  }
  return r;
}

static uint8_t dayNameToBitmask(const char* day) {
  if (strcmp(day, "Sun") == 0) return 0x01;
  if (strcmp(day, "Mon") == 0) return 0x02;
  if (strcmp(day, "Tue") == 0) return 0x04;
  if (strcmp(day, "Wed") == 0) return 0x08;
  if (strcmp(day, "Thu") == 0) return 0x10;
  if (strcmp(day, "Fri") == 0) return 0x20;
  if (strcmp(day, "Sat") == 0) return 0x40;
  return 0;
}

static const char* bitmaskToDayName(uint8_t bit) {
  switch (bit) {
    case 0x01: return "Sun";
    case 0x02: return "Mon";
    case 0x04: return "Tue";
    case 0x08: return "Wed";
    case 0x10: return "Thu";
    case 0x20: return "Fri";
    case 0x40: return "Sat";
  }
  return "";
}

// Accept days as a bitmask integer (e.g. 127 = every day) or a JSON array of day names
static uint8_t parseDays(JsonVariant v) {
  if (v.isNull()) return 0;
  if (v.is<JsonArray>()) {
    uint8_t m = 0;
    for (JsonVariant d : v.as<JsonArray>()) m |= dayNameToBitmask(d.as<const char*>());
    return m;
  }
  return (uint8_t)(v.as<int>() & 0x7F);
}

// Sakamoto algorithm: returns 0=Sunday ... 6=Saturday
static uint8_t computeDow(uint16_t y, uint8_t m, uint8_t d) {
  static const uint8_t t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (uint8_t)((y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7);
}

// ------------------------- Security / AAA Helpers -------------------------
static String md5Hex(const String& input) {
  unsigned char digest[16];
  mbedtls_md5_context ctx;
  mbedtls_md5_init(&ctx);
  mbedtls_md5_starts(&ctx);
  mbedtls_md5_update(&ctx, (const unsigned char*)input.c_str(), input.length());
  mbedtls_md5_finish(&ctx, digest);
  mbedtls_md5_free(&ctx);
  char buf[33];
  for (int i = 0; i < 16; i++) sprintf(buf + i * 2, "%02x", digest[i]);
  buf[32] = '\0';
  return String(buf);
}

/**
 * AAA Audit log -> /security_log.txt
 * Authentication / Authorization / Accounting events
 * CSV: millis,user,role,action,result,details
 */
static void auditLog(const char* user, const char* role, const char* action,
                     const char* result, const char* details) {
  char line[256];
  snprintf(line, sizeof(line), "%lu,%s,%s,%s,%s,%s\n",
           (unsigned long)millis(),
           user ? user : "-",
           role ? role : "-",
           action,
           result,
           details ? details : "-");
  appendFileLine(SECURITY_LOG_FILE, line);
  Serial.printf("[AUDIT] %s", line);
}

static char* newSession(const char* user, const char* role) {
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!sessions[i].active) {
      for (int j = 0; j < 32; j++) {
        uint8_t b = (uint8_t)random(0, 16);
        sessions[i].token[j] = (b < 10) ? ('0' + b) : ('a' + b - 10);
      }
      sessions[i].token[32] = '\0';
      strlcpy(sessions[i].user, user, sizeof(sessions[i].user));
      strlcpy(sessions[i].role, role, sizeof(sessions[i].role));
      sessions[i].expiry = millis() + SESSION_TIMEOUT_MS;
      sessions[i].active = true;
      return sessions[i].token;
    }
  }
  // reuse first slot
  strlcpy(sessions[0].user, user, sizeof(sessions[0].user));
  strlcpy(sessions[0].role, role, sizeof(sessions[0].role));
  sessions[0].expiry = millis() + SESSION_TIMEOUT_MS;
  sessions[0].active = true;
  return sessions[0].token;
}

// returns role through outRole if found; true if valid & not expired
static bool checkSession(const char* token, char* outRole = NULL) {
  if (!token || strlen(token) == 0) return false;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].active && strcmp(sessions[i].token, token) == 0) {
      if ((int32_t)(sessions[i].expiry - millis()) > 0) {
        sessions[i].expiry = millis() + SESSION_TIMEOUT_MS;
        if (outRole) strlcpy(outRole, sessions[i].role, 16);
        return true;
      }
      sessions[i].active = false;
    }
  }
  return false;
}

static bool isAdmin(const char* token) {
  char role[16] = "";
  if (!checkSession(token, role)) return false;
  return strcmp(role, "Admin") == 0;
}

static void invalidateSession(const char* token) {
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].active && strcmp(sessions[i].token, token) == 0) sessions[i].active = false;
  }
}

// ---------- CRUD-managed user authentication (LittleFS /data/users.json) ----------
enum ManagedAuthResult { MA_OK = 0, MA_FAIL = 1, MA_INACTIVE = 2, MA_NOPASSWORD = 3 };

// Verifies `u`/`hash` against users.json.
// Returns MA_OK and fills *outRole on success; otherwise a failure reason.
static int managedUserAuth(const String& u, const String& hash, String* outRole) {
  if (!lfsExists(USERS_FILE)) return MA_FAIL;
  String s;
  if (!lfsReadFile(USERS_FILE, s)) return MA_FAIL;
  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, s) != DeserializationError::Ok) return MA_FAIL;
  JsonArray arr = doc["users"].as<JsonArray>();
  if (arr.isNull()) return MA_FAIL;
  for (JsonObject usr : arr) {
    if (strcmp(usr["name"] | "", u.c_str()) != 0) continue;
    const char* status = usr["status"] | "Active";
    const char* pw     = usr["password"] | "";
    if (outRole) *outRole = String((const char*)(usr["role"] | "Staff"));
    if (strcmp(status, "Active") != 0) return MA_INACTIVE;
    if (strlen(pw) == 0)               return MA_NOPASSWORD; // never allow blank-password login
    if (!hash.equalsIgnoreCase(pw))    return MA_FAIL;
    usr["lastLogin"] = "Just now";
    String out; serializeJson(doc, out);
    lfsWriteFile(USERS_FILE, out.c_str());
    return MA_OK;
  }
  return MA_FAIL;
}

// ------------------------- WiFi STA (Network Sync) -------------------------
static bool loadWiFiConfig(void) {
  if (!lfsExists(WIFI_FILE)) return false;
  String s;
  if (!lfsReadFile(WIFI_FILE, s)) return false;
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, s)) return false;
  strlcpy(staSSID, doc["ssid"] | "", sizeof(staSSID));
  strlcpy(staPass, doc["pass"] | "", sizeof(staPass));
  return strlen(staSSID) > 0;
}

static void saveWiFiConfig(const char* ssid, const char* pass) {
  if (!lfsTake()) return;
  File f = LittleFS.open(WIFI_FILE, FILE_WRITE);
  if (f) {
    DynamicJsonDocument doc(512);
    doc["ssid"] = ssid;
    doc["pass"] = pass;
    serializeJson(doc, f);
    f.close();
  }
  lfsGive();
}

static uint32_t wifiSTAConnectStart = 0;

static void connectWiFiSTA(void) {
  if (strlen(staSSID) == 0) return;
  // Already online — nothing to do (button becomes a no-op while connected)
  if (wifiSTAConnected && WiFi.status() == WL_CONNECTED) return;
  // Start a fresh connection attempt (non-blocking)
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(staSSID, staPass);
  wifiSTAConnecting = true;
  wifiSTAConnected = false;
  wifiSTAConnectStart = millis();
  Serial.printf("[WiFi] Connecting to %s...\n", staSSID);
}

// Connection state machine — call every loop iteration
static void wifiSTAPoll(void) {
  if (strlen(staSSID) == 0) return;
  wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED) {
    if (!wifiSTAConnected) {
      wifiSTAConnected = true;
      wifiSTAConnecting = false;
      ntpSynced = false; // force NTP re-sync after (re)connect
      Serial.printf("[WiFi] Connected to %s, IP: %s\n", staSSID, WiFi.localIP().toString().c_str());
      auditLog("system", "-", "wifi_sta", "connected", staSSID);
    }
    return;
  }
  if (wifiSTAConnected) { // was connected, link dropped
    wifiSTAConnected = false;
    Serial.println("[WiFi] connection lost");
    auditLog("system", "-", "wifi_sta", "lost", staSSID);
  }
  if (wifiSTAConnecting && (millis() - wifiSTAConnectStart) > WIFI_CONNECT_TIMEOUT_MS) {
    wifiSTAConnecting = false;
    Serial.println("[WiFi] STA connection timeout");
    auditLog("system", "-", "wifi_sta", "failed", staSSID);
  }
}

// ------------------------- Redundant Timekeeping -------------------------
// Returns true always (RTC preferred, millis fallback). Fills `now`.
static bool getCurrentTime(DateTime& now) {
  if (rtcOK && xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    now = rtc.now();
    xSemaphoreGive(xI2CMutex);
    return true;
  }
  // Fallback: synthesize from boot anchor + elapsed millis
  uint32_t elapsedSecs = (millis() - bootMillis) / 1000UL;
  now = bootTime + TimeSpan(elapsedSecs);
  return true;
}

static void attemptNtpResync(void) {
  if (!wifiSTAConnected) return; // only sync when STA is connected

  uint32_t interval = ntpSynced ? NTP_RESYNC_MS : 5000UL; // retry every 5s until first sync
  if ((millis() - lastNtpAttempt) < interval) return;

  lastNtpAttempt = millis();
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 1000)) {
    DateTime ntp(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    if (rtcOK && xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      rtc.adjust(ntp);
      xSemaphoreGive(xI2CMutex);
    }
    bootTime = ntp;
    bootMillis = millis();
    ntpSynced = true;
    Serial.println("[OK] NTP sync complete");
    auditLog("system", "-", "ntp_sync", "success", "time synchronized");
  } else {
    Serial.println("[INFO] NTP not reachable; will retry");
    auditLog("system", "-", "ntp_sync", "failed", "no response");
  }
}

static String getTimeString(void) {
  DateTime now;
  getCurrentTime(now);
  char b[32];
  snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d",
           now.year(), now.month(), now.day(),
           now.hour(), now.minute(), now.second());
  return String(b);
}

// ------------------------- Alarms (LittleFS persistence) -------------------------
static uint32_t nextAlarmId = 1;

static void loadAlarmsFromLFS(void) {
  if (!lfsExists(ALARM_FILE)) return;
  String s;
  if (!lfsReadFile(ALARM_FILE, s)) return;

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, s)) return;
  JsonArray arr;
  if (doc.is<JsonArray>()) {
    arr = doc.as<JsonArray>();
  } else {
    arr = doc["alarms"].as<JsonArray>();
    if (arr.isNull()) return;
  }

  if (xSemaphoreTake(xAlarmMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    alarmCount = 0;
    nextAlarmId = 1;
    for (JsonObject o : arr) {
      if (alarmCount >= ALARM_MAX_ENTRIES) break;
      AlarmEntry_t* a = &alarms[alarmCount];
      memset(a, 0, sizeof(AlarmEntry_t));

      a->id = o["id"] | 0;
      if (a->id == 0) a->id = nextAlarmId++;
      if (a->id >= nextAlarmId) nextAlarmId = a->id + 1;

      strlcpy(a->label, o["label"] | "Alarm", sizeof(a->label));
      a->hour   = o["hour"] | 0;
      a->minute = o["minute"] | 0;
      a->enabled = o["enabled"] | false;
      strlcpy(a->audioPath, o["audio"] | "/chime.mp3", sizeof(a->audioPath));

      a->days = parseDays(o["days"]);
      if (a->days == 0) a->days = 0x7F; // rescue legacy/empty alarms to every day

      a->lastFired = 0;
      alarmCount++;
    }
    xSemaphoreGive(xAlarmMutex);
  }
}

static void saveAlarmsToLFS(void) {
  if (!lfsTake()) return;
  File f = LittleFS.open(ALARM_FILE, FILE_WRITE);
  if (f) {
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.to<JsonArray>();
    if (xSemaphoreTake(xAlarmMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      for (int i = 0; i < alarmCount; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["id"] = alarms[i].id;
        o["label"] = alarms[i].label;
        o["hour"] = alarms[i].hour;
        o["minute"] = alarms[i].minute;
        o["enabled"] = alarms[i].enabled;
        o["audio"] = alarms[i].audioPath;
        o["days"] = alarms[i].days;
      }
      xSemaphoreGive(xAlarmMutex);
    }
    serializeJson(doc, f);
    f.close();
  }
  lfsGive();
  alarmsNeedsReload = false;
}

// ------------------------- Audio helpers -------------------------
void createAlarmWav(void) {
  if (!lfsTake()) return;
  const uint32_t sampleRate = 16000;
  const uint32_t durationSec = 3;
  const uint32_t numSamples = sampleRate * durationSec;
  const uint32_t dataSize = numSamples * 2;
  const uint32_t fileSize = 36 + dataSize;

  File f = LittleFS.open("/_alarm880.wav", FILE_WRITE);
  if (!f) { lfsGive(); return; }

  f.write((const uint8_t*)"RIFF", 4);
  uint32_t v = fileSize - 8;
  f.write((const uint8_t*)&v, 4);
  f.write((const uint8_t*)"WAVE", 4);
  f.write((const uint8_t*)"fmt ", 4);
  v = 16; f.write((const uint8_t*)&v, 4);
  uint16_t a = 1; f.write((const uint8_t*)&a, 2);
  a = 1; f.write((const uint8_t*)&a, 2);
  v = sampleRate; f.write((const uint8_t*)&v, 4);
  v = sampleRate * 2; f.write((const uint8_t*)&v, 4);
  a = 2; f.write((const uint8_t*)&a, 2);
  a = 16; f.write((const uint8_t*)&a, 2);
  f.write((const uint8_t*)"data", 4);
  v = dataSize; f.write((const uint8_t*)&v, 4);

  int half = sampleRate / (880 * 2);
  int cnt = 0;
  for (uint32_t i = 0; i < numSamples; i++) {
    int16_t sample = (cnt++ % (half * 2) < half) ? 12000 : -12000;
    f.write((const uint8_t*)&sample, 2);
  }
  f.close();
  lfsGive();
}

// ------------------------- Forward Decl -------------------------
void vWebTask(void*); void vAudioTask(void*); void vSystemEngineTask(void*);
void setupWebServer(void);
bool debounceReadButton(DebounceButton_t*, uint8_t pin);

// ------------------------- Setup -------------------------
void setup() {
  Serial.begin(115200);
  Serial.printf("\n\n=== %s v%s ===\n", FIRMWARE_NAME, FIRMWARE_VERSION);

  randomSeed(analogRead(0) * millis());

  // Create sync primitives first — all FS helpers below rely on them
  xAudioQueue = xQueueCreate(AUDIO_QUEUE_SIZE, sizeof(AudioMsg_t));
  xI2CMutex = xSemaphoreCreateMutex();
  xAlarmMutex = xSemaphoreCreateMutex();
  xSdMutex  = xSemaphoreCreateMutex();
  xLfsMutex = xSemaphoreCreateMutex();

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  rtcOK = rtc.begin();
  if (!rtcOK) Serial.println("[WARN] DS3231 RTC not found.");

  if (rtcOK) {
    bootTime = rtc.now();
    bootMillis = millis();
    Serial.printf("[OK] RTC time: %02d:%02d:%02d\n", bootTime.hour(), bootTime.minute(), bootTime.second());
  } else {
    bootMillis = millis();
    Serial.println("[INFO] Using millis fallback timekeeping");
  }

  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  delay(100);
  customSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  bool sdMounted = false;
  uint32_t speeds[] = { 2000000, 1000000, 400000, 200000 };
  for (int i = 0; i < 4 && !sdMounted; i++) {
    delay(300);
    Serial.printf("[SD] trying %u Hz...\n", speeds[i]);
    sdMounted = SD.begin(SD_CS_PIN, customSPI, speeds[i]);
  }
  if (!sdMounted) {
    Serial.println("[ERROR] SD Card Mount Failed! Audit logs will be unavailable.");
  } else {
    Serial.printf("[OK] SD Card Mounted: %llu MB\n", SD.cardSize() / (1024ULL * 1024ULL));
    if (!SD.exists("/data")) SD.mkdir("/data");
  }

  // Mount LittleFS for internal storage (alarms, wifi config, users, audio files)
  if (!LittleFS.begin(false)) {
    Serial.println("[ERROR] LittleFS mount failed! Formatting...");
    if (LittleFS.begin(true)) {
      Serial.println("[OK] LittleFS formatted and mounted");
    } else {
      Serial.println("[ERROR] LittleFS mount FAILED even after format");
    }
  } else {
    Serial.println("[OK] LittleFS mounted");
    LittleFS.mkdir("/audio"); // no-op if it already exists
    LittleFS.mkdir("/data");
  }

  loadAlarmsFromLFS();

  pinMode(PTT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(CALL_ALERT_PIN, INPUT_PULLUP);
  pinMode(PLAY_BUTTON_PIN, INPUT_PULLUP);

  IPAddress apIP(172, 217, 28, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  if (strlen(AP_PASSWORD) >= 8) WiFi.softAP(apSSID, AP_PASSWORD);
  else                          WiFi.softAP(apSSID);

  dnsServer.start(CAPTIVE_PORTAL_PORT, "*", apIP);

  // Load and connect WiFi STA (for NTP sync)
  loadWiFiConfig();
  if (strlen(staSSID) > 0) {
    connectWiFiSTA();
  }

  // NTP sync is handled by attemptNtpResync() in vWebTask (retries until WiFi is up)
  if (strlen(staSSID) > 0) {
    Serial.println("[INFO] WiFi STA configured; NTP will sync once connected");
  } else {
    Serial.println("[INFO] No WiFi STA configured; using RTC / millis fallback");
    auditLog("system", "-", "ntp_sync", "skipped", "no WiFi credentials");
  }

  audio.setPinout(I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DIN_PIN);
  audio.setVolume(18);

  Serial.printf("[OK] UI assets embedded in flash: %u files\n", (unsigned)UI_ASSET_COUNT);
  setupWebServer();
  server.begin();

  xTaskCreatePinnedToCore(vWebTask, "WebTask", 4096, NULL, 1, &xWebTaskHandle, 1);
  xTaskCreatePinnedToCore(vAudioTask, "AudioTask", 8192, NULL, 5, &xAudioTaskHandle, 1);
  xTaskCreatePinnedToCore(vSystemEngineTask, "SysEngineTask", 4096, NULL, 3, &xSystemEngineTaskHandle, 1);
}

void loop() { vTaskDelay(portMAX_DELAY); }

// ================= FreeRTOS Tasks =================
void vWebTask(void* pvParameters) {
  TickType_t xLast = xTaskGetTickCount();
  static uint32_t lastWifiCheck = 0;
  for (;;) {
    dnsServer.processNextRequest();
    wifiSTAPoll();
    attemptNtpResync();
    // Periodically retry WiFi STA connection if not connected/connecting
    if (strlen(staSSID) > 0 && !wifiSTAConnected && !wifiSTAConnecting && (millis() - lastWifiCheck) > 30000UL) {
      lastWifiCheck = millis();
      connectWiFiSTA();
    }
    vTaskDelayUntil(&xLast, pdMS_TO_TICKS(100));
  }
}

void vAudioTask(void* pvParameters) {
  AudioMsg_t msg;
  bool playing = false;

  for (;;) {
    if (xQueueReceive(xAudioQueue, &msg, 0) == pdTRUE) {
      if (playing) { audio.stopSong(); playing = false; }
      Serial.printf("[AUDIO] Received play request: %s\n", msg.path);
      String cand = lfsPath(msg.path);
      bool have = lfsExists(cand.c_str());
      if (!have && cand.length() > 1) {
        String alt = cand.substring(1);
        if (lfsExists(alt.c_str())) { cand = alt; have = true; }
      }
      if (have) {
        Serial.printf("[AUDIO] File exists, connecting to LittleFS: %s\n", cand.c_str());
        if (audio.connecttoFS(LittleFS, cand.c_str())) {
          playing = true;
          Serial.println("[AUDIO] Playback started");
          auditLog("system", "-", "alarm_audio", "play", cand.c_str());
        } else {
          Serial.println("[AUDIO] connecttoFS FAILED");
        }
      } else {
        Serial.printf("[AUDIO] File NOT found: %s\n", msg.path);
      }
    }
    if (playing) {
      audio.loop();
      if (!audio.isRunning()) playing = false;
    }
    if (alarmRequested) {
      alarmRequested = false;
      audio.stopSong();
      playing = false;
      createAlarmWav();
      if (lfsExists("/_alarm880.wav") && audio.connecttoFS(LittleFS, "/_alarm880.wav")) {
        playing = true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void vSystemEngineTask(void* pvParameters) {
  TickType_t xLast = xTaskGetTickCount();
  DebounceButton_t ptt   = { HIGH, 0, false };
  DebounceButton_t alert = { HIGH, 0, false };
  DebounceButton_t playBtn = { HIGH, 0, false };
  static uint32_t lastSdCheck = 0;
  static uint32_t lastSdRemount = 0;
  static uint8_t sdFailCount = 0;   // consecutive SD health check failures

  for (;;) {
    vTaskDelayUntil(&xLast, pdMS_TO_TICKS(1000));

    // Periodic SD health check + auto-remount if card became unresponsive
    // Check every 60s (was 10s) to reduce SPI bus contention and crash risk
    if ((millis() - lastSdCheck) > 60000UL) {
      lastSdCheck = millis();
      if (!sdHealthCheck()) {
        sdFailCount++;
        Serial.printf("[SD] health check failed (fail #%u)\n", sdFailCount);
        // Stop remounting after 3 consecutive failures — card may be physically unreadable
        if (sdFailCount >= 3) {
          Serial.println("[SD] too many failures, suspending auto-remount");
        } else if ((millis() - lastSdRemount) > 120000UL) {  // cooldown: 2 min (was 30s)
          lastSdRemount = millis();
          Serial.println("[SD] health check failed, re-mounting...");
          sdRemount();
        } else {
          Serial.println("[SD] health check failed, remount cooldown active");
        }
      } else {
        sdFailCount = 0;  // reset on successful check
      }
    }

    if (alarmsNeedsReload) {
      alarmsNeedsReload = false;
      loadAlarmsFromLFS();
    }

    attemptNtpResync();

    DateTime now;
    getCurrentTime(now);

    uint8_t dowBit = (1 << computeDow(now.year(), now.month(), now.day()));
    uint32_t secs = now.hour() * 3600UL + now.minute() * 60UL + now.second();
    uint32_t dayStart = now.unixtime() - secs;

    if (xSemaphoreTake(xAlarmMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      for (int i = 0; i < alarmCount; i++) {
        if (!alarms[i].enabled) continue;
        if (!(alarms[i].days & dowBit)) continue;

        uint32_t alarmSecs = alarms[i].hour * 3600UL + alarms[i].minute * 60UL;
        uint32_t fireKey = dayStart + alarmSecs;

        if (alarms[i].lastFired != fireKey && secs >= alarmSecs) {
          alarms[i].lastFired = fireKey;
          AudioMsg_t m;
          strlcpy(m.path, alarms[i].audioPath, sizeof(m.path));
          xQueueSend(xAudioQueue, &m, 0);
          auditLog("system", "-", "alarm_triggered", "success", alarms[i].label);
          Serial.printf("[ALARM] %s at %02d:%02d\n", alarms[i].label, alarms[i].hour, alarms[i].minute);
        }
      }
      xSemaphoreGive(xAlarmMutex);
    }

    if (debounceReadButton(&alert, CALL_ALERT_PIN)) {
      alarmRequested = true;
      auditLog("system", "-", "emergency_button", "pressed", "call alert");
    }
    if (debounceReadButton(&ptt, PTT_BUTTON_PIN)) {
      pttPressed = true;
      auditLog("system", "-", "ptt_button", "pressed", "push-to-talk");
    }
    if (debounceReadButton(&playBtn, PLAY_BUTTON_PIN)) {
      AudioMsg_t m;
      strlcpy(m.path, playButtonAudioPath, sizeof(m.path));
      xQueueSend(xAudioQueue, &m, 0);
      auditLog("system", "-", "play_button", "pressed", playButtonAudioPath);
      Serial.printf("[PLAY BUTTON] playing %s\n", playButtonAudioPath);
    }

    // Serial command handling (self-test)
    if (Serial.available()) {
      String cmd = Serial.readStringUntil('\n');
      cmd.trim();
      if (cmd.equalsIgnoreCase("test")) {
        Serial.println("=== SELF TEST ===");
        bool sdOk = (SD.cardSize() > 0);
        Serial.printf("SD: %s (cardSize=%llu MB)\n", sdOk ? "PASS" : "FAIL", (unsigned long long)(SD.cardSize() / (1024ULL*1024ULL)));
        Serial.printf("RTC: %s\n", rtcOK ? "PASS" : "FAIL");
        Serial.printf("I2S pins: BCLK=%d LRC=%d DIN=%d\n", I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DIN_PIN);
        Serial.printf("Alarms: %d\n", alarmCount);
        Serial.println("=== END TEST ===");
      } else if (cmd.equalsIgnoreCase("alarms")) {
        Serial.printf("Configured alarms: %d\n", alarmCount);
        for (int i = 0; i < alarmCount; i++) {
          Serial.printf("  id=%u label=%s %02d:%02d days=0x%02X enabled=%d audio=%s\n",
                        alarms[i].id, alarms[i].label, alarms[i].hour, alarms[i].minute,
                        alarms[i].days, alarms[i].enabled, alarms[i].audioPath);
        }
      }
    }
  }
}

bool debounceReadButton(DebounceButton_t* b, uint8_t pin) {
  uint32_t n = millis();
  int r = digitalRead(pin);
  if (r != b->lastState) b->lastDebounceTime = n;
  b->lastState = r;
  if ((n - b->lastDebounceTime) > DEBOUNCE_MS && r == LOW && !b->stablePress) {
    b->stablePress = true;
    return true;
  }
  if (r == HIGH) b->stablePress = false;
  return false;
}

// ================= Web Server Implementation =================
static String getCookieValue(AsyncWebServerRequest* req, const char* name) {
  String cookie = req->header("Cookie");
  String key = String(name) + "=";
  int idx = cookie.indexOf(key);
  if (idx < 0) return "";
  int start = idx + key.length();
  int end = cookie.indexOf(';', start);
  if (end < 0) end = cookie.length();
  return cookie.substring(start, end);
}

static bool authOK(AsyncWebServerRequest* req, char* outRole = NULL) {
  String tok = getCookieValue(req, "session");
  return checkSession(tok.c_str(), outRole);
}

static bool requireAdmin(AsyncWebServerRequest* req) {
  char role[16] = "";
  String tok = getCookieValue(req, "session");
  if (!checkSession(tok.c_str(), role) || strcmp(role, "Admin") != 0) {
    req->send(403, "application/json", "{\"error\":\"admin required\"}");
    return false;
  }
  return true;
}

void setupWebServer(void) {
  // ---------- Login ----------
  static String loginBody;
  server.on("/api/login", HTTP_POST, [](AsyncWebServerRequest* request) {}, NULL,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      if (index == 0) loginBody = "";
      if (loginBody.length() + len < JSON_BODY_MAX_LEN) {
        loginBody += String((const char*)data, len);
      }
      bool bodyComplete = (total > 0 && (size_t)(index + len) == total) ||
                          (total == 0 && loginBody.length() > 0 && loginBody.endsWith("}"));
      if (!bodyComplete) return;

      String remote = request->client()->remoteIP().toString();
      Serial.printf("[LOGIN DEBUG] attempt from %s body=%s\n", remote.c_str(), loginBody.c_str());

      if (millis() < lockoutUntil) {
        auditLog("-", "-", "login", "lockedout", remote.c_str());
        request->send(429, "application/json", "{\"error\":\"locked out\"}");
        loginBody = "";
        return;
      }

      DynamicJsonDocument doc(512);
      if (deserializeJson(doc, loginBody)) {
        auditLog("-", "-", "login", "bad_json", remote.c_str());
        request->send(400, "application/json", "{\"error\":\"bad json\"}");
        loginBody = "";
        return;
      }
      loginBody = "";

      const char* u = doc["username"] | "";
      const char* p = doc["password"] | "";
      String hash = md5Hex(String(p));

      bool ok = false;
      String roleStr = "Staff";
      int managed = MA_FAIL;

      if (strcmp(u, ADMIN_USER) == 0 &&
          (hash.equalsIgnoreCase(ADMIN_PASS_HASH) || strcmp(p, ADMIN_PASS) == 0)) {
        // Built-in superadmin
        ok = true;
        roleStr = "Admin";
      } else {
        // CRUD-managed users from LittleFS /data/users.json
        managed = managedUserAuth(u, hash, &roleStr);
        ok = (managed == MA_OK);
      }

      if (ok) {
        Serial.printf("[LOGIN DEBUG] success user=%s role=%s\n", u, roleStr.c_str());
        char* tok = newSession(u, roleStr.c_str());
        String cookie = String("session=") + tok + "; HttpOnly; Path=/; Max-Age=600";
        String json = String("{\"status\":\"ok\",\"role\":\"") + roleStr + "\"}";
        AsyncWebServerResponse* resp = request->beginResponse(200, "application/json", json);
        resp->addHeader("Set-Cookie", cookie);
        request->send(resp);
        failedAttempts = 0;
        auditLog(u, roleStr.c_str(), "login", "success", remote.c_str());
      } else if (managed == MA_INACTIVE) {
        auditLog(u, "-", "login", "inactive", remote.c_str());
        request->send(403, "application/json", "{\"error\":\"account not active\"}");
      } else if (managed == MA_NOPASSWORD) {
        auditLog(u, "-", "login", "no_password", remote.c_str());
        request->send(403, "application/json", "{\"error\":\"account has no password set\"}");
      } else {
        Serial.printf("[LOGIN DEBUG] failed user=%s\n", u);
        failedAttempts++;
        if (failedAttempts >= MAX_LOGIN_ATTEMPTS) {
          lockoutUntil = millis() + LOCKOUT_MS;
          failedAttempts = 0;
          auditLog(u, "-", "login", "lockout", remote.c_str());
        } else {
          auditLog(u, "-", "login", "failed", remote.c_str());
        }
        request->send(401, "application/json", "{\"error\":\"invalid credentials\"}");
      }
    });

  // ---------- Logout ----------
  server.on("/api/logout", HTTP_POST, [](AsyncWebServerRequest* req) {
    String tok = getCookieValue(req, "session");
    char role[16] = "";
    checkSession(tok.c_str(), role);
    invalidateSession(tok.c_str());
    AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", "{\"status\":\"ok\"}");
    resp->addHeader("Set-Cookie", "session=; HttpOnly; Path=/; Max-Age=0");
    req->send(resp);
    auditLog(tok.c_str(), role, "logout", "success", "session ended");
  });

  // ---------- Telemetry ----------
  server.on("/api/telemetry", HTTP_GET, [](AsyncWebServerRequest* req) {
    char role[16] = "";
    if (!authOK(req, role)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    DateTime now; getCurrentTime(now);
    DynamicJsonDocument doc(512);
    doc["time"] = getTimeString();
    doc["uptime"] = millis() / 1000;
    doc["alarms"] = alarmCount;
    doc["rtc"] = rtcOK;
    doc["role"] = role;
    doc["wifi_connected"] = wifiSTAConnected;
    doc["wifi_ssid"] = staSSID;
    doc["sd_ok"] = sdHealthCheck();
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // ---------- Alarms CRUD ----------
  // GET list
  server.on("/api/alarms", HTTP_GET, [](AsyncWebServerRequest* req) {
    char role[16] = "";
    if (!authOK(req, role)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.to<JsonArray>();
    if (xSemaphoreTake(xAlarmMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      for (int i = 0; i < alarmCount; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["id"] = alarms[i].id;
        o["label"] = alarms[i].label;
        o["hour"] = alarms[i].hour;
        o["minute"] = alarms[i].minute;
        o["enabled"] = alarms[i].enabled;
        o["audio"] = alarms[i].audioPath;
        o["days"] = alarms[i].days;
      }
      xSemaphoreGive(xAlarmMutex);
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // POST create
  static String alarmBody;
  server.on("/api/alarms", HTTP_POST, [](AsyncWebServerRequest* request) {}, NULL,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      char role[16] = "";
      if (!authOK(request, role)) { request->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
      if (index == 0) alarmBody = "";
      if (alarmBody.length() + len < JSON_BODY_MAX_LEN) alarmBody += String((const char*)data, len);
      if (index + len == total) {
        DynamicJsonDocument doc(1024);
        if (deserializeJson(doc, alarmBody)) {
          request->send(400, "application/json", "{\"error\":\"bad json\"}");
          alarmBody = "";
          return;
        }
        if (alarmCount >= ALARM_MAX_ENTRIES) {
          request->send(400, "application/json", "{\"error\":\"alarm limit reached\"}");
          alarmBody = "";
          return;
        }
        AlarmEntry_t a;
        memset(&a, 0, sizeof(a));
        a.id = nextAlarmId++;
        strlcpy(a.label, doc["label"] | "Alarm", sizeof(a.label));
        a.hour = doc["hour"] | 0;
        a.minute = doc["minute"] | 0;
        a.enabled = doc["enabled"] | true;
        strlcpy(a.audioPath, doc["audio"] | "/chime.mp3", sizeof(a.audioPath));
        a.days = parseDays(doc["days"]);
        if (a.days == 0) a.days = 0x7F; // no day selected -> every day
        // Don't immediately ring an alarm for a time that already passed today
        {
          DateTime nowT; getCurrentTime(nowT);
          uint32_t secs = nowT.hour() * 3600UL + nowT.minute() * 60UL + nowT.second();
          uint32_t alarmSecs = a.hour * 3600UL + a.minute * 60UL;
          a.lastFired = (secs >= alarmSecs) ? (nowT.unixtime() - secs + alarmSecs) : 0;
        }

        if (xSemaphoreTake(xAlarmMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
          alarms[alarmCount++] = a;
          xSemaphoreGive(xAlarmMutex);
        }
        saveAlarmsToLFS();
        auditLog(role, role, "alarm_create", "success", a.label);
        request->send(200, "application/json", "{\"status\":\"created\",\"id\":" + String(a.id) + "}");
        alarmBody = "";
      }
    });

  // PUT update by id
  static String alarmUpdateBody;
  server.on("/api/alarms", HTTP_PUT, [](AsyncWebServerRequest* request) {}, NULL,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      char role[16] = "";
      if (!authOK(request, role)) { request->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
      if (index == 0) alarmUpdateBody = "";
      if (alarmUpdateBody.length() + len < JSON_BODY_MAX_LEN) alarmUpdateBody += String((const char*)data, len);
      if (index + len == total) {
        DynamicJsonDocument doc(1024);
        if (deserializeJson(doc, alarmUpdateBody)) {
          request->send(400, "application/json", "{\"error\":\"bad json\"}");
          alarmUpdateBody = "";
          return;
        }
        uint32_t id = doc["id"] | 0;
        bool found = false;
        if (xSemaphoreTake(xAlarmMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
          for (int i = 0; i < alarmCount; i++) {
            if (alarms[i].id == id) {
              uint8_t oldHour = alarms[i].hour, oldMinute = alarms[i].minute, oldDays = alarms[i].days;
              strlcpy(alarms[i].label, doc["label"] | alarms[i].label, sizeof(alarms[i].label));
              alarms[i].hour = doc["hour"] | alarms[i].hour;
              alarms[i].minute = doc["minute"] | alarms[i].minute;
              alarms[i].enabled = doc["enabled"] | alarms[i].enabled;
              strlcpy(alarms[i].audioPath, doc["audio"] | alarms[i].audioPath, sizeof(alarms[i].audioPath));
              JsonVariant dv = doc["days"];
              if (!dv.isNull()) {
                uint8_t d = parseDays(dv);
                alarms[i].days = d ? d : 0x7F; // no day selected -> every day
              }
              // Only re-arm an alarm whose time/days actually changed (toggling
              // enabled or editing the label must not re-fire it)
              if (alarms[i].hour != oldHour || alarms[i].minute != oldMinute || alarms[i].days != oldDays)
                alarms[i].lastFired = 0;
              found = true;
              break;
            }
          }
          xSemaphoreGive(xAlarmMutex);
        }
        alarmUpdateBody = "";
        if (!found) { request->send(404, "application/json", "{\"error\":\"not found\"}"); return; }
        saveAlarmsToLFS();
        auditLog(role, role, "alarm_update", "success", String(id).c_str());
        request->send(200, "application/json", "{\"status\":\"updated\"}");
      }
    });

  // DELETE by id (?id=)
  server.on("/api/alarms", HTTP_DELETE, [](AsyncWebServerRequest* req) {
    char role[16] = "";
    if (!authOK(req, role)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    String idStr = req->arg("id");
    uint32_t id = idStr.toInt();
    bool found = false;
    if (xSemaphoreTake(xAlarmMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      for (int i = 0; i < alarmCount; i++) {
        if (alarms[i].id == id) {
          for (int j = i; j < alarmCount - 1; j++) alarms[j] = alarms[j + 1];
          alarmCount--;
          found = true;
          break;
        }
      }
      xSemaphoreGive(xAlarmMutex);
    }
    if (!found) { req->send(404, "application/json", "{\"error\":\"not found\"}"); return; }
    saveAlarmsToLFS();
    auditLog(role, role, "alarm_delete", "success", idStr.c_str());
    req->send(200, "application/json", "{\"status\":\"deleted\"}");
  });

  // ---------- Users CRUD (Admin only) ----------
  server.on("/api/users", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!requireAdmin(req)) return;
    String out;
    if (lfsExists(USERS_FILE)) {
      if (lfsTake()) {
        File f = LittleFS.open(USERS_FILE, FILE_READ);
        if (f) { while (f.available()) out += (char)f.read(); f.close(); }
        lfsGive();
      }
    } else {
      out = "{\"users\":[]}";
    }
    req->send(200, "application/json", out);
  });

  static String usersBody;
  server.on("/api/users", HTTP_POST, [](AsyncWebServerRequest* request) {}, NULL,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      if (!requireAdmin(request)) return;
      if (index == 0) usersBody = "";
      if (usersBody.length() + len < JSON_BODY_MAX_LEN) usersBody += String((const char*)data, len);
      if (index + len == total) {
        DynamicJsonDocument doc(1024);
        if (deserializeJson(doc, usersBody)) {
          request->send(400, "application/json", "{\"error\":\"bad json\"}");
          usersBody = "";
          return;
        }
        const char* name = doc["name"] | "";
        const char* role = doc["role"] | "Staff";
        const char* status = doc["status"] | "Active";
        const char* pass = doc["password"] | "";
        if (strlen(name) == 0) {
          request->send(400, "application/json", "{\"error\":\"name required\"}");
          usersBody = "";
          return;
        }
        String passHash = strlen(pass) > 0 ? md5Hex(String(pass)) : "";

        DynamicJsonDocument storeDoc(2048);
        if (lfsExists(USERS_FILE)) {
          if (lfsTake()) {
            File f = LittleFS.open(USERS_FILE, FILE_READ);
            if (f) { deserializeJson(storeDoc, f); f.close(); }
            lfsGive();
          }
        }
        if (storeDoc["users"].isNull()) storeDoc["users"].to<JsonArray>();
        JsonArray arr = storeDoc["users"].as<JsonArray>();

        bool found = false;
        for (JsonObject u : arr) {
          if (strcmp(u["name"] | "", name) == 0) {
            u["role"] = role;
            u["status"] = status;
            u["lastLogin"] = doc["lastLogin"] | "Just now";
            if (strlen(pass) > 0) u["password"] = passHash;
            found = true;
            break;
          }
        }
        if (!found) {
          JsonObject u = arr.add<JsonObject>();
          u["name"] = name;
          u["role"] = role;
          u["status"] = status;
          u["password"] = passHash;
          u["lastLogin"] = doc["lastLogin"] | "Just now";
        }

        if (lfsTake()) {
          File f = LittleFS.open(USERS_FILE, FILE_WRITE);
          if (f) { serializeJson(storeDoc, f); f.close(); }
          lfsGive();
        }
        usersBody = "";
        auditLog("admin", "Admin", "user_create_update", "success", name);
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      }
    });

  server.on("/api/users", HTTP_DELETE, [](AsyncWebServerRequest* req) {
    if (!requireAdmin(req)) return;
    String name = req->arg("name");
    if (name == ADMIN_USER) { req->send(403, "application/json", "{\"error\":\"cannot delete admin\"}"); return; }

    DynamicJsonDocument storeDoc(2048);
    if (lfsExists(USERS_FILE)) {
      if (lfsTake()) {
        File f = LittleFS.open(USERS_FILE, FILE_READ);
        if (f) { deserializeJson(storeDoc, f); f.close(); }
        lfsGive();
      }
    }
    bool removed = false;
    if (!storeDoc["users"].isNull()) {
      JsonArray arr = storeDoc["users"].as<JsonArray>();
      for (size_t i = 0; i < arr.size(); i++) {
        if (String(arr[i]["name"].as<const char*>()) == name) {
          arr.remove(i);
          removed = true;
          break;
        }
      }
    }
    if (lfsTake()) {
      File f = LittleFS.open(USERS_FILE, FILE_WRITE);
      if (f) { serializeJson(storeDoc, f); f.close(); }
      lfsGive();
    }
    auditLog("admin", "Admin", "user_delete", removed ? "success" : "not_found", name.c_str());
    req->send(200, "application/json", "{\"status\":\"deleted\"}");
  });

  // ---------- Audio files ----------
  server.on("/api/audio-files", HTTP_GET, [](AsyncWebServerRequest* req) {
    char role[16] = "";
    if (!authOK(req, role)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    DynamicJsonDocument doc(1024);
    JsonArray arr = doc["files"].to<JsonArray>();
    if (lfsTake(200)) {
      File root = LittleFS.open("/");
      if (root && root.isDirectory()) {
        File f = root.openNextFile();
        while (f) {
          if (!f.isDirectory()) {
            String name = String(f.name());
            int slash = name.lastIndexOf('/');
            if (slash >= 0) name = name.substring(slash + 1);
            if (name.length() > 0 && name[0] != '/') name = "/" + name;
            // Only list audio files
            if (name.endsWith(".mp3") || name.endsWith(".wav")) {
              arr.add(name);
            }
          }
          f.close();
          f = root.openNextFile();
        }
        root.close();
      }
      lfsGive();
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // POST /api/upload (multipart) -> root
  server.on("/api/upload", HTTP_POST, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", "{\"status\":\"success\"}");
  }, [](AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
    char role[16] = "";
    if (!authOK(request, role)) return;
    // sanitize filename: reject path separators and ..
    if (filename.indexOf("..") >= 0 || filename.indexOf('/') >= 0 || filename.indexOf('\\') >= 0) {
      Serial.println("[Upload] unsafe filename rejected");
      return;
    }
    bool tookSd = false;
    if (!index) {
      Serial.printf("[Upload] start: %s\n", filename.c_str());
      if (lfsTake()) {
        tookSd = true;
        String filepath = "/" + filename;
        uploadFile = LittleFS.open(filepath.c_str(), FILE_WRITE);
        if (!uploadFile) Serial.println("[Upload] open failed");
      } else {
        Serial.println("[Upload] lfsTake failed");
      }
    }
    if (uploadFile) uploadFile.write(data, len);
    if (final) {
      if (uploadFile) uploadFile.close();
      if (tookSd) lfsGive();
      Serial.printf("[Upload] done: %s\n", filename.c_str());
      if (uploadFile) auditLog(role, role, "audio_upload", "success", filename.c_str());
    }
  });

  // DELETE /api/audio-files?name=xxx
  server.on("/api/audio-files", HTTP_DELETE, [](AsyncWebServerRequest* req) {
    char role[16] = "";
    if (!authOK(req, role)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    String name = req->arg("name");
    if (name.indexOf("..") >= 0 || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) {
      req->send(400, "application/json", "{\"error\":\"unsafe name\"}"); return;
    }
    String filepath = "/" + name;
    bool ok = lfsRemove(filepath.c_str());
    auditLog(role, role, "audio_delete", ok ? "success" : "failed", name.c_str());
    req->send(200, "application/json", ok ? "{\"status\":\"deleted\"}" : "{\"error\":\"not found\"}");
  });

  // ---------- WiFi Network Config (Admin only) ----------
  server.on("/api/wifi", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!requireAdmin(req)) return;
    DynamicJsonDocument doc(512);
    doc["ssid"] = staSSID;
    doc["connected"] = wifiSTAConnected;
    if (wifiSTAConnected) doc["ip"] = WiFi.localIP().toString();
    else doc["ip"] = "";
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  static String wifiBody;
  server.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest* request) {}, NULL,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      if (!requireAdmin(request)) return;
      if (index == 0) wifiBody = "";
      if (wifiBody.length() + len < JSON_BODY_MAX_LEN) wifiBody += String((const char*)data, len);
      if (index + len == total) {
        DynamicJsonDocument doc(512);
        if (deserializeJson(doc, wifiBody)) {
          request->send(400, "application/json", "{\"error\":\"bad json\"}");
          wifiBody = "";
          return;
        }
        const char* ssid = doc["ssid"] | "";
        const char* pass = doc["pass"] | "";
        if (strlen(ssid) == 0) {
          request->send(400, "application/json", "{\"error\":\"ssid required\"}");
          wifiBody = "";
          return;
        }
        strlcpy(staSSID, ssid, sizeof(staSSID));
        strlcpy(staPass, pass, sizeof(staPass));
        saveWiFiConfig(ssid, pass);
        wifiBody = "";
        // Drop the old link and reconnect with the new credentials right away
        wifiSTAConnected = false;
        wifiSTAConnecting = false;
        WiFi.disconnect(false, false);
        connectWiFiSTA();
        auditLog("admin", "Admin", "wifi_config", "success", ssid);
        request->send(200, "application/json", "{\"status\":\"saved\",\"connecting\":true}");
      }
    });

  server.on("/api/wifi/connect", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!requireAdmin(req)) return;
    // Force a fresh connection attempt regardless of stale state
    WiFi.disconnect(false, false);
    wifiSTAConnected = false;
    wifiSTAConnecting = false;
    wifiSTAConnectStart = 0;
    connectWiFiSTA();
    req->send(200, "application/json", "{\"status\":\"connecting\"}");
  });

  server.on("/api/sd/remount", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!requireAdmin(req)) return;
    bool ok = sdRemount();
    auditLog("admin", "Admin", "sd_remount", ok ? "success" : "failed", "manual");
    req->send(200, "application/json", ok ? "{\"status\":\"remounted\"}" : "{\"error\":\"remount failed\"}");
  });

  // ---------- Manual controls ----------
  server.on("/api/manual-ring", HTTP_POST, [](AsyncWebServerRequest* req) {
    char role[16] = "";
    if (!authOK(req, role)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    AudioMsg_t m; strlcpy(m.path, "/chime.mp3", sizeof(m.path));
    xQueueSend(xAudioQueue, &m, 0);
    auditLog(role, role, "manual_ring", "success", "chime");
    req->send(200, "application/json", "{\"status\":\"ringing\"}");
  });

  server.on("/api/emergency", HTTP_POST, [](AsyncWebServerRequest* req) {
    char role[16] = "";
    if (!authOK(req, role)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    alarmRequested = true;
    auditLog(role, role, "emergency", "triggered", "web");
    req->send(200, "application/json", "{\"status\":\"alarm\"}");
  });

  // ---------- Play Button Config ----------
  // GET: return current play-button audio path + available audio files
  server.on("/api/play-button-config", HTTP_GET, [](AsyncWebServerRequest* req) {
    char role[16] = "";
    if (!authOK(req, role)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    DynamicJsonDocument doc(1024);
    doc["audioPath"] = playButtonAudioPath;
    JsonArray arr = doc["files"].to<JsonArray>();
    if (lfsTake(200)) {
      File root = LittleFS.open("/");
      if (root && root.isDirectory()) {
        File f = root.openNextFile();
        while (f) {
          if (!f.isDirectory()) {
            String name = String(f.name());
            int slash = name.lastIndexOf('/');
            if (slash >= 0) name = name.substring(slash + 1);
            if (name.length() > 0 && name[0] != '/') name = "/" + name;
            if (name.endsWith(".mp3") || name.endsWith(".wav")) {
              arr.add(name);
            }
          }
          f.close();
          f = root.openNextFile();
        }
        root.close();
      }
      lfsGive();
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  static String playButtonConfigBody;
  server.on("/api/play-button-config", HTTP_POST, [](AsyncWebServerRequest* request) {}, NULL,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      char role[16] = "";
      if (!authOK(request, role)) { request->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
      if (index == 0) playButtonConfigBody = "";
      if (playButtonConfigBody.length() + len < JSON_BODY_MAX_LEN) playButtonConfigBody += String((const char*)data, len);
      if (index + len == total) {
        DynamicJsonDocument doc(512);
        if (deserializeJson(doc, playButtonConfigBody)) {
          request->send(400, "application/json", "{\"error\":\"bad json\"}");
          playButtonConfigBody = "";
          return;
        }
        const char* audioPath = doc["audioPath"] | "";
        if (strlen(audioPath) == 0) {
          request->send(400, "application/json", "{\"error\":\"audioPath required\"}");
          playButtonConfigBody = "";
          return;
        }
        strlcpy(playButtonAudioPath, audioPath, sizeof(playButtonAudioPath));
        Serial.printf("[PLAY BUTTON CONFIG] Saved path: %s\n", playButtonAudioPath);
        playButtonConfigBody = "";
        auditLog(role, role, "play_button_config", "success", playButtonAudioPath);
        request->send(200, "application/json", "{\"status\":\"ok\",\"audioPath\":\"" + String(playButtonAudioPath) + "\"}");
      }
    });

  // POST /api/play-button - manually trigger play button audio
  server.on("/api/play-button", HTTP_POST, [](AsyncWebServerRequest* req) {
    char role[16] = "";
    if (!authOK(req, role)) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    AudioMsg_t m; strlcpy(m.path, playButtonAudioPath, sizeof(m.path));
    Serial.printf("[PLAY BUTTON] Trigger: path=%s\n", playButtonAudioPath);
    if (xQueueSend(xAudioQueue, &m, 0) == pdTRUE) {
      Serial.println("[PLAY BUTTON] Queued successfully");
    } else {
      Serial.println("[PLAY BUTTON] Queue FULL - failed to queue");
    }
    auditLog(role, role, "play_button", "triggered", playButtonAudioPath);
    req->send(200, "application/json", "{\"status\":\"playing\",\"audioPath\":\"" + String(playButtonAudioPath) + "\"}");
  });

  // ---------- Audit logs (Admin only) ----------
  server.on("/api/audit-logs", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!requireAdmin(req)) return;
    String out;
    if (sdExists(SECURITY_LOG_FILE)) {
      if (sdTake()) {
        File f = SD.open(SECURITY_LOG_FILE, FILE_READ);
        if (f) { while (f.available()) out += (char)f.read(); f.close(); }
        sdGive();
      }
    }
    req->send(200, "text/plain", out);
  });

  // ---------- Default users file ----------
  if (!lfsExists(USERS_FILE)) {
    if (lfsTake()) {
      File uf = LittleFS.open(USERS_FILE, FILE_WRITE);
      if (uf) {
        uf.print("{\"users\":["
          "{\"name\":\"admin\",\"role\":\"Admin\",\"status\":\"Active\",\"password\":\"0192023a7bbd73250516f069df18b500\",\"lastLogin\":\"Never\"},"
          "{\"name\":\"staff1\",\"role\":\"Staff\",\"status\":\"Active\",\"password\":\"\",\"lastLogin\":\"Never\"}"
          "]}");
        uf.close();
      }
      lfsGive();
    }
  }

  // ---------- Captive portal detection endpoints ----------
  server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest* r) {
    AsyncWebServerResponse* resp = r->beginResponse_P(200, "text/html", UI_INDEX_HTML, sizeof(UI_INDEX_HTML));
    r->send(resp);
  });
  server.on("/generate204", HTTP_GET, [](AsyncWebServerRequest* r) {
    AsyncWebServerResponse* resp = r->beginResponse_P(200, "text/html", UI_INDEX_HTML, sizeof(UI_INDEX_HTML));
    r->send(resp);
  });
  server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest* r) {
    AsyncWebServerResponse* resp = r->beginResponse_P(200, "text/html", UI_INDEX_HTML, sizeof(UI_INDEX_HTML));
    r->send(resp);
  });

  // ---------- Catch-all: serve embedded UI ----------
  server.onNotFound([](AsyncWebServerRequest* r) {
    String uri = r->url();
    if (uri.indexOf('?') >= 0) uri = uri.substring(0, uri.indexOf('?'));
    if (uri.endsWith("/")) uri += "index.html";
    if (uri == "/") uri = "/index.html";

    for (size_t i = 0; i < UI_ASSET_COUNT; i++) {
      if (uri.equalsIgnoreCase(UI_ASSETS[i].path)) {
        AsyncWebServerResponse* resp = r->beginResponse_P(200, UI_ASSETS[i].contentType, UI_ASSETS[i].data, UI_ASSETS[i].size);
        resp->addHeader("Cache-Control", "public, max-age=3600");
        r->send(resp);
        return;
      }
    }

    if (uri.equalsIgnoreCase("/index.html")) {
      r->send(200, "text/html", "<!DOCTYPE html><html><body><h1>UI not available</h1></body></html>");
      return;
    }
    r->send(404, "text/plain", "Not found");
  });
}
