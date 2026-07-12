#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include "time.h"

// --- SETTINGS ---
const char* ssid_STA = "extension2.4";      // Your internet Wi-Fi
const char* pass_STA = "5200-524"; 
const char* ssid_AP  = "FCU_Smart_Bell_Config"; // The Hotspot name

const byte DNS_PORT = 53;
IPAddress apIP(172, 217, 28, 1);
DNSServer dnsServer;
WebServer server(80);

// Philippine Time (GMT+8)
const long gmtOffset_sec = 28800;
const int daylightOffset_sec = 0;

String getNow() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return "Time Syncing...";
  char timeStringBuff[50];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%A, %B %d, %H:%M:%S", &timeinfo);
  return String(timeStringBuff);
}

// The HTML Page
String htmlPage() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif; text-align:center; padding-top:50px; background:#f4f4f4;}";
  html += ".card{background:white; padding:20px; border-radius:15px; display:inline-block; box-shadow:0 4px 8px rgba(0,0,0,0.1); border-top: 10px solid #800000;}";
  html += "h1{color:#800000;} #time{font-size:1.5rem; font-weight:bold; color:#333;}</style>";
  html += "<script>setInterval(function(){location.reload();}, 5000);</script></head><body>"; // Auto-refresh
  html += "<div class='card'><h1>FCU Smart Bell</h1>";
  html += "<p>Current Network Time:</p>";
  html += "<div id='time'>" + getNow() + "</div>";
  html += "<p style='color:green;'>System Status: Active</p>";
  html += "</div></body></html>";
  return html;
}

void setup() {
  Serial.begin(115200);

  // 1. Connect to Internet
  WiFi.begin(ssid_STA, pass_STA);
  Serial.print("Connecting to Internet...");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  
  // 2. Setup Time
  configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org");

  // 3. Setup Hotspot (AP)
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(ssid_AP);

  // 4. Start DNS for Captive Portal
  dnsServer.start(DNS_PORT, "*", apIP);

  // 5. Define Server Routes
  server.onNotFound([]() {
    server.send(200, "text/html", htmlPage());
  });
  
  server.begin();
  Serial.println("\nReady! Connect to 'FCU_Smart_Bell_Config'");
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
}