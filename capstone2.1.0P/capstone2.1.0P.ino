#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <RTClib.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <esp_wifi.h>
#include <time.h>
#include <SPIFFS.h>
#include "mbedtls/md.h"
#include <AsyncTCP.h>

#define FIRMWARE_VERSION "2.2.0"
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

// Web-UI admin credentials. CHANGE THESE BEFORE DEPLOYING.
#define ADMIN_USER "admin"
#define ADMIN_PASS "admin123"

// Cookie-session auth (deterministic per-client token, no RAM store).
#define COOKIE_NAME "FCU_SESS"
#define MAX_LOGIN_FAILS 5
#define LOGIN_LOCK_MS   60000UL
static uint8_t gLoginFails = 0;
static uint32_t gLockUntil = 0;

// Network config persisted on SPIFFS (managed via GET/POST /api/config).
#define NETCONFIG_FILE "/config.json"

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

// WAV/I2S backend (WROOM-safe: 4KB streaming, no 720KB MP3 buffer).
#define I2S_PORT ((i2s_port_t)0)
#define WAV_CHUNK 4096
#define AUDIO_UPLOAD_MAX (300UL * 1024UL)  // 300KB ~= ~9s at 16kHz mono 16-bit
static int gBellVolume = 18;  // 0-21, matches old Audio.setVolume scale
static bool gI2SReady = false;

// Web audio upload state (multipart streaming straight to SD, no big RAM buffer).
static volatile bool sUploading = false;
static File sUpFile;
static String sUpPath = "";
static size_t sUpSize = 0;
static bool sUpAuth = false;
static bool sUpTooBig = false;

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

// NTP -> RTC sync state (Asia/Manila UTC+8, no DST).
#define NTP_TZ_OFFSET_SEC 28800
static char gLastNtpSync[24] = "never";
static bool gNtpOK = false;
static uint32_t sLastNtpAttempt = 0;
#define NTP_RETRY_MS (6UL * 3600UL * 1000UL)  // auto retry every 6h when STA up
#define NTP_TIMEOUT_MS 5000

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
void setupI2S(void);
bool syncRTCfromNTP(uint32_t timeoutMs);
void loadScheduleFromSD(void);
void saveScheduleToSD(const JsonDocument&);
void wifiSnifferCallback(void*, wifi_promiscuous_pkt_type_t);
void enableSniffer(bool enable);
bool debounceReadButton(DebounceButton_t*, uint8_t pin);
void setupWebServer(void);
String authTokenFor(AsyncWebServerRequest* r);
bool isAuthed(AsyncWebServerRequest* r);
bool guard(AsyncWebServerRequest* r);
void loadNetConfig(void);
bool saveNetConfig(const char* ap, const char* ssid, const char* pass);

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

  loadNetConfig();
  Serial.printf("[OK] NetConfig: AP=%s STA=%s\n", apSSID, staSSID);

  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  bool sdOK = SD.begin(SD_CS_PIN, SPI, 8000000);
  if (!sdOK) {
    Serial.println("[SD] retry at 1MHz...");
    SPI.setFrequency(1000000);
    sdOK = SD.begin(SD_CS_PIN, SPI, 1000000);
  }
  if (!sdOK) {
    Serial.printf("[WARN] SD Card. cardType=%d\n", SD.cardType());
  } else {
    Serial.printf("[OK] SD: %llu MB type=%d\n", SD.cardSize() / (1024ULL * 1024ULL), SD.cardType());
    const char* dirs[] = { "/", "/ui", "/audio", "/data" };
    for (int d = 0; d < 4; d++) {
      File root = SD.open(dirs[d]);
      if (!root) { Serial.printf("[SD] %s: OPEN FAIL\n", dirs[d]); continue; }
      for (File f = root.openNextFile(); f; f = root.openNextFile())
        Serial.printf("[SD] %s/%s %d\n", dirs[d], f.name(), (int)f.size());
      root.close();
    }
    Serial.printf("[SD] /ui/index.html %s\n", SD.exists("/ui/index.html") ? "FOUND" : "MISSING");
    Serial.printf("[SD] /ui/login.html %s\n", SD.exists("/ui/login.html") ? "FOUND" : "MISSING");
    SPI.setFrequency(8000000);
  }

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

  setupI2S();
  Serial.printf("[Audio] I2S MAX98357A BCLK=%d LRC=%d DIN=%d vol=%d freeHeap:%u\n",
                I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DIN_PIN, gBellVolume,
                (unsigned)ESP.getFreeHeap());
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

