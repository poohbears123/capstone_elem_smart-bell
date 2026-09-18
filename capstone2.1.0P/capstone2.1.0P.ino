#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <RTClib.h>
#include <ArduinoJson.h>
#include "Audio.h"
#include <esp_wifi.h>
#include <SPIFFS.h>

#define FIRMWARE_VERSION "2.1.0"
#define FIRMWARE_NAME    "FCU Smart Bell FreeRTOS"

// ------------------------- Pins -------------------------
#define SD_CS_PIN       5
#define SD_SCK_PIN      18
#define SD_MISO_PIN     19
#define SD_MOSI_PIN     23
#define I2S_LRC_PIN     27
#define I2S_BCLK_PIN    26
#define I2S_DIN_PIN     25
#define I2C_SDA_PIN     21
#define I2C_SCL_PIN     22
#define PTT_BUTTON_PIN  14
#define CALL_ALERT_PIN  13

// ------------------------- Constants -------------------------
#define SCHEDULE_MAX_ENTRIES  20
#define MAC_ADDR_STR_LEN      18
#define CSV_LINE_MAX_LEN      128
#define MAX_SSID_LEN          32
#define AUDIO_PATH_MAX_LEN    64
#define DEBOUNCE_MS           50
#define SNIFFER_QUEUE_SIZE    20
#define AUDIO_QUEUE_SIZE      5
#define SCHEDULE_FILE         "/data/schedule.json"
#define LOG_FILE              "/data/logs.csv"
#define ATTENDANCE_DEDUP_MS   300000UL
#define CAPTIVE_PORTAL_PORT   53
#define DEDUP_CACHE_SIZE      50
#define JSON_BODY_MAX_LEN     4096

// WPA2 requires 8+ chars. CHANGE THIS BEFORE DEPLOYING.
const char* AP_PASSWORD = "ChangeMe123!";

// ------------------------- Types -------------------------
typedef struct {
  char mac[MAC_ADDR_STR_LEN];
  int16_t rssi;
  uint32_t timestamp;
} LogEntry_t;

typedef struct { char path[AUDIO_PATH_MAX_LEN]; bool isEmergency; } AudioMsg_t;

typedef struct {
  uint8_t id; char subject[64]; uint8_t days;
  uint8_t startHour, startMin, startSec;
  uint8_t endHour, endMin, endSec;
  char chimeStart[AUDIO_PATH_MAX_LEN];
  char chimeEnd[AUDIO_PATH_MAX_LEN];
} ScheduleEntry_t;

typedef struct {
  int lastState;
  uint32_t lastDebounceTime;
  bool stablePress;
} DebounceButton_t;

// ------------------------- Globals -------------------------
TaskHandle_t xWebTaskHandle = NULL, xSnifferTaskHandle = NULL,
             xAudioTaskHandle = NULL, xSystemEngineTaskHandle = NULL;
QueueHandle_t xLogQueue = NULL, xAudioQueue = NULL;
SemaphoreHandle_t xI2CMutex = NULL, xScheduleMutex = NULL, xSdMutex = NULL;

RTC_DS3231 rtc;
AsyncWebServer server(80);
DNSServer dnsServer;
Audio audio;

char apSSID[MAX_SSID_LEN] = "FCU_Secure_Bell";
char staSSID[MAX_SSID_LEN] = "";
char staPass[64] = "";

ScheduleEntry_t scheduleEntries[SCHEDULE_MAX_ENTRIES];
int scheduleCount = 0;
volatile bool scheduleNeedsReload = false;
bool snifferActive = false;
volatile bool attendanceActive = false;

// FIX #2: per-entry "last fired" key = day-start epoch + trigger second
uint32_t lastFiredStart[SCHEDULE_MAX_ENTRIES];
uint32_t lastFiredEnd[SCHEDULE_MAX_ENTRIES];

bool rtcOK = false;

// ------------------------- Helpers -------------------------
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

// FIX #6: SD access guard (SPI/SD driver is not thread-safe).
static bool sdTake(uint32_t ms = 100) { return xSemaphoreTake(xSdMutex, pdMS_TO_TICKS(ms)) == pdTRUE; }
static void sdGive(void) { xSemaphoreGive(xSdMutex); }

static bool sdExists(const char* path) {
  bool r = false;
  if (sdTake()) { r = SD.exists(path); sdGive(); }
  return r;
}

static void appendCSVLine(const char* line) {
  if (!sdTake()) return;
  File f = SD.open(LOG_FILE, FILE_APPEND);
  if (f) { f.print(line); f.close(); }
  sdGive();
}

static bool sdReadFile(const char* path, String& out) {
  if (!sdTake()) return false;
  File f = SD.open(path, FILE_READ);
  bool ok = (bool)f;
  if (ok) { out = ""; while (f.available()) out += (char)f.read(); f.close(); }
  sdGive();
  return ok;
}

// ------------------------- Forward declarations -------------------------
void vWebTask(void*); void vSnifferTask(void*); void vAudioTask(void*); void vSystemEngineTask(void*);
void loadScheduleFromSD(void);
void saveScheduleToSD(const JsonDocument&);
void wifiSnifferCallback(void*, wifi_promiscuous_pkt_type_t);
void enableSniffer(bool enable);
bool debounceReadButton(DebounceButton_t*, uint8_t pin);
void setupWebServer(void);

// ------------------------- Setup -------------------------
void setup() {
  Serial.begin(115200);
  Serial.printf("\n\n=== %s v%s ===\n", FIRMWARE_NAME, FIRMWARE_VERSION);
  Serial.println("Booting...");

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // FIX #9: initialize RTC exactly once
  rtcOK = rtc.begin();
  if (!rtcOK) Serial.println("[WARN] DS3231 RTC not found.");
  else {
    if (rtc.lostPower()) {
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
      Serial.println("[WARN] RTC lost power - set to compile time.");
    }
    Serial.println("[OK] RTC.");
  }

  if (!SPIFFS.begin(true)) Serial.println("[WARN] SPIFFS.");
  else Serial.println("[OK] SPIFFS.");

  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN)) Serial.println("[WARN] SD Card.");
  else Serial.printf("[OK] SD: %llu MB\n", SD.cardSize() / (1024ULL * 1024ULL));

  xLogQueue = xQueueCreate(SNIFFER_QUEUE_SIZE, sizeof(LogEntry_t));
  xAudioQueue = xQueueCreate(AUDIO_QUEUE_SIZE, sizeof(AudioMsg_t));
  xI2CMutex = xSemaphoreCreateMutex();
  xScheduleMutex = xSemaphoreCreateMutex();
  xSdMutex = xSemaphoreCreateMutex();
  Serial.println("[OK] Queues/mutexes.");

  memset(lastFiredStart, 0, sizeof(lastFiredStart));
  memset(lastFiredEnd, 0, sizeof(lastFiredEnd));

  loadScheduleFromSD();
  Serial.printf("[Schedule] %d entries\n", scheduleCount);

  pinMode(PTT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(CALL_ALERT_PIN, INPUT_PULLUP);
  Serial.println("[OK] Buttons.");

  // --- WiFi AP + DNS ---
  WiFi.mode(WIFI_AP);
  IPAddress apIP(172, 217, 28, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  // FIX #7: secured AP (password must be 8+ chars for WPA2)
  if (strlen(AP_PASSWORD) >= 8) WiFi.softAP(apSSID, AP_PASSWORD);
  else                          WiFi.softAP(apSSID);
  Serial.printf("[OK] AP: %s @ 172.217.28.1 (%s)\n",
                apSSID, strlen(AP_PASSWORD) >= 8 ? "WPA2" : "OPEN");

  if (strlen(staSSID) > 0) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(staSSID, staPass);
    Serial.printf("[OK] Station: %s\n", staSSID);
  }

  dnsServer.start(CAPTIVE_PORTAL_PORT, "*", apIP);
  Serial.println("[OK] DNS.");

  audio.setPinout(I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DIN_PIN);
  audio.setVolume(18);
  Serial.println("[OK] Audio.");

  setupWebServer();
  server.begin();
  Serial.println("[OK] HTTP server started on port 80.");

  xTaskCreatePinnedToCore(vWebTask, "WebTask",       4096, NULL, 1, &xWebTaskHandle,       1);
  xTaskCreatePinnedToCore(vSnifferTask, "SnifferTask",  4096, NULL, 2, &xSnifferTaskHandle,  0);
  xTaskCreatePinnedToCore(vAudioTask, "AudioTask",    8192, NULL, 5, &xAudioTaskHandle,    1);
  xTaskCreatePinnedToCore(vSystemEngineTask, "SysEngineTask", 4096, NULL, 3, &xSystemEngineTaskHandle, 1);
  Serial.println("[OK] Tasks created.\n");
  Serial.println("=== System Ready ===\n");
}

void loop() { vTaskDelay(portMAX_DELAY); }

// ================= CORE 0 =================
// Attendance sniffer. Promiscuous mode breaks the AP, so it is enabled ONLY
// during active attendance windows (see enableSniffer calls in the engine).
void enableSniffer(bool enable) {
  if (enable == snifferActive) return;
  if (enable) {
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&wifiSnifferCallback);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  } else {
    esp_wifi_set_promiscuous(false);
  }
  snifferActive = enable;
  Serial.printf("[Sniffer] %s\n", enable ? "ON" : "OFF");
}

void vSnifferTask(void* pvParameters) {
  Serial.println("[SnifferTask] Core 0 (promiscuous OFF by default)");
  snifferActive = false;

  LogEntry_t e;
  struct { char m[MAC_ADDR_STR_LEN]; uint32_t ls; } dedup[DEDUP_CACHE_SIZE];
  int dedupCnt = 0;

  for (;;) {
    if (attendanceActive && snifferActive) {
      if (xQueueReceive(xLogQueue, &e, pdMS_TO_TICKS(100)) == pdTRUE) {
        uint32_t n = millis() / 1000UL;
        bool dup = false;
        for (int i = 0; i < dedupCnt; i++) {
          if (strcmp(dedup[i].m, e.mac) == 0) {
            if ((n - dedup[i].ls) < (ATTENDANCE_DEDUP_MS / 1000UL)) dup = true;
            else dedup[i].ls = n;
            break;
          }
        }
        if (!dup) {
          if (dedupCnt < DEDUP_CACHE_SIZE) {
            strcpy(dedup[dedupCnt].m, e.mac); dedup[dedupCnt].ls = n; dedupCnt++;
          } else {
            static int ri = 0;
            strcpy(dedup[ri].m, e.mac); dedup[ri].ls = n; ri = (ri + 1) % DEDUP_CACHE_SIZE;
          }

          // FIX #4: real RTC timestamp (I2C read happens here, never in ISR)
          char timeBuf[24] = "N/A";
          if (rtcOK && xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            DateTime now = rtc.now();
            snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02d %02d:%02d:%02d",
                     now.year(), now.month(), now.day(),
                     now.hour(), now.minute(), now.second());
            xSemaphoreGive(xI2CMutex);
          }

          char line[CSV_LINE_MAX_LEN];
          snprintf(line, sizeof(line), "%s,%s,%d,PRESENT\n", timeBuf, e.mac, e.rssi);
          appendCSVLine(line);
        }
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(250));
    }
  }
}

// ================= CORE 1 =================
void vWebTask(void* pvParameters) {
  Serial.println("[WebTask] Core 1 (DNS)...");
  TickType_t xLast = xTaskGetTickCount();
  for (;;) {
    dnsServer.processNextRequest();
    vTaskDelayUntil(&xLast, pdMS_TO_TICKS(50));
  }
}

