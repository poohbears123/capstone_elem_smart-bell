#include <Arduino.h>
#include <esp_heap_caps.h>
#include "ui_embed.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <SPI.h>
#include <SD.h>
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
SemaphoreHandle_t xI2CMutex = NULL, xAlarmMutex = NULL, xSdMutex = NULL;

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

// ------------------------- SD Helpers -------------------------
static bool sdTake(uint32_t ms = 100) {
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
  File f = SD.open(path, FILE_APPEND);
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
  // Try to take the mutex (longer wait since SD may be stuck)
  if (!sdTake(500)) {
    Serial.println("[SD] remount: could not take mutex");
    return false;
  }
  delay(100);
  customSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  uint32_t speeds[] = { 4000000, 2000000, 1000000, 400000, 200000 };
  for (int i = 0; i < 5; i++) {
    delay(300);
    Serial.printf("[SD] remount trying %u Hz...\n", speeds[i]);
    if (SD.begin(SD_CS_PIN, customSPI, speeds[i])) {
      Serial.println("[SD] re-mounted OK");
      if (!SD.exists("/audio")) SD.mkdir("/audio");
      if (!SD.exists("/data")) SD.mkdir("/data");
      sdGive();
      return true;
    }
  }
  Serial.println("[SD] re-mount FAILED");
  sdGive();
  return false;
}

static bool sdHealthCheck(void) {
  // Quick check: try to stat a known file
  return sdExists("/data");
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

// ------------------------- WiFi STA (Network Sync) -------------------------
static bool loadWiFiConfig(void) {
  if (!sdExists(WIFI_FILE)) return false;
  String s;
  if (!sdReadFile(WIFI_FILE, s)) return false;
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, s)) return false;
  strlcpy(staSSID, doc["ssid"] | "", sizeof(staSSID));
  strlcpy(staPass, doc["pass"] | "", sizeof(staPass));
  return strlen(staSSID) > 0;
}

static void saveWiFiConfig(const char* ssid, const char* pass) {
  if (!sdTake()) return;
  File f = SD.open(WIFI_FILE, FILE_WRITE);
  if (f) {
    DynamicJsonDocument doc(512);
    doc["ssid"] = ssid;
    doc["pass"] = pass;
    serializeJson(doc, f);
    f.close();
  }
  sdGive();
}

static uint32_t wifiSTAConnectStart = 0;

static void connectWiFiSTA(void) {
  if (strlen(staSSID) == 0) return;
  if (wifiSTAConnected) return;
  if (wifiSTAConnecting) {
    // Already connecting, check if it's done
    if (WiFi.status() == WL_CONNECTED) {
      wifiSTAConnected = true;
      wifiSTAConnecting = false;
      Serial.printf("[WiFi] Connected to %s, IP: %s\n", staSSID, WiFi.localIP().toString().c_str());
      auditLog("system", "-", "wifi_sta", "connected", staSSID);
    } else if ((millis() - wifiSTAConnectStart) > WIFI_CONNECT_TIMEOUT_MS) {
      wifiSTAConnecting = false;
      Serial.println("[WiFi] STA connection timeout");
      auditLog("system", "-", "wifi_sta", "failed", staSSID);
    }
    return;
  }
  // Start connection (non-blocking)
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(staSSID, staPass);
  wifiSTAConnecting = true;
  wifiSTAConnectStart = millis();
  Serial.printf("[WiFi] Connecting to %s...\n", staSSID);
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

// ------------------------- Alarms (SD persistence) -------------------------
static uint32_t nextAlarmId = 1;

static void loadAlarmsFromSD(void) {
  if (!sdExists(ALARM_FILE)) return;
  String s;
  if (!sdReadFile(ALARM_FILE, s)) return;

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
      strlcpy(a->audioPath, o["audio"] | "/audio/chime.mp3", sizeof(a->audioPath));

      a->days = 0;
      JsonArray da = o["days"].as<JsonArray>();
      if (!da.isNull())
        for (JsonVariant v : da) a->days |= dayNameToBitmask(v.as<const char*>());

      a->lastFired = 0;
      alarmCount++;
    }
    xSemaphoreGive(xAlarmMutex);
  }
}

static void saveAlarmsToSD(void) {
  if (!sdTake()) return;
  File f = SD.open(ALARM_FILE, FILE_WRITE);
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
        JsonArray da = o.createNestedArray("days");
        for (int b = 0; b < 7; b++) {
          if (alarms[i].days & (1 << b)) da.add(bitmaskToDayName(1 << b));
        }
      }
      xSemaphoreGive(xAlarmMutex);
    }
    serializeJson(doc, f);
    f.close();
  }
  sdGive();
  alarmsNeedsReload = false;
}

// ------------------------- Audio helpers -------------------------
void createAlarmWav(void) {
  if (!sdTake()) return;
  const uint32_t sampleRate = 16000;
  const uint32_t durationSec = 3;
  const uint32_t numSamples = sampleRate * durationSec;
  const uint32_t dataSize = numSamples * 2;
  const uint32_t fileSize = 36 + dataSize;

  File f = SD.open("/audio/_alarm880.wav", FILE_WRITE);
  if (!f) { sdGive(); return; }

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
  sdGive();
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
  uint32_t speeds[] = { 4000000, 2000000, 1000000, 400000, 200000 };
  for (int i = 0; i < 5 && !sdMounted; i++) {
    delay(300);
    Serial.printf("[SD] trying %u Hz...\n", speeds[i]);
    sdMounted = SD.begin(SD_CS_PIN, customSPI, speeds[i]);
  }
  if (!sdMounted) {
    Serial.println("[ERROR] SD Card Mount Failed! Will retry in system task.");
  } else {
    Serial.printf("[OK] SD Card Mounted: %llu MB\n", SD.cardSize() / (1024ULL * 1024ULL));
    if (!SD.exists("/audio")) SD.mkdir("/audio");
    if (!SD.exists("/data")) SD.mkdir("/data");
  }

  xAudioQueue = xQueueCreate(AUDIO_QUEUE_SIZE, sizeof(AudioMsg_t));
  xI2CMutex = xSemaphoreCreateMutex();
  xAlarmMutex = xSemaphoreCreateMutex();
  xSdMutex  = xSemaphoreCreateMutex();

  loadAlarmsFromSD();

  pinMode(PTT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(CALL_ALERT_PIN, INPUT_PULLUP);

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
    attemptNtpResync();
    // Periodically retry WiFi STA connection if not connected
    if (strlen(staSSID) > 0 && !wifiSTAConnected && (millis() - lastWifiCheck) > 30000UL) {
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
      if (sdExists(msg.path) && audio.connecttoFS(SD, msg.path)) {
        playing = true;
        auditLog("system", "-", "alarm_audio", "play", msg.path);
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
      if (sdExists("/audio/_alarm880.wav") && audio.connecttoFS(SD, "/audio/_alarm880.wav")) {
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
  static uint32_t lastSdCheck = 0;
  static uint32_t lastSdRemount = 0;

  for (;;) {
    vTaskDelayUntil(&xLast, pdMS_TO_TICKS(1000));

    // Periodic SD health check + auto-remount if card became unresponsive
    if ((millis() - lastSdCheck) > 10000UL) {
      lastSdCheck = millis();
      if (!sdHealthCheck()) {
        // Only remount if at least 30 seconds since last remount attempt
        if ((millis() - lastSdRemount) > 30000UL) {
          lastSdRemount = millis();
          Serial.println("[SD] health check failed, re-mounting...");
          sdRemount();
        } else {
          Serial.println("[SD] health check failed, remount cooldown active");
        }
      }
    }
      }
    }

    if (alarmsNeedsReload) {
      alarmsNeedsReload = false;
      loadAlarmsFromSD();
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
      bool ok = (strcmp(u, ADMIN_USER) == 0) &&
                (hash.equalsIgnoreCase(ADMIN_PASS_HASH) || strcmp(p, ADMIN_PASS) == 0);

      if (ok) {
        char* tok = newSession(u, "Admin");
        String cookie = String("session=") + tok + "; HttpOnly; Path=/; Max-Age=600";
        AsyncWebServerResponse* resp = request->beginResponse(200, "application/json", "{\"status\":\"ok\",\"role\":\"Admin\"}");
        resp->addHeader("Set-Cookie", cookie);
        request->send(resp);
        failedAttempts = 0;
        auditLog(u, "Admin", "login", "success", remote.c_str());
      } else {
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
        JsonArray da = o.createNestedArray("days");
        for (int b = 0; b < 7; b++) {
          if (alarms[i].days & (1 << b)) da.add(bitmaskToDayName(1 << b));
        }
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
        strlcpy(a.audioPath, doc["audio"] | "/audio/chime.mp3", sizeof(a.audioPath));
        a.days = 0;
        JsonArray da = doc["days"].as<JsonArray>();
        if (!da.isNull())
          for (JsonVariant v : da) a.days |= dayNameToBitmask(v.as<const char*>());
        a.lastFired = 0;

        if (xSemaphoreTake(xAlarmMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
          alarms[alarmCount++] = a;
          xSemaphoreGive(xAlarmMutex);
        }
        saveAlarmsToSD();
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
              strlcpy(alarms[i].label, doc["label"] | alarms[i].label, sizeof(alarms[i].label));
              alarms[i].hour = doc["hour"] | alarms[i].hour;
              alarms[i].minute = doc["minute"] | alarms[i].minute;
              alarms[i].enabled = doc["enabled"] | alarms[i].enabled;
              strlcpy(alarms[i].audioPath, doc["audio"] | alarms[i].audioPath, sizeof(alarms[i].audioPath));
              alarms[i].days = 0;
              JsonArray da = doc["days"].as<JsonArray>();
              if (!da.isNull())
                for (JsonVariant v : da) alarms[i].days |= dayNameToBitmask(v.as<const char*>());
              alarms[i].lastFired = 0;
              found = true;
              break;
            }
          }
          xSemaphoreGive(xAlarmMutex);
        }
        alarmUpdateBody = "";
        if (!found) { request->send(404, "application/json", "{\"error\":\"not found\"}"); return; }
        saveAlarmsToSD();
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
    saveAlarmsToSD();
    auditLog(role, role, "alarm_delete", "success", idStr.c_str());
    req->send(200, "application/json", "{\"status\":\"deleted\"}");
  });

  // ---------- Users CRUD (Admin only) ----------
  server.on("/api/users", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!requireAdmin(req)) return;
    String out;
    if (sdExists(USERS_FILE)) {
      if (sdTake()) {
        File f = SD.open(USERS_FILE, FILE_READ);
        if (f) { while (f.available()) out += (char)f.read(); f.close(); }
        sdGive();
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
        if (sdExists(USERS_FILE)) {
          if (sdTake()) {
            File f = SD.open(USERS_FILE, FILE_READ);
            if (f) { deserializeJson(storeDoc, f); f.close(); }
            sdGive();
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

        if (sdTake()) {
          File f = SD.open(USERS_FILE, FILE_WRITE);
          if (f) { serializeJson(storeDoc, f); f.close(); }
          sdGive();
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
    if (sdExists(USERS_FILE)) {
      if (sdTake()) {
        File f = SD.open(USERS_FILE, FILE_READ);
        if (f) { deserializeJson(storeDoc, f); f.close(); }
        sdGive();
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
    if (sdTake()) {
      File f = SD.open(USERS_FILE, FILE_WRITE);
      if (f) { serializeJson(storeDoc, f); f.close(); }
      sdGive();
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
    if (sdTake(200)) {
      File root = SD.open("/audio");
      if (root && root.isDirectory()) {
        File f = root.openNextFile();
        while (f) {
          if (!f.isDirectory()) {
            String name = String(f.name());
            int slash = name.lastIndexOf('/');
            if (slash >= 0) name = name.substring(slash + 1);
            arr.add(name);
          }
          f.close();
          f = root.openNextFile();
        }
        root.close();
      }
      sdGive();
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // POST /api/upload (multipart) -> /audio/
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
      if (sdTake()) {
        tookSd = true;
        String filepath = "/audio/" + filename;
        uploadFile = SD.open(filepath.c_str(), FILE_WRITE);
        if (!uploadFile) Serial.println("[Upload] open failed");
      } else {
        Serial.println("[Upload] sdTake failed");
      }
    }
    if (uploadFile) uploadFile.write(data, len);
    if (final) {
      if (uploadFile) uploadFile.close();
      if (tookSd) sdGive();
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
    String filepath = "/audio/" + name;
    bool ok = sdExists(filepath.c_str()) && SD.remove(filepath.c_str());
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
        auditLog("admin", "Admin", "wifi_config", "success", ssid);
        request->send(200, "application/json", "{\"status\":\"saved\"}");
      }
    });

  server.on("/api/wifi/connect", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!requireAdmin(req)) return;
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
    AudioMsg_t m; strlcpy(m.path, "/audio/chime.mp3", sizeof(m.path));
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
  if (!sdExists(USERS_FILE)) {
    if (sdTake()) {
      File uf = SD.open(USERS_FILE, FILE_WRITE);
      if (uf) {
        uf.print("{\"users\":["
          "{\"name\":\"admin\",\"role\":\"Admin\",\"status\":\"Active\",\"password\":\"0192023a7bbd73250516f069df18b500\",\"lastLogin\":\"Never\"},"
          "{\"name\":\"staff1\",\"role\":\"Staff\",\"status\":\"Active\",\"password\":\"\",\"lastLogin\":\"Never\"}"
          "]}");
        uf.close();
      }
      sdGive();
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