void setupI2S(void) {
  i2s_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = 16000;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = 512;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  i2s_pin_config_t pins;
  memset(&pins, 0, sizeof(pins));
  pins.bck_io_num = I2S_BCLK_PIN;
  pins.ws_io_num = I2S_LRC_PIN;
  pins.data_out_num = I2S_DIN_PIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) == ESP_OK) {
    i2s_set_pin(I2S_PORT, &pins);
    gI2SReady = true;
  } else {
    Serial.println("[Audio] WARN i2s_driver_install failed");
  }
}

// Map legacy .mp3 schedule paths to .wav (WROOM has no MP3 decoder).
static void wavPathFor(const char* in, char* out, size_t n) {
  strlcpy(out, in, n);
  size_t L = strlen(out);
  if (L > 4 && strcasecmp(out + L - 4, ".mp3") == 0) {
    strlcpy(out + L - 4, ".wav", n - (L - 4));
  }
}

// Blocking WAV stream, 4KB chunks. Returns after file done or new msg arrived.
static void playWavFile(const char* sdPath) {
  if (!gI2SReady) return;
  // Defer playback while a web upload is writing to SD (shared SPI bus).
  for (int w = 0; sUploading && w < 300; w++) vTaskDelay(pdMS_TO_TICKS(100));
  if (!sdTake()) return;
  File f = SD.open(sdPath, FILE_READ);
  if (!f) { sdGive(); Serial.printf("[Audio] WARN missing file: %s\n", sdPath); return; }
  // Parse 44-byte RIFF header for 16-bit PCM mono/stereo.
  uint8_t hdr[44];
  uint32_t sampleRate = 16000;
  uint16_t channels = 1, bits = 16;
  bool hdrOK = false;
  if (f.size() > 44 && f.read(hdr, 44) == 44 &&
      memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 8, "WAVE", 4) == 0) {
    channels = hdr[22] | (hdr[23] << 8);
    sampleRate = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
    bits = hdr[34] | (hdr[35] << 8);
    if (bits == 16 && (channels == 1 || channels == 2) &&
        sampleRate >= 8000 && sampleRate <= 48000) hdrOK = true;
  }
  if (!hdrOK) {
    // Raw 16-bit mono 16k fallback: rewind and play as-is.
    f.seek(0);
    sampleRate = 16000; channels = 1;
  }
  sdGive();  // release during I2S reconfig (short)
  i2s_set_clk(I2S_PORT, sampleRate, I2S_BITS_PER_SAMPLE_16BIT,
              channels == 2 ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO);
  i2s_zero_dma_buffer(I2S_PORT);

  float vol = gBellVolume <= 0 ? 0 : (gBellVolume >= 21 ? 1.0f : gBellVolume / 21.0f);
  static uint8_t buf[WAV_CHUNK];
  AudioMsg_t pending;
  bool aborted = false;
  while (true) {
    // Abort if a newer chime was queued.
    if (xQueueReceive(xAudioQueue, &pending, 0) == pdTRUE) {
      // Re-queue so outer loop picks it up next.
      xQueueSendToFront(xAudioQueue, &pending, 0);
      aborted = true;
      break;
    }
    size_t n = 0;
    if (sdTake()) { n = f.read(buf, sizeof(buf)); sdGive(); }
    else break;
    if (n == 0) break;
    // Software volume on int16 samples.
    if (vol < 0.999f) {
      int16_t* s = (int16_t*)buf;
      size_t cnt = n / 2;
      for (size_t i = 0; i < cnt; i++) s[i] = (int16_t)(s[i] * vol);
    }
    size_t written = 0;
    uint8_t* p = buf;
    size_t left = n;
    while (left > 0) {
      size_t w = 0;
      if (i2s_write(I2S_PORT, p, left, &w, portMAX_DELAY) != ESP_OK) break;
      left -= w; p += w; written += w;
    }
    if (written == 0) break;
  }
  f.close();
  i2s_zero_dma_buffer(I2S_PORT);
  Serial.printf("[Audio] done %s (%s)\n", sdPath, aborted ? "aborted" : "ok");
}