void vAudioTask(void* pvParameters) {
  Serial.println("[AudioTask] Core 1.");
  AudioMsg_t msg;
  bool playing = false;

  for (;;) {
    if (xQueueReceive(xAudioQueue, &msg, 0) == pdTRUE) {
      if (playing) { audio.stopSong(); playing = false; }
      if (sdExists(msg.path) && audio.connecttoFS(SD, msg.path)) {
        playing = true;
      } else if (msg.isEmergency) {
        Serial.printf("[Audio] WARN missing file: %s\n", msg.path);
      }
    }
    if (playing) {
      audio.loop();
      if (!audio.isRunning()) playing = false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void vSystemEngineTask(void* pvParameters) {
  Serial.println("[SysEngineTask] Core 1.");
  TickType_t xLast = xTaskGetTickCount();
  DebounceButton_t ptt   = { HIGH, 0, false };
  DebounceButton_t alert = { HIGH, 0, false };

  for (;;) {
    vTaskDelayUntil(&xLast, pdMS_TO_TICKS(1000));

    // FIX #3: reload BEFORE taking the read lock (no nested mutex take)
    if (scheduleNeedsReload) {
      scheduleNeedsReload = false;
      loadScheduleFromSD();
    }

    bool timeOK = false;
    DateTime now(2026, 1, 1, 0, 0, 0);
    if (rtcOK && xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      now = rtc.now();
      timeOK = true;
      xSemaphoreGive(xI2CMutex);
    }

    if (timeOK && scheduleCount > 0) {
      // FIX #1: dayOfTheWeek() returns 0=Sun..6=Sat, which already matches
      // the bitmask; no remapping needed (old code shifted by one day)
      uint8_t dowBit = (1 << now.dayOfTheWeek());
      uint32_t secs = now.hour() * 3600UL + now.minute() * 60UL + now.second();
      uint32_t dayStart = now.unixtime() - secs;

      if (xSemaphoreTake(xScheduleMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        for (int i = 0; i < scheduleCount; i++) {
          if (!(scheduleEntries[i].days & dowBit)) continue;
          uint32_t ss = scheduleEntries[i].startHour * 3600UL
                      + scheduleEntries[i].startMin * 60UL
                      + scheduleEntries[i].startSec;
          uint32_t es = scheduleEntries[i].endHour * 3600UL
                      + scheduleEntries[i].endMin * 60UL
                      + scheduleEntries[i].endSec;

          // FIX #2: fire exactly once per day per entry
          if (secs == ss && lastFiredStart[i] != dayStart + ss) {
            lastFiredStart[i] = dayStart + ss;
            AudioMsg_t m;
            strlcpy(m.path, scheduleEntries[i].chimeStart, sizeof(m.path));
            m.isEmergency = false;
            xQueueSend(xAudioQueue, &m, 0);
            attendanceActive = true;
            enableSniffer(true);      // sniff during this window only
          }
          if (secs == es && lastFiredEnd[i] != dayStart + es) {
            lastFiredEnd[i] = dayStart + es;
            AudioMsg_t m;
            strlcpy(m.path, scheduleEntries[i].chimeEnd, sizeof(m.path));
            m.isEmergency = false;
            xQueueSend(xAudioQueue, &m, 0);
            attendanceActive = false;
            enableSniffer(false);     // restore normal AP operation
          }
        }
        xSemaphoreGive(xScheduleMutex);
      }
    }

    // FIX #5: edge-detected buttons - alert fires once per press
    if (debounceReadButton(&alert, CALL_ALERT_PIN)) {
      AudioMsg_t m;
      strlcpy(m.path, "/audio/alert.mp3", sizeof(m.path));
      m.isEmergency = true;
      xQueueSend(xAudioQueue, &m, 0);
      Serial.println("[SYS] Alert!");
    }
    // PTT button: read + consume (hook your live-PA feature here)
    debounceReadButton(&ptt, PTT_BUTTON_PIN);
  }
}

// FIX #5: returns true exactly once per press (rising edge of stable LOW)
bool debounceReadButton(DebounceButton_t* b, uint8_t pin) {
  uint32_t n = millis();
  int r = digitalRead(pin);
  if (r != b->lastState) b->lastDebounceTime = n;
  b->lastState = r;
  if ((n - b->lastDebounceTime) > DEBOUNCE_MS && r == LOW && !b->stablePress) {
    b->stablePress = true;
    return true;                     // press edge
  }
  if (r == HIGH) b->stablePress = false;   // released -> re-arm
  return false;
}

// ================= Schedule / storage =================
void loadScheduleFromSD(void) {
  if (!sdExists(SCHEDULE_FILE)) {
    if (sdTake()) {
      File f = SD.open(SCHEDULE_FILE, FILE_WRITE);
      if (f) { f.print("{\"schedules\":[]}"); f.close(); }
      sdGive();
    }
    return;
  }
  String s;
  if (!sdReadFile(SCHEDULE_FILE, s)) return;

  JsonDocument d;
  if (deserializeJson(d, s)) return;
  JsonArray a = d["schedules"].as<JsonArray>();
  if (a.isNull()) return;

  if (xSemaphoreTake(xScheduleMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    scheduleCount = 0;
    for (JsonObject o : a) {
      if (scheduleCount >= SCHEDULE_MAX_ENTRIES) break;
      ScheduleEntry_t* e = &scheduleEntries[scheduleCount];
      memset(e, 0, sizeof(ScheduleEntry_t));

      e->id = o["id"] | 0;
      strlcpy(e->subject, o["subject"] | "Untitled", sizeof(e->subject));

      e->days = 0;
      JsonArray da = o["days"].as<JsonArray>();
      if (!da.isNull())
        for (JsonVariant v : da) e->days |= dayNameToBitmask(v.as<const char*>());

      sscanf(o["startTime"] | "08:00", "%hhu:%hhu", &e->startHour, &e->startMin);
      sscanf(o["endTime"]   | "17:00", "%hhu:%hhu", &e->endHour,   &e->endMin);

      strlcpy(e->chimeStart, o["chimeStart"] | "/audio/chime.mp3", sizeof(e->chimeStart));
      strlcpy(e->chimeEnd,   o["chimeEnd"]   | "/audio/chime.mp3", sizeof(e->chimeEnd));

      lastFiredStart[scheduleCount] = 0;   // FIX #2: reset fired keys on reload
      lastFiredEnd[scheduleCount]   = 0;
      scheduleCount++;
    }
    xSemaphoreGive(xScheduleMutex);
  }
  Serial.printf("[Schedule] loaded %d entries\n", scheduleCount);
}

// FIX #10: only set the flag; SysEngineTask performs the reload.
// (Prevents double-load and keeps SD writes out of the async_tcp task.)
void saveScheduleToSD(const JsonDocument& d) {
  if (!sdTake()) return;
  File f = SD.open(SCHEDULE_FILE, FILE_WRITE);
  if (f) { serializeJson(d, f); f.close(); }
  sdGive();
  scheduleNeedsReload = true;
}

// ================= WiFi sniffer (promiscuous, ISR context) =================
// Keep this short: no Serial, no SD, no I2C here.
void wifiSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t t) {
  if (!attendanceActive || !snifferActive || t != WIFI_PKT_MGMT) return;
  wifi_promiscuous_pkt_t* p = (wifi_promiscuous_pkt_t*)buf;
  if (((p->payload[0] >> 4) & 0x0F) != 4) return;   // probe-req frames only
  uint8_t* m = &p->payload[10];
  LogEntry_t e;
  snprintf(e.mac, sizeof(e.mac), "%02X:%02X:%02X:%02X:%02X:%02X",
           m[0], m[1], m[2], m[3], m[4], m[5]);
  e.rssi = p->rx_ctrl.rssi;
  e.timestamp = millis() / 1000UL;
  BaseType_t w = pdFALSE;
  xQueueSendFromISR(xLogQueue, &e, &w);
  if (w) portYIELD_FROM_ISR();
}

// ================= Web server =================
static const char* SUCCESS_PAGE =
  "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";

// FIX #8: accumulate chunked JSON bodies safely.
// ESPAsyncWebServer calls the upload handler once per chunk; we buffer until
// the final chunk (index + len == total), then parse and respond.
static String sBodyBuffer;

void handleSchedulePost(AsyncWebServerRequest* r, uint8_t* data, size_t len,
                        size_t index, size_t total) {
  if (index == 0) {
    sBodyBuffer = "";
    if (total > JSON_BODY_MAX_LEN) {
      r->send(413, "application/json", "{\"error\":\"Body too large\"}");
      return;
    }
    sBodyBuffer.reserve(total);
  }
  sBodyBuffer += String((const char*)data, len);
  if (index + len < total) return;          // wait for more chunks

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, sBodyBuffer);
  sBodyBuffer = "";
  if (err || !doc["schedules"].is<JsonArray>()) {
    r->send(400, "application/json",
            "{\"error\":\"Invalid JSON or missing schedules\"}");
    return;
  }
  saveScheduleToSD(doc);                    // sets reload flag only
  r->send(200, "application/json", "{\"status\":\"ok\"}");
}

void setupWebServer(void) {
  // --- Captive-portal detection endpoints ---
  server.on("/hotspot-detect.html",      HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/html", SUCCESS_PAGE); });
  server.on("/library/test/success.html", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/html", SUCCESS_PAGE); });
  server.on("/success.html",             HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/html", SUCCESS_PAGE); });
  server.on("/generate_204",             HTTP_GET, [](AsyncWebServerRequest* r) { r->send(204, "text/plain", ""); });
  server.on("/ncsi.txt",                 HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/plain", "Microsoft NCSI"); });
  server.on("/connecttest.txt",          HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/plain", "Microsoft Connect Test"); });

  // --- Static UI (SPIFFS; SD fallback handled in "/" route) ---
  server.serveStatic("/ui/", SPIFFS, "/ui/")
        .setDefaultFile("index.html")
        .setCacheControl("max-age=600");

  // --- Root ---
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    Serial.printf("[HTTP] GET / from %s\n", r->client()->remoteIP().toString().c_str());
    if (sdExists("/ui/index.html")) { r->send(SD, "/ui/index.html", "text/html"); return; }
    if (SPIFFS.exists("/ui/index.html")) { r->send(SPIFFS, "/ui/index.html", "text/html"); return; }
    r->send(200, "text/html",
      "<html><head><meta charset='UTF-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>FCU Smart Bell</title>"
      "<style>body{font-family:Arial;text-align:center;padding:40px;"
      "background:#0b2c75;color:white;}</style></head><body>"
      "<h1>FCU Smart Bell System</h1><p>Server running</p>"
      "<p><a href='/test' style='color:#ffcc00;'>Test</a></p>"
      "</body></html>");
  });

  server.on("/test", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "text/plain", "FCU Smart Bell OK!");
  });

  // --- Attendance CSV ---
  server.on("/api/attendance", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!sdExists(LOG_FILE)) {
      r->send(200, "text/csv", "Timestamp,MAC_Address,RSSI,Status\n");
      return;
    }
    String data;
    if (sdReadFile(LOG_FILE, data)) {
      auto resp = r->beginResponseStream("text/csv");
      resp->addHeader("Cache-Control", "no-cache");
      resp->write((const uint8_t*)data.c_str(), data.length());
      r->send(resp);
    } else {
      r->send(500, "text/csv", "read error\n");
    }
  });

  // --- Schedule GET ---
  server.on("/api/schedule", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!sdExists(SCHEDULE_FILE)) { r->send(200, "application/json", "{\"schedules\":[]}"); return; }
    r->send(SD, SCHEDULE_FILE, "application/json");
  });

  // --- Schedule POST (chunk-safe, FIX #8) ---
  server.on("/api/schedule", HTTP_POST,
    [](AsyncWebServerRequest* r) {},
    NULL,
    [](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t i, size_t t) {
      handleSchedulePost(r, d, l, i, t);
    });

  // --- Telemetry ---
  server.on("/api/telemetry", HTTP_GET, [](AsyncWebServerRequest* r) {
    char timeBuf[24] = "N/A";
    if (rtcOK && xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      DateTime n = rtc.now();
      snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02d %02d:%02d:%02d",
               n.year(), n.month(), n.day(),
               n.hour(), n.minute(), n.second());
      xSemaphoreGive(xI2CMutex);
    }
    String j = "{\"time\":\"" + String(timeBuf) + "\"";
    j += ",\"status\":\"" + String(WiFi.status() == WL_CONNECTED ? "Connected" : "AP Only") + "\"";
    j += ",\"ssid\":\"" + String(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "(none)") + "\"";
    j += ",\"ap_ssid\":\"" + String(apSSID) + "\"";
    j += ",\"attendance\":" + String(attendanceActive ? "true" : "false");
    j += ",\"sniffer\":" + String(snifferActive ? "true" : "false");
    j += ",\"schedule_count\":" + String(scheduleCount);
    j += "}";
    r->send(200, "application/json", j);
  });

  server.on("/api/attendance/status", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json",
            "{\"active\":" + String(attendanceActive ? "true" : "false") + "}");
  });

  server.onNotFound([](AsyncWebServerRequest* r) { r->redirect("/"); });
}