void vAudioTask(void* pvParameters) {
  Serial.println("[AudioTask] Core 1 (WAV).");
  AudioMsg_t msg;
  char wav[AUDIO_PATH_MAX_LEN];

  for (;;) {
    if (xQueueReceive(xAudioQueue, &msg, portMAX_DELAY) == pdTRUE) {
      wavPathFor(msg.path, wav, sizeof(wav));
      if (!sdExists(wav)) {
        Serial.printf("[Audio] WARN missing file: %s\n", wav);
      } else {
        Serial.printf("[Audio] play %s\n", wav);
        playWavFile(wav);
      }
    }
  }
}

// NTP -> DS3231. Call only when STA is up. Blocks up to timeoutMs.
bool syncRTCfromNTP(uint32_t timeoutMs) {
  if (!rtcOK) { Serial.println("[NTP] no RTC"); return false; }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[NTP] STA not connected");
    return false;
  }
  configTime(NTP_TZ_OFFSET_SEC, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
  struct tm tm;
  uint32_t t0 = millis();
  bool got = false;
  while (millis() - t0 < timeoutMs) {
    if (getLocalTime(&tm, 50)) { got = true; break; }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (!got) {
    Serial.println("[NTP] timeout");
    return false;
  }
  DateTime dt((uint16_t)(tm.tm_year + 1900), (uint8_t)(tm.tm_mon + 1),
              (uint8_t)tm.tm_mday, (uint8_t)tm.tm_hour,
              (uint8_t)tm.tm_min, (uint8_t)tm.tm_sec);
  if (xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    rtc.adjust(dt);
    xSemaphoreGive(xI2CMutex);
  } else {
    Serial.println("[NTP] I2C busy");
    return false;
  }
  snprintf(gLastNtpSync, sizeof(gLastNtpSync), "%04d-%02d-%02d %02d:%02d:%02d",
           dt.year(), dt.month(), dt.day(), dt.hour(), dt.minute(), dt.second());
  gNtpOK = true;
  Serial.printf("[NTP] RTC synced: %s\n", gLastNtpSync);
  return true;
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

    // NTP auto-sync: when STA is up, retry every 6h (first try 30s after boot).
    {
      uint32_t n = millis();
      static bool firstNtpDone = false;
      uint32_t due = firstNtpDone ? NTP_RETRY_MS : 30000UL;
      if (WiFi.status() == WL_CONNECTED && (n - sLastNtpAttempt) >= due) {
        sLastNtpAttempt = n;
        firstNtpDone = true;
        if (syncRTCfromNTP(NTP_TIMEOUT_MS)) {
          memset(lastFiredStart, 0, sizeof(lastFiredStart));
          memset(lastFiredEnd, 0, sizeof(lastFiredEnd));
        }
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

// ================= Auth (cookie session) =================
// Token = SHA1(user:pass:clientIP), verified per request. No RAM store,
// so nothing to invalidate server-side; logout expires the browser cookie.
static String sha1Hex(const String& payload) {
  uint8_t out[20];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char*)payload.c_str(), payload.length());
  mbedtls_md_finish(&ctx, out);
  mbedtls_md_free(&ctx);
  String s = "";
  s.reserve(40);
  for (int i = 0; i < 20; i++) {
    if (out[i] < 16) s += "0";
    s += String(out[i], HEX);
  }
  return s;
}

String authTokenFor(AsyncWebServerRequest* r) {
  return sha1Hex(String(ADMIN_USER) + ":" + String(ADMIN_PASS) + ":"
                 + r->client()->remoteIP().toString());
}

bool isAuthed(AsyncWebServerRequest* r) {
  if (!r->hasHeader("Cookie")) return false;
  String c = r->header("Cookie");
  return c.indexOf(String(COOKIE_NAME) + "=" + authTokenFor(r)) != -1;
}

// Guard for protected API routes: sends 401 JSON when unauthenticated.
bool guard(AsyncWebServerRequest* r) {
  if (isAuthed(r)) return true;
  r->send(401, "application/json", "{\"error\":\"unauthorized\"}");
  return false;
}

// ================= Network config (SPIFFS) =================
void loadNetConfig(void) {
  if (!SPIFFS.exists(NETCONFIG_FILE)) return;
  File f = SPIFFS.open(NETCONFIG_FILE, FILE_READ);
  if (!f) return;
  JsonDocument d;
  DeserializationError e = deserializeJson(d, f);
  f.close();
  if (e) return;
  const char* ap = d["ap_ssid"];
  const char* ss = d["sta_ssid"];
  const char* pw = d["sta_pass"];
  if (ap && strlen(ap) > 0 && strlen(ap) < MAX_SSID_LEN)
    strlcpy(apSSID, ap, sizeof(apSSID));
  if (ss && strlen(ss) < MAX_SSID_LEN)
    strlcpy(staSSID, ss, sizeof(staSSID));
  if (pw && strlen(pw) < 64)
    strlcpy(staPass, pw, sizeof(staPass));
}

bool saveNetConfig(const char* ap, const char* ssid, const char* pass) {
  JsonDocument d;
  d["ap_ssid"] = ap;
  d["sta_ssid"] = ssid;
  d["sta_pass"] = pass;
  File f = SPIFFS.open(NETCONFIG_FILE, FILE_WRITE);
  if (!f) return false;
  serializeJson(d, f);
  f.close();
  return true;
}

// Chunk-safe POST /api/config (mirrors handleSchedulePost).
static String sCfgBuffer;
static bool sCfgSkip = false;

void handleConfigPost(AsyncWebServerRequest* r, uint8_t* data, size_t len,
                      size_t index, size_t total) {
  if (index == 0) {
    sCfgSkip = !isAuthed(r);
    if (sCfgSkip) {
      r->send(401, "application/json", "{\"error\":\"unauthorized\"}");
      return;
    }
    sCfgBuffer = "";
    if (total > JSON_BODY_MAX_LEN) {
      sCfgSkip = true;
      r->send(413, "application/json", "{\"error\":\"Body too large\"}");
      return;
    }
    sCfgBuffer.reserve(total);
  }
  if (sCfgSkip) return;
  sCfgBuffer += String((const char*)data, len);
  if (index + len < total) return;          // wait for more chunks

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, sCfgBuffer);
  sCfgBuffer = "";
  if (err) {
    r->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  const char* ap = doc["ap_ssid"] | apSSID;
  const char* ss = doc["sta_ssid"] | staSSID;
  const char* pw = doc["sta_pass"] | staPass;
  if (!ap || strlen(ap) == 0 || strlen(ap) >= MAX_SSID_LEN ||
      !ss || strlen(ss) >= MAX_SSID_LEN ||
      !pw || (strlen(pw) > 0 && (strlen(pw) < 8 || strlen(pw) > 63))) {
    r->send(400, "application/json",
            "{\"error\":\"Bad values (ap 1-31 chars, sta_ssid 0-31, sta_pass empty or 8-63)\"}");
    return;
  }
  if (!saveNetConfig(ap, ss, pw)) {
    r->send(500, "application/json", "{\"error\":\"save failed\"}");
    return;
  }
  strlcpy(apSSID, ap, sizeof(apSSID));
  strlcpy(staSSID, ss, sizeof(staSSID));
  strlcpy(staPass, pw, sizeof(staPass));

  // Live-apply: reconfigure AP SSID and (re)connect station.
  if (strlen(AP_PASSWORD) >= 8) WiFi.softAP(apSSID, AP_PASSWORD);
  else                           WiFi.softAP(apSSID);
  if (strlen(staSSID) > 0) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(staSSID, staPass);
  }
  Serial.printf("[HTTP] netconfig saved AP=%s STA=%s\n", apSSID, staSSID);
  r->send(200, "application/json", "{\"status\":\"ok\"}");
}

// ================= Web server =================
static const char* SUCCESS_PAGE =
  "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";

// FIX #8: accumulate chunked JSON bodies safely.
// ESPAsyncWebServer calls the upload handler once per chunk; we buffer until
// the final chunk (index + len == total), then parse and respond.
static String sBodyBuffer;
static bool sSchedSkip = false;

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

  // --- Static UI: SD first, then SPIFFS. Root-relative aliases because
  // "/" serves /ui/index.html content, so "style.css" resolves to "/style.css".
  // NOTE: single serveStatic only (duplicate URI aborts). Aliases use r->send.
  server.serveStatic("/ui/", SD, "/ui/").setCacheControl("max-age=600");
  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (sdExists("/ui/script.js")) { r->send(SD, "/ui/script.js", "application/javascript"); return; }
    if (SPIFFS.exists("/ui/script.js")) { r->send(SPIFFS, "/ui/script.js", "application/javascript"); return; }
    r->send(404, "text/plain", "not found");
  });
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (sdExists("/ui/style.css")) { r->send(SD, "/ui/style.css", "text/css"); return; }
    if (SPIFFS.exists("/ui/style.css")) { r->send(SPIFFS, "/ui/style.css", "text/css"); return; }
    r->send(404, "text/plain", "not found");
  });
  server.on("/FCU.jpg", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (sdExists("/ui/FCU.jpg")) { r->send(SD, "/ui/FCU.jpg", "image/jpeg"); return; }
    if (SPIFFS.exists("/ui/FCU.jpg")) { r->send(SPIFFS, "/ui/FCU.jpg", "image/jpeg"); return; }
    r->send(404, "text/plain", "not found");
  });
  server.on("/building.jpg", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (sdExists("/ui/building.jpg")) { r->send(SD, "/ui/building.jpg", "image/jpeg"); return; }
    if (SPIFFS.exists("/ui/building.jpg")) { r->send(SPIFFS, "/ui/building.jpg", "image/jpeg"); return; }
    r->send(404, "text/plain", "not found");
  });
  server.on("/pfp.jpg", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (sdExists("/ui/pfp.jpg")) { r->send(SD, "/ui/pfp.jpg", "image/jpeg"); return; }
    if (SPIFFS.exists("/ui/pfp.jpg")) { r->send(SPIFFS, "/ui/pfp.jpg", "image/jpeg"); return; }
    r->send(404, "text/plain", "not found");
  });

  // --- Root: app when authed, login page when not ---
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    Serial.printf("[HTTP] GET / from %s (%s)\n",
                  r->client()->remoteIP().toString().c_str(),
                  isAuthed(r) ? "authed" : "guest");
    const char* page = isAuthed(r) ? "/ui/index.html" : "/ui/login.html";
    if (sdExists(page)) { Serial.printf("[HTTP] / -> SD:%s\n", page); r->send(SD, page, "text/html"); return; }
    if (SPIFFS.exists(page)) { Serial.printf("[HTTP] / -> SPIFFS:%s\n", page); r->send(SPIFFS, page, "text/html"); return; }
    Serial.printf("[HTTP] / -> FALLBACK (missing %s)\n", page);
    if (isAuthed(r)) {
      r->send(200, "text/html",
        "<html><head><meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>FCU Smart Bell</title>"
        "<style>body{font-family:Arial;text-align:center;padding:40px;"
        "background:#0b2c75;color:white;}</style></head><body>"
        "<h1>FCU Smart Bell System</h1><p>Server running</p>"
        "<p><a href='/test' style='color:#ffcc00;'>Test</a></p>"
        "</body></html>");
      return;
    }
    // Inline login fallback (used only when /ui/login.html is not deployed).
    r->send(200, "text/html",
      "<html><head><meta charset='UTF-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>FCU Smart Bell - Login</title>"
      "<style>body{font-family:Arial;background:#0b2c75;color:#fff;text-align:center;"
      "padding:40px;}form{display:inline-block;background:#f5f5f5;color:#222;padding:30px;"
      "border-radius:6px;}input{display:block;width:240px;margin:10px auto;padding:12px;}"
      "button{background:#13278f;color:#fff;border:0;padding:12px 24px;cursor:pointer;}"
      "#m{color:#b00020;min-height:20px;}</style></head><body>"
      "<h2>FCU SCHOOL BELL SYSTEM</h2>"
      "<form id='f'><input name='username' placeholder='Username'>"
      "<input name='password' type='password' placeholder='Password'>"
      "<button>SIGN IN</button><p id='m'></p></form>"
      "<script>document.getElementById('f').addEventListener('submit',async(e)=>{"
      "e.preventDefault();var f=e.target;"
      "var r=await fetch('/handleLogin',{method:'POST',"
      "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
      "body:'username='+encodeURIComponent(f.username.value)+"
      "'&password='+encodeURIComponent(f.password.value)});"
      "if(r.ok){location.href='/';}else{"
      "document.getElementById('m').textContent=(r.status===423)?"
      "'Locked, try later.':'Invalid credentials.';}});</script>"
      "</body></html>");
  });

  server.on("/test", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "text/plain", "FCU Smart Bell OK!");
  });

  // --- Attendance CSV ---
  server.on("/api/attendance", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!guard(r)) return;
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
    if (!guard(r)) return;
    if (!sdExists(SCHEDULE_FILE)) { r->send(200, "application/json", "{\"schedules\":[]}"); return; }
    r->send(SD, SCHEDULE_FILE, "application/json");
  });

  // --- Schedule POST (chunk-safe, FIX #8) ---
  server.on("/api/schedule", HTTP_POST,
    [](AsyncWebServerRequest* r) {},
    NULL,
    [](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t i, size_t t) {
      if (i == 0) {
        sSchedSkip = !isAuthed(r);
        if (sSchedSkip)
          r->send(401, "application/json", "{\"error\":\"unauthorized\"}");
      }
      if (sSchedSkip) return;
      handleSchedulePost(r, d, l, i, t);
    });

  // --- Telemetry ---
  server.on("/api/telemetry", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!guard(r)) return;
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
    j += ",\"ntp_ok\":" + String(gNtpOK ? "true" : "false");
    j += ",\"last_ntp\":\"" + String(gLastNtpSync) + "\"";
    j += "}";
    r->send(200, "application/json", j);
  });

  server.on("/api/attendance/status", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!guard(r)) return;
    r->send(200, "application/json",
            "{\"active\":" + String(attendanceActive ? "true" : "false") + "}");
  });

  // --- Manual NTP sync: POST /api/time/sync (authed, needs STA online) ---
  server.on("/api/time/sync", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!guard(r)) return;
    sLastNtpAttempt = millis();
    if (WiFi.status() != WL_CONNECTED) {
      r->send(503, "application/json", "{\"error\":\"STA offline: join WiFi first\"}");
      return;
    }
    if (syncRTCfromNTP(NTP_TIMEOUT_MS)) {
      memset(lastFiredStart, 0, sizeof(lastFiredStart));
      memset(lastFiredEnd, 0, sizeof(lastFiredEnd));
      r->send(200, "application/json",
              "{\"status\":\"ok\",\"time\":\"" + String(gLastNtpSync) + "\"}");
    } else {
      r->send(504, "application/json", "{\"error\":\"NTP timeout\"}");
    }
  });

  // --- Login (public): urlencoded username + password -> session cookie ---
  server.on("/handleLogin", HTTP_POST, [](AsyncWebServerRequest* r) {
    if ((int32_t)(millis() - gLockUntil) < 0) {
      r->send(423, "application/json", "{\"error\":\"locked, try later\"}");
      return;
    }
    String u = r->hasParam("username", true) ? r->getParam("username", true)->value() : "";
    String p = r->hasParam("password", true) ? r->getParam("password", true)->value() : "";
    u.trim();
    if (u == ADMIN_USER && p == ADMIN_PASS) {
      gLoginFails = 0;
      auto resp = r->beginResponse(200, "application/json", "{\"status\":\"ok\"}");
      resp->addHeader("Set-Cookie", String(COOKIE_NAME) + "=" + authTokenFor(r) + "; Path=/");
      resp->addHeader("Cache-Control", "no-cache");
      r->send(resp);
      Serial.printf("[HTTP] login ok from %s\n", r->client()->remoteIP().toString().c_str());
    } else {
      gLoginFails++;
      Serial.printf("[HTTP] login fail #%d from %s\n",
                    gLoginFails, r->client()->remoteIP().toString().c_str());
      if (gLoginFails >= MAX_LOGIN_FAILS) {
        gLoginFails = 0;
        gLockUntil = millis() + LOGIN_LOCK_MS;
        r->send(423, "application/json", "{\"error\":\"locked, try later\"}");
      } else {
        r->send(401, "application/json", "{\"error\":\"invalid credentials\"}");
      }
    }
  });

  // --- Logout: expire the session cookie ---
  server.on("/logout", HTTP_GET, [](AsyncWebServerRequest* r) {
    auto resp = r->beginResponse(302, "text/plain", "logged out");
    resp->addHeader("Location", "/");
    resp->addHeader("Set-Cookie", String(COOKIE_NAME) + "=deleted; Path=/; Max-Age=0");
    r->send(resp);
  });

  // --- Audio file list (SD:/audio) ---
  server.on("/api/audio/list", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!guard(r)) return;
    String j = "{\"files\":[";
    bool first = true;
    if (sdTake()) {
      File dir = SD.open("/audio");
      if (dir) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
          String nm = String(f.name());
          int sl = nm.lastIndexOf('/');
          if (sl >= 0) nm = nm.substring(sl + 1);
          if (!nm.endsWith(".wav")) continue;
          if (!first) j += ",";
          first = false;
          j += "{\"name\":\"" + nm + "\",\"size\":" + String((int)f.size()) + "}";
        }
        dir.close();
      }
      sdGive();
    }
    j += "]}";
    r->send(200, "application/json", j);
  });

  // --- Audio upload: multipart .wav -> SD:/audio (no File Explorer needed) ---
  // Streaming write, 300KB cap. While sUploading, playback defers (see playWavFile).
  server.on("/api/audio/upload", HTTP_POST,
    [](AsyncWebServerRequest* r) {
      if (!sUpAuth) {
        if (sUpFile) { sUpFile.close(); }
        if (sUpPath.length() && sUpSize == 0) {
          if (sdTake()) { SD.remove(sUpPath); sdGive(); }
        }
        sUploading = false;
        // Note: 401 here also covers the unauthed-upload case (upload cb skipped).
        r->send(sUpAuth ? 500 : 401, "application/json",
                sUpAuth ? "{\"error\":\"upload failed\"}" : "{\"error\":\"unauthorized\"}");
        return;
      }
      if (sUpTooBig) {
        if (sUpFile) { sUpFile.close(); }
        if (sdTake()) { SD.remove(sUpPath); sdGive(); }
        sUploading = false;
        r->send(413, "application/json", "{\"error\":\"file too large (max 300KB .wav)\"}");
        return;
      }
      if (sUpFile) { sUpFile.close(); }
      sUploading = false;
      Serial.printf("[HTTP] audio upload ok: %s %u bytes\n", sUpPath.c_str(), (unsigned)sUpSize);
      r->send(200, "application/json",
              "{\"status\":\"ok\",\"name\":\"" + sUpPath.substring(String("/audio/").length()) +
              "\",\"size\":" + String((unsigned)sUpSize) + "}");
    },
    [](AsyncWebServerRequest* r, String filename, size_t index, uint8_t* data, size_t len, bool final) {
      if (index == 0) {
        sUpAuth = isAuthed(r);
        sUpSize = 0; sUpTooBig = false; sUpPath = "";
        if (sUpFile) { sUpFile.close(); }
        if (!sUpAuth) { sUploading = false; return; }
        // Sanitize: basename, [A-Za-z0-9_.-], must end .wav, <= 32 chars.
        int s = filename.lastIndexOf('/');
        int bs = filename.lastIndexOf('\\');
        if (bs > s) s = bs;
        String b = (s >= 0) ? filename.substring(s + 1) : filename;
        b.trim();
        bool ok = b.length() > 4 && b.length() <= 32 && b.endsWith(".wav");
        for (size_t i = 0; ok && i < b.length(); i++) {
          char c = b[i];
          if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')) ok = false;
        }
        if (!ok) { sUpAuth = false; sUploading = false; return; }
        sUpPath = "/audio/" + b;
        sUploading = true;
        if (sdTake()) {
          SD.mkdir("/audio");
          sUpFile = SD.open(sUpPath, FILE_WRITE);  // truncate
          sdGive();
        }
        if (!sUpFile) { sUpAuth = false; sUploading = false; return; }
      }
      if (!sUpAuth) return;
      if (sUpTooBig) return;
      if (len > 0) {
        if (sUpSize + len > AUDIO_UPLOAD_MAX) { sUpTooBig = true; return; }
        if (sUpFile) { sUpFile.write(data, len); sUpSize += len; }
      }
      if (final && sUpFile) { sUpFile.flush(); }
    });

  // --- Manual chime: queue a bell sound (WAV, WROOM-safe; .mp3 auto-maps to .wav) ---
  server.on("/ring", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!guard(r)) return;
    char path[AUDIO_PATH_MAX_LEN];
    strlcpy(path, "/audio/chime.wav", sizeof(path));
    if (r->hasParam("file")) {
      String b = r->getParam("file")->value();
      b.trim();
      int s = b.lastIndexOf('/');
      int bs = b.lastIndexOf('\\');
      if (bs > s) s = bs;
      if (s >= 0) b = b.substring(s + 1);   // strip any directory traversal
      if (b.length() > 0 && b.length() < 48 &&
          (b.endsWith(".wav") || b.endsWith(".mp3"))) {
        if (b.endsWith(".mp3")) b = b.substring(0, b.length() - 4) + ".wav";
        String full = "/audio/" + b;
        strlcpy(path, full.c_str(), sizeof(path));
      }
    }
    AudioMsg_t m;
    strlcpy(m.path, path, sizeof(m.path));
    m.isEmergency = false;
    if (!sdExists(path)) {
      r->send(404, "application/json", "{\"error\":\"audio file not on SD (need .wav)\"}");
      return;
    }
    if (sUploading) {
      r->send(503, "application/json", "{\"error\":\"upload in progress, try again\"}");
      return;
    }
    if (xQueueSend(xAudioQueue, &m, 0) != pdTRUE) {
      r->send(503, "application/json", "{\"error\":\"audio busy\"}");
      return;
    }
    Serial.printf("[HTTP] ring %s\n", path);
    r->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  // --- Network config (SPIFFS /config.json) ---
  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!guard(r)) return;
    String j = "{\"ap_ssid\":\"" + String(apSSID) + "\"";
    j += ",\"sta_ssid\":\"" + String(staSSID) + "\"";
    j += ",\"sta_pass\":\"" + String(staPass) + "\"}";
    r->send(200, "application/json", j);
  });

  server.on("/api/config", HTTP_POST,
    [](AsyncWebServerRequest* r) {},
    NULL,
    [](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t i, size_t t) {
      handleConfigPost(r, d, l, i, t);
    });

  server.onNotFound([](AsyncWebServerRequest* r) {
    String p = r->url();
    if (p.startsWith("/ui/") && SPIFFS.exists(p)) { r->send(SPIFFS, p, "text/plain"); return; }
    r->redirect("/");
  });
}