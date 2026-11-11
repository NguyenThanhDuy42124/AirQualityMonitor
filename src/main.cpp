#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <DHT.h>
#include <WebSocketsServer.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include "index_html_gz.h"

// ============================================================================
// 1. CẤU HÌNH THÔNG SỐ HỆ THỐNG & NVS FLASH
// ============================================================================
Preferences prefs;
String currentSSID = "";
String currentPass = "";

// Cấu hình Supabase Cloud IoT:
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  // Cấu hình mặc định / mẫu (Vui lòng sao chép secrets.example.h thành secrets.h và điền thông tin Supabase của bạn)
  const char* supabaseUrl = "https://YOUR_PROJECT_ID.supabase.co";
  const char* supabaseKey = "YOUR_SUPABASE_ANON_KEY";
#endif

// Cấu hình SQLite / Local Offline Cache (LittleFS Flash Storage):
const char* SQL_CACHE_FILE = "/sql_cache.jsonl";
const int MAX_SQL_CACHE_RECORDS = 50;  // Giới hạn bộ nhớ đệm để bảo vệ Flash
const int SYNC_BATCH_SIZE = 20;        // Đồng bộ tối đa 20 bản ghi mỗi batch để tránh quá tải
int sqlCacheCount = 0;

// Chân phần cứng (ESP32 38 Chân + Bo mở rộng tím):
const int GAS_PIN    = 34;   // Cảm biến khí MQ-135 (AO -> P34)
const int DHT_PIN    = 15;   // Cảm biến DHT22 (OUT -> P15)
const int BUZZER_PIN = 23;   // Active Buzzer 5V (Chân (+) -> P23)
const int LED_GREEN  = 18;   // LED Xanh
const int LED_YELLOW = 19;   // LED Vàng
const int LED_RED    = 4;    // LED Đỏ

// Ngưỡng không khí:
volatile int gasThresholdDanger   = 500; // >= 500: NGUY HIỂM -> HÚ CÒI
volatile int gasThresholdModerate = 250; // = 1/2 ngưỡng nguy hiểm: Ô nhiễm nhẹ/vàng
const float TEMP_WARNING          = 35.0; // °C
const float HUMIDITY_WARNING      = 85.0; // %

// ============================================================================
// 2. BIẾN TOÀN CỤC CHIA SẺ GIỮA 2 NHÂN (CORE 0 & CORE 1)
// ============================================================================
volatile float sharedTemp          = 28.0;
volatile float sharedHumidity      = 65.0;
volatile int   sharedGasLevel      = 0;
volatile int   sharedPeakGas       = 0;
volatile int   sharedAqiLevel      = 0;  // 0: Tốt, 1: TB, 2: Nguy hiểm
volatile int   sharedMode          = 0;  // 0: AUTO, 1: ON, 2: OFF / SNOOZE
volatile bool  sharedBuzzerActive  = false;
volatile bool  sharedWifiConnected = false;
volatile bool  isAPMode            = false;
String currentIPStr                = "Connecting...";

// Tạm tắt còi (Snooze 30 giây)
volatile bool isSnoozed            = false;
volatile unsigned long snoozeStartTime = 0;
const unsigned long SNOOZE_DURATION_MS = 30000;
volatile int snoozeRemainingSec    = 0;

// Quản lý thời gian kết nối WiFi STA
unsigned long wifiConnectStartTime = 0;
const unsigned long WIFI_STA_TIMEOUT = 30000; // 30 giay thu ket noi STA

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
#define DHT_TYPE DHT22
DHT dht(DHT_PIN, DHT_TYPE);
WebServer server(80);
WebSocketsServer webSocket(81);

// ============================================================================
// 3. QUẢN LÝ KẾT NỐI WIFI & SOFTAP DỰ PHÒNG
// ============================================================================
String apSSID = "AirMonitor_Duy";
const char* DEFAULT_AP_PASS = "admin1234";

void initApSSID() {
  apSSID = "AirMonitor_Duy";
}

void startAPMode() {
  if (isAPMode) return;
  isAPMode = true;
  sharedWifiConnected = false;

  Serial.println("[WIFI AP] Khoi dong SoftAP toi gian theo chuan...");
  initApSSID();

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  delay(100);

  bool apSuccess = WiFi.softAP(apSSID.c_str(), DEFAULT_AP_PASS);
  delay(200);

  IPAddress apIP = WiFi.softAPIP();
  currentIPStr = apIP.toString();
  Serial.printf("[WIFI AP] Khoi dong SoftAP %s: SSID = %s | Pass = %s | IP = %s\r\n",
                apSuccess ? "THANH CONG" : "THAT BAI", apSSID.c_str(), DEFAULT_AP_PASS, currentIPStr.c_str());

  MDNS.begin("esp32");
  server.begin();
  webSocket.begin();
}

void tryConnectSTA(const String& s, const String& p) {
  if (s.length() == 0) {
    Serial.println("[WIFI STA] Khong co WiFi duoc luu! Chuyen thang sang SoftAP AirMonitor_Duy...");
    startAPMode();
    return;
  }
  isAPMode = false;
  sharedWifiConnected = false;
  WiFi.softAPdisconnect(true);
  delay(50);
  WiFi.mode(WIFI_STA);
  WiFi.begin(s.c_str(), p.c_str());
  wifiConnectStartTime = millis();
  currentIPStr = "Connecting...";
  Serial.printf("[WIFI STA] Bat dau thu ket noi toi: %s\r\n", s.c_str());
}

void buildTelemetryJson(char* buf, size_t maxLen) {
  snprintf(buf, maxLen,
    "{\"temp\":%.1f,\"humi\":%.1f,\"gas\":%d,\"peak\":%d,\"aqi\":%d,\"mode\":%d,\"buzzer\":%s,\"snooze\":%s,\"snooze_rem\":%d,\"thresh_mod\":%d,\"thresh_dang\":%d,\"ip\":\"%s\",\"is_ap\":%s,\"sql_cache\":%d}",
    sharedTemp, sharedHumidity, sharedGasLevel, sharedPeakGas, sharedAqiLevel, sharedMode,
    sharedBuzzerActive ? "true" : "false",
    isSnoozed ? "true" : "false",
    snoozeRemainingSec,
    gasThresholdModerate, gasThresholdDanger,
    currentIPStr.c_str(),
    isAPMode ? "true" : "false",
    sqlCacheCount
  );
}

// ============================================================================
// 4. WEBSOCKET REAL-TIME EVENT HANDLER
// ============================================================================
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.printf("[WS] Client #%u thoat\r\n", num);
      break;

    case WStype_CONNECTED: {
      IPAddress ip = webSocket.remoteIP(num);
      Serial.printf("[WS] Client #%u ket noi tu: %s\r\n", num, ip.toString().c_str());
      char jsonBuf[384];
      buildTelemetryJson(jsonBuf, sizeof(jsonBuf));
      webSocket.sendTXT(num, jsonBuf);
      break;
    }

    case WStype_TEXT: {
      String msg = String((char*)payload);
      if (msg.indexOf("\"cmd\":\"mode\"") >= 0) {
        int idx = msg.indexOf("\"val\":");
        if (idx > 0) {
          int m = msg.substring(idx + 6).toInt();
          if (m == 2) {
            // Tạm tắt còi (Snooze 30 giây)
            sharedMode = 2;
            isSnoozed = true;
            snoozeStartTime = millis();
            snoozeRemainingSec = 30;
            Serial.println("[WS] Lenh TAM TAT COI 30s!");
          } else if (m == 0 || m == 1) {
            sharedMode = m;
            isSnoozed = false;
            snoozeRemainingSec = 0;
            Serial.printf("[WS] Chuyen Mode = %d\r\n", m);
          }
        }
      } else if (msg.indexOf("\"cmd\":\"thresh\"") >= 0) {
        int idx = msg.indexOf("\"val\":");
        if (idx > 0) {
          int d = msg.substring(idx + 6).toInt();
          if (d > 100 && d < 4000) {
            gasThresholdDanger = d;
            gasThresholdModerate = d / 2; // Luôn bằng 1/2 ngưỡng cao nhất
            prefs.putInt("thresh_dang", gasThresholdDanger);
            prefs.putInt("thresh_mod", gasThresholdModerate);
            sharedMode = 0;
            isSnoozed = false;
            snoozeRemainingSec = 0;
            Serial.printf("[WS] Nguong moi: Danger = %d, Mod = %d (1/2)\r\n", gasThresholdDanger, gasThresholdModerate);
          }
        }
      }
      break;
    }

    default:
      break;
  }
}

// ============================================================================
// 5. REST API & GZIP WEB SERVER
// ============================================================================
void handleRoot() {
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.send_P(200, "text/html", (const char*)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
}

void handleApiData() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  char jsonBuf[384];
  buildTelemetryJson(jsonBuf, sizeof(jsonBuf));
  server.send(200, "application/json", jsonBuf);
}

void handleApiCmd() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.hasArg("mode")) {
    int m = server.arg("mode").toInt();
    if (m == 2) {
      sharedMode = 2;
      isSnoozed = true;
      snoozeStartTime = millis();
      snoozeRemainingSec = 30;
      server.send(200, "text/plain", "OK");
      return;
    } else if (m == 0 || m == 1) {
      sharedMode = m;
      isSnoozed = false;
      snoozeRemainingSec = 0;
      server.send(200, "text/plain", "OK");
      return;
    }
  }
  server.send(400, "text/plain", "Bad Request");
}

void handleApiThreshold() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.hasArg("danger")) {
    int d = server.arg("danger").toInt();
    if (d > 100 && d < 4000) {
      gasThresholdDanger = d;
      gasThresholdModerate = d / 2; // Luôn bằng 1/2 ngưỡng cao nhất
      prefs.putInt("thresh_dang", gasThresholdDanger);
      prefs.putInt("thresh_mod", gasThresholdModerate);
      sharedMode = 0;
      isSnoozed = false;
      snoozeRemainingSec = 0;
      Serial.printf("[REST] Nguong moi: Danger = %d, Mod = %d (1/2)\r\n", gasThresholdDanger, gasThresholdModerate);
      server.send(200, "text/plain", "OK");
      return;
    }
  }
  server.send(400, "text/plain", "Bad Request");
}

void handleWifiScan() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; ++i) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleWifiSave() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.hasArg("ssid")) {
    String newSsid = server.arg("ssid");
    String newPass = server.hasArg("pass") ? server.arg("pass") : "";
    newSsid.trim();
    newPass.trim();
    if (newSsid.length() > 0) {
      prefs.putString("wifi_ssid", newSsid);
      prefs.putString("wifi_pass", newPass);
      currentSSID = newSsid;
      currentPass = newPass;
      server.send(200, "application/json", "{\"status\":\"ok\",\"msg\":\"Da luu! ESP32 dang tat AP va ket noi mang moi...\"}");
      delay(800);
      WiFi.softAPdisconnect(true);
      delay(200);
      ESP.restart();
      return;
    }
  }
  server.send(400, "application/json", "{\"status\":\"error\",\"msg\":\"Thieu ten WiFi (SSID)\"}");
}

void handleWifiReset() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  prefs.remove("wifi_ssid");
  prefs.remove("wifi_pass");
  currentSSID = "";
  currentPass = "";
  server.send(200, "application/json", "{\"status\":\"ok\",\"msg\":\"Da xoa data WiFi! Dang phat lai SoftAP...\"}");
  delay(800);
  WiFi.disconnect(true, true);
  WiFi.softAPdisconnect(true);
  delay(200);
  ESP.restart();
}

void handleNotFound() {
  String uri = server.uri();
  if (uri.endsWith("generate_204") || uri.endsWith("hotspot-detect.html") || uri.endsWith("ncsi.txt") || uri.endsWith("connecttest.txt")) {
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302, "text/plain", "");
    return;
  }
  if (server.method() == HTTP_HEAD && (uri == "/" || uri == "")) {
    server.sendHeader("Content-Encoding", "gzip");
    server.send(200, "text/html", "");
    return;
  }
  server.send(404, "text/plain", "Not Found");
}

// ============================================================================
// 5B. QUẢN LÝ BỘ NHỚ ĐỆM SQLITE / LOCAL FLASH (OFFLINE CACHE & SYNC)
// ============================================================================
void countSqlCacheRecords() {
  sqlCacheCount = 0;
  if (!LittleFS.exists(SQL_CACHE_FILE)) return;
  File f = LittleFS.open(SQL_CACHE_FILE, "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 5) sqlCacheCount++;
  }
  f.close();
}

void initSqlCache() {
  countSqlCacheRecords();
  Serial.printf("[SQL CACHE] Khoi tao SQLite Store thanh cong, hien co: %d ban ghi\r\n", sqlCacheCount);
}

void appendSqlRecord(float temp, float humi, int gas, int peak, int aqi, int mode, int thresh, bool buzzer) {
  if (sqlCacheCount >= MAX_SQL_CACHE_RECORDS) {
    return; // Đã đạt trần an toàn, bảo vệ flash
  }

  File f = LittleFS.open(SQL_CACHE_FILE, "a");
  if (!f) return;

  char buf[220];
  snprintf(buf, sizeof(buf),
    "{\"temp\":%.1f,\"humidity\":%.1f,\"gas\":%d,\"peak\":%d,\"aqi\":%d,\"mode\":%d,\"danger_thresh\":%d,\"buzzer\":%s}",
    temp, humi, gas, peak, aqi, mode, thresh, buzzer ? "true" : "false"
  );
  f.println(buf);
  f.close();
  sqlCacheCount++;
  Serial.printf("[SQL CACHE] Da luu ban ghi offline vao SQLite/Local DB (Tong: %d ban ghi)\r\n", sqlCacheCount);
}

void clearSqlCache() {
  if (LittleFS.exists(SQL_CACHE_FILE)) {
    LittleFS.remove(SQL_CACHE_FILE);
  }
  sqlCacheCount = 0;
  Serial.println("[SQL CACHE] Da xoa toan bo cache offline local!");
}

void syncSqlCacheToSupabase() {
  if (sqlCacheCount <= 0 || !LittleFS.exists(SQL_CACHE_FILE)) {
    sqlCacheCount = 0;
    return;
  }

  File f = LittleFS.open(SQL_CACHE_FILE, "r");
  if (!f) return;

  String batchJson = "[";
  int batchCount = 0;
  File tempFile;
  bool hasRemaining = false;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 5) {
      if (batchCount < SYNC_BATCH_SIZE) {
        if (batchCount > 0) batchJson += ",";
        batchJson += line;
        batchCount++;
      } else {
        if (!hasRemaining) {
          hasRemaining = true;
          tempFile = LittleFS.open("/sql_temp.jsonl", "w");
        }
        if (tempFile) {
          tempFile.println(line);
        }
      }
    }
  }
  f.close();
  if (tempFile) tempFile.close();
  batchJson += "]";

  if (batchCount == 0) {
    clearSqlCache();
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(4);

  HTTPClient http;
  String insertUrl = String(supabaseUrl) + "/rest/v1/air_telemetry";
  if (http.begin(client, insertUrl)) {
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", supabaseKey);
    http.addHeader("Authorization", String("Bearer ") + supabaseKey);
    http.addHeader("Prefer", "return=minimal");
    http.setTimeout(4000);

    int code = http.POST(batchJson);
    if (code == HTTP_CODE_CREATED || code == HTTP_CODE_OK || code == 204) {
      Serial.printf("[SQL SYNC] Dong bo thanh cong %d ban ghi offline len Supabase! (HTTP %d)\r\n", batchCount, code);
      LittleFS.remove(SQL_CACHE_FILE);
      if (hasRemaining && LittleFS.exists("/sql_temp.jsonl")) {
        LittleFS.rename("/sql_temp.jsonl", SQL_CACHE_FILE);
        countSqlCacheRecords();
      } else {
        sqlCacheCount = 0;
      }
    } else {
      Serial.printf("[SQL SYNC] Loi dong bo batch len Supabase (Code %d), giu lai cache thu lai sau.\r\n", code);
      if (hasRemaining && LittleFS.exists("/sql_temp.jsonl")) {
        LittleFS.remove("/sql_temp.jsonl");
      }
    }
    http.end();
  }
}

void handleApiSqlCache() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!LittleFS.exists(SQL_CACHE_FILE)) {
    server.send(200, "application/json", "{\"count\":0,\"records\":[]}");
    return;
  }
  File f = LittleFS.open(SQL_CACHE_FILE, "r");
  if (!f) {
    server.send(500, "application/json", "{\"count\":0,\"error\":\"read_error\"}");
    return;
  }
  String json = "{\"count\":" + String(sqlCacheCount) + ",\"records\":[";
  int c = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 5) {
      if (c > 0) json += ",";
      json += line;
      c++;
    }
  }
  f.close();
  json += "]}";
  server.send(200, "application/json", json);
}

void handleApiSqlClear() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  clearSqlCache();
  server.send(200, "application/json", "{\"status\":\"ok\",\"msg\":\"Da xoa sach SQL Cache local!\"}");
}

// ============================================================================
// 6. HIỂN THỊ MÀN HÌNH OLED 0.96" SSD1306 (GIAO DIỆN PHÒNG THÍ NGHIỆM LAB)
// ============================================================================
const char* getAirQualityStatus(int aqi) {
  if (aqi >= 2) return "KK: NGUY HIEM KHOI!";
  if (aqi == 1) return "KK: O NHIEM (CHU Y)";
  return "KK: AN TOAN (SACH)";
}

const char* getClimateStatus(float temp, float humi) {
  if (isnan(temp) || isnan(humi) || (temp == 0.0f && humi == 0.0f)) {
    return "MT: DANG KHOI TAO...";
  }
  if (temp >= 28.0f && humi >= 70.0f) return "MT: NONG & AM";
  if (temp >= 28.0f && humi < 40.0f)  return "MT: NONG & KHO";
  if (temp >= 28.0f)                  return "MT: NONG (CAN QUAT)";
  if (temp < 20.0f && humi >= 70.0f)  return "MT: LANH & AM";
  if (temp < 20.0f && humi < 40.0f)   return "MT: LANH & KHO";
  if (temp < 20.0f)                   return "MT: LANH (CAN SUOI)";
  if (humi >= 70.0f)                  return "MT: AM (NGUY CO MOC)";
  if (humi < 40.0f)                   return "MT: KHO (NGUY CO ESD)";
  return "MT: ON DINH (CHUAN)";
}

void updateOLED() {
  u8g2.clearBuffer();

  // 1. THANH TIÊU ĐỀ (Top Bar - y=9)
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(2, 9, "LAB AIR");

  if (isSnoozed) {
    char szBuf[16];
    snprintf(szBuf, sizeof(szBuf), "[MUT:%2ds]", snoozeRemainingSec);
    u8g2.drawStr(70, 9, szBuf);
  } else if (isAPMode) {
    u8g2.drawStr(76, 9, "Mode ARP");
  } else {
    const char* mTag = (sharedMode == 0) ? "Mode AUTO" : ((sharedMode == 1) ? "Mode ON" : "Mode OFF");
    u8g2.drawStr(68, 9, mTag);
  }
  u8g2.drawHLine(0, 11, 128);

  // 2. CHỈ SỐ KHÍ GAS / KHÓI HÓA CHẤT (ĐÃ BỎ PK & TH - RỘNG RÃI CHO 4K ADC - y=21)
  u8g2.setFont(u8g2_font_helvB08_tr);
  char gasBuf[24];
  snprintf(gasBuf, sizeof(gasBuf), "GAS: %d ADC", sharedGasLevel);
  u8g2.drawStr(2, 21, gasBuf);

  // 3. CHỈ SỐ NHIỆT ĐỘ & ĐỘ ẨM (y=32)
  char thMainBuf[30];
  snprintf(thMainBuf, sizeof(thMainBuf), "%.1f\xb0 C    %.0f%% RH", sharedTemp, sharedHumidity);
  u8g2.drawStr(2, 32, thMainBuf);

  // 4. ĐÁNH GIÁ CHẤT LƯỢNG KHÔNG KHÍ (DÒNG RIÊNG THEO KHÓI/GAS - y=43)
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(2, 43, getAirQualityStatus(sharedAqiLevel));

  // 5. ĐÁNH GIÁ VI KHÍ HẬU PHÒNG LAB THEO NHIỆT/ẨM (y=53)
  u8g2.drawStr(2, 53, getClimateStatus(sharedTemp, sharedHumidity));
  u8g2.drawHLine(0, 55, 128);

  // 6. THÔNG TIN PHỤ: KẾT NỐI MẠNG / CÒI ĐÁY MÀN HÌNH (y=63)
  u8g2.setFont(u8g2_font_5x7_tf);
  char botBuf[36];
  if (isAPMode) {
    snprintf(botBuf, sizeof(botBuf), "IP:192.168.4.1|admin1234");
  } else {
    snprintf(botBuf, sizeof(botBuf), "IP:%s|Coi:%s", currentIPStr.c_str(), sharedBuzzerActive ? "ON" : "OFF");
  }
  u8g2.drawStr(2, 63, botBuf);

  u8g2.sendBuffer();
}

// ============================================================================
// GIAI ĐIỆU BẠC PHẬN (JACK ft. K-ICM) - BẢN KÝ ÂM CHUẨN THEO SHEET (108 BPM)
// ============================================================================
#define NOTE_F4   349
#define NOTE_G4   392
#define NOTE_A4   440
#define NOTE_AS4  466
#define NOTE_C5   523
#define NOTE_D5   587
#define NOTE_E5   659
#define NOTE_F5   698
#define NOTE_G5   784
#define NOTE_A5   880
#define NOTE_AS5  932
#define NOTE_REST 0

#define D_05B 278
#define D_10B 556
#define D_15B 833
#define D_20B 1111
#define D_25B 1389
#define D_30B 1667
#define D_40B 2222
#define D_65B 3611

struct NoteStep {
  uint16_t freq;
  uint16_t duration;
};

const NoteStep bacPhanMelody[] = {
  // --- PHẦN 1: VERSE 1 (Ô nhịp 1 - 8) ---
  // Ô 1 (Dm): Ai Rê5 - gieo Rê5 - tình La4 - này, La4
  {NOTE_D5, D_10B}, {NOTE_D5, D_10B}, {NOTE_A4, D_10B}, {NOTE_A4, D_10B},
  // Ô 2 (C): ai Đô5 - mang Đô5 - tình Sol4 - này, Sol4
  {NOTE_C5, D_10B}, {NOTE_C5, D_10B}, {NOTE_G4, D_10B}, {NOTE_G4, D_10B},
  // Ô 3 (Bb): để Fa4 - lệ Sol4 - trên La4 - khóe Đô5
  {NOTE_F4, D_15B}, {NOTE_G4, D_05B}, {NOTE_A4, D_10B}, {NOTE_C5, D_10B},
  // Ô 4 (A): mi La4 - cay La4 - (Nghỉ/Rest)
  {NOTE_A4, D_20B}, {NOTE_A4, D_10B}, {NOTE_REST, D_10B},
  // Ô 5 (Dm): ai Rê5 - đưa Rê5 - về La4 - nhà, La4
  {NOTE_D5, D_10B}, {NOTE_D5, D_10B}, {NOTE_A4, D_10B}, {NOTE_A4, D_10B},
  // Ô 6 (C): ai Đô5 - cho Đô5 - ngọc Sol4 - ngà, Sol4
  {NOTE_C5, D_10B}, {NOTE_C5, D_10B}, {NOTE_G4, D_10B}, {NOTE_G4, D_10B},
  // Ô 7 (Bb): giờ Fa4 - người Sol4 - xa La4 - cách Đô5
  {NOTE_F4, D_15B}, {NOTE_G4, D_05B}, {NOTE_A4, D_10B}, {NOTE_C5, D_10B},
  // Ô 8 (A): ta La4 - từng Đô5 - là Đô5 - một Đô5
  {NOTE_A4, D_25B}, {NOTE_C5, D_05B}, {NOTE_C5, D_05B}, {NOTE_C5, D_05B},

  // --- PHẦN 2: PRE-CHORUS (Ô nhịp 9 - 16) ---
  // Ô 9 (Dm): thời Rê5 - thiếu Fa5 - nữ La5 - trong Rê5
  {NOTE_D5, D_10B}, {NOTE_F5, D_10B}, {NOTE_A5, D_10B}, {NOTE_D5, D_10B},
  // Ô 10 (C): vùng Đô5 - quê Đô5 - nghèo Rê5 - Đô5 - hồn La4
  {NOTE_C5, D_15B}, {NOTE_C5, D_05B}, {NOTE_D5, D_05B}, {NOTE_C5, D_10B}, {NOTE_A4, D_05B},
  // Ô 11 (Bb): nhiên Rê5 - cài Đô5 - hoa Rê5 - mái Fa5
  {NOTE_D5, D_10B}, {NOTE_C5, D_05B}, {NOTE_D5, D_10B}, {NOTE_F5, D_15B},
  // Ô 12 (A): đầu La4 - dòng Đô5 - người Đô5 - vội Đô5
  {NOTE_A4, D_25B}, {NOTE_C5, D_05B}, {NOTE_C5, D_05B}, {NOTE_C5, D_05B},
  // Ô 13 (Dm): vàng Rê5 - em Fa5 - hóa La5 - thân Rê5
  {NOTE_D5, D_10B}, {NOTE_F5, D_10B}, {NOTE_A5, D_10B}, {NOTE_D5, D_10B},
  // Ô 14 (C): đời Đô5 - Đô5 - bẽ Rê5 - bàng Đô5 - rời La4
  {NOTE_C5, D_05B}, {NOTE_C5, D_10B}, {NOTE_D5, D_05B}, {NOTE_C5, D_15B}, {NOTE_A4, D_05B},
  // Ô 15 (Bb): xa Rê5 - tình Đô5 - anh Rê5 - năm Fa5
  {NOTE_D5, D_10B}, {NOTE_C5, D_05B}, {NOTE_D5, D_20B}, {NOTE_F5, D_05B},
  // Ô 16 (A): tháng Mi5 - ôi Mi5 - phút Fa5 - giây Mi5 - tương Rê5
  {NOTE_E5, D_20B}, {NOTE_E5, D_05B}, {NOTE_F5, D_05B}, {NOTE_E5, D_05B}, {NOTE_D5, D_05B},

  // --- PHẦN 3: ĐIỆP KHÚC / CHORUS (Ô nhịp 17 - 33) ---
  // Ô 17 (Bb): phùng Rê5 - anh Fa5 - nhớ La5 - và Đô5 - Fa5
  {NOTE_D5, D_10B}, {NOTE_F5, D_10B}, {NOTE_A5, D_10B}, {NOTE_C5, D_05B}, {NOTE_F5, D_05B},
  // Ô 18 (C): mong Mi5 - dòng Mi5 - lưu Fa5 - bút Sol5 - năm Mi5
  {NOTE_E5, D_20B}, {NOTE_E5, D_05B}, {NOTE_F5, D_05B}, {NOTE_G5, D_05B}, {NOTE_E5, D_05B},
  // Ô 19 (Am): xưa Mi5 - in Mi5 - dấu Sol5 - mãi Sol5 - đậm Đô5
  {NOTE_E5, D_10B}, {NOTE_E5, D_05B}, {NOTE_G5, D_10B}, {NOTE_G5, D_05B}, {NOTE_C5, D_10B},
  // Ô 20 (Dm): sâu Rê5 - trong Rê5 - nỗi Mi5 - đau Rê5 - anh Đô5
  {NOTE_D5, D_20B}, {NOTE_D5, D_05B}, {NOTE_E5, D_05B}, {NOTE_D5, D_05B}, {NOTE_C5, D_05B},
  // Ô 21 (Bb): mệt Đô5 - nhoài Fa5 - trong Rê5 - phút Mi5 - giây Fa5 - anh Rê5
  {NOTE_C5, D_05B}, {NOTE_F5, D_15B}, {NOTE_D5, D_05B}, {NOTE_E5, D_05B}, {NOTE_F5, D_05B}, {NOTE_D5, D_05B},
  // Ô 22 (C): tìm Rê5 - hoài Sol5 - muốn Mi5 - giữ Fa5 - em Sol5 - ở Mi5
  {NOTE_D5, D_05B}, {NOTE_G5, D_15B}, {NOTE_E5, D_05B}, {NOTE_F5, D_05B}, {NOTE_G5, D_05B}, {NOTE_E5, D_05B},
  // Ô 23 (Am): lại Mi5 - một Rê5 - lần Đô5 - này Rê5 - vì Sol4
  {NOTE_E5, D_10B}, {NOTE_D5, D_05B}, {NOTE_C5, D_05B}, {NOTE_D5, D_10B}, {NOTE_G4, D_10B},
  // Ô 24 (Dm): anh Đô5 - mãi Mi5 - thương Rê5 - xa Rê5 - cách Mi5 - nhau Fa5 - thật Rê5
  {NOTE_C5, D_05B}, {NOTE_E5, D_10B}, {NOTE_D5, D_05B}, {NOTE_D5, D_05B}, {NOTE_E5, D_05B}, {NOTE_F5, D_05B}, {NOTE_D5, D_05B},
  // Ô 25 (Bb): rồi Rê5 - sương Fa5 - trắng La5 - chiều Đô5 - Fa5
  {NOTE_D5, D_10B}, {NOTE_F5, D_10B}, {NOTE_A5, D_10B}, {NOTE_C5, D_05B}, {NOTE_F5, D_05B},
  // Ô 26 (C): thu Mi5 - ngày Mi5 - em Fa5 - bước Sol5 - ra Mi5
  {NOTE_E5, D_20B}, {NOTE_E5, D_05B}, {NOTE_F5, D_05B}, {NOTE_G5, D_05B}, {NOTE_E5, D_05B},
  // Ô 27 (Am): đi Mi5 - nước Mi5 - mắt Sol5 - ấy Sol5 - biệt Đô5
  {NOTE_E5, D_10B}, {NOTE_E5, D_05B}, {NOTE_G5, D_10B}, {NOTE_G5, D_05B}, {NOTE_C5, D_10B},
  // Ô 28 (Dm): li Rê5 - hoa Rê5 - vẫn Mi5 - rơi Rê5 - bên Đô5
  {NOTE_D5, D_20B}, {NOTE_D5, D_05B}, {NOTE_E5, D_05B}, {NOTE_D5, D_05B}, {NOTE_C5, D_05B},
  // Ô 29 (Bb): thềm Đô5 - nhà Fa5 - lá Rê5 - xát Mi5 - xơ Fa5 - đi Rê5
  {NOTE_C5, D_05B}, {NOTE_F5, D_15B}, {NOTE_D5, D_05B}, {NOTE_E5, D_05B}, {NOTE_F5, D_05B}, {NOTE_D5, D_05B},
  // Ô 30 (C): nhiều Rê5 - và Sol5 - anh Mi5 - chúc Fa5 - em Sol5 - yên Mi5
  {NOTE_D5, D_05B}, {NOTE_G5, D_15B}, {NOTE_E5, D_05B}, {NOTE_F5, D_05B}, {NOTE_G5, D_05B}, {NOTE_E5, D_05B},
  // Ô 31 (Am): bình Mi5 - mối Rê5 - tình Đô5 - mình Rê5 - hẹn Sol4
  {NOTE_E5, D_15B}, {NOTE_D5, D_05B}, {NOTE_C5, D_05B}, {NOTE_D5, D_10B}, {NOTE_G4, D_05B},
  // Ô 32–33 (Khung số 1 - Dm): em Đô5 - kiếp Mi5 - sau Rê5
  {NOTE_C5, D_05B}, {NOTE_E5, D_10B}, {NOTE_D5, D_65B},

  // --- PHẦN 4: ĐOẠN RAP / MELODIC RAP (Ô nhịp 34 - 49) ---
  // Ô 34 (Bb): thoáng Rê5 - thoáng, Rê5 - ngày La4
  {NOTE_D5, D_15B}, {NOTE_D5, D_15B}, {NOTE_A4, D_10B},
  // Ô 35 (C): miên Đô5 - mang, Đô5 - giờ Sol4
  {NOTE_C5, D_15B}, {NOTE_C5, D_15B}, {NOTE_G4, D_10B},
  // Ô 36 (Am): con Đô5 - nước Đô5 - dài Sol4
  {NOTE_C5, D_15B}, {NOTE_C5, D_15B}, {NOTE_G4, D_10B},
  // Ô 37 (Dm): thênh La4 - thang La4
  {NOTE_A4, D_15B}, {NOTE_A4, D_25B},
  // Ô 38 (Bb): không Rê5 - trách, Rê5 - người La4
  {NOTE_D5, D_15B}, {NOTE_D5, D_15B}, {NOTE_A4, D_10B},
  // Ô 39 (C): không Đô5 - thương, Đô5 - mà Sol4
  {NOTE_C5, D_15B}, {NOTE_C5, D_15B}, {NOTE_G4, D_10B},
  // Ô 40 (Am): hương Mi5 - tóc Mi5 - còn Sol4
  {NOTE_E5, D_15B}, {NOTE_E5, D_15B}, {NOTE_G4, D_10B},
  // Ô 41 (Dm): vương La4 - vương La4 - La4 - giờ Đô5
  {NOTE_A4, D_15B}, {NOTE_A4, D_15B}, {NOTE_A4, D_05B}, {NOTE_C5, D_05B},
  // Ô 42 (Bb): em Rê5 - Fa5 - ở Mi5 - nơi Rê5 - khuê Đô5 - phòng La4 - ngày Đô5
  {NOTE_D5, D_05B}, {NOTE_F5, D_05B}, {NOTE_E5, D_05B}, {NOTE_D5, D_05B}, {NOTE_C5, D_10B}, {NOTE_A4, D_05B}, {NOTE_C5, D_05B},
  // Ô 43 (C): mai Rê5 - nữa Fa5 - em Mi5 - theo Rê5 - chồng Đô5 - và Đô5
  {NOTE_D5, D_05B}, {NOTE_F5, D_05B}, {NOTE_E5, D_05B}, {NOTE_D5, D_05B}, {NOTE_C5, D_15B}, {NOTE_C5, D_05B},
  // Ô 44 (Am): tô Mi5 - má Sol5 - em Fa5 - thêm Mi5 - hồng Rê5 - ôi Rê5 - đớn Mi5 - đau Đô5
  {NOTE_E5, D_05B}, {NOTE_G5, D_05B}, {NOTE_F5, D_05B}, {NOTE_E5, D_05B}, {NOTE_D5, D_05B}, {NOTE_D5, D_05B}, {NOTE_E5, D_05B}, {NOTE_C5, D_05B},
  // Ô 45 (Dm): lòng La4 - ôi Rê5 - đớn Mi5 - đau Đô5 - lòng La4 - bình Đô5
  {NOTE_A4, D_05B}, {NOTE_D5, D_05B}, {NOTE_E5, D_05B}, {NOTE_C5, D_05B}, {NOTE_A4, D_15B}, {NOTE_C5, D_05B},
  // Ô 46 (Bb): minh Rê5 - dẫn Fa5 - em Mi5 - đi Rê5 - rồi Đô5 - vòng Đô5
  {NOTE_D5, D_05B}, {NOTE_F5, D_05B}, {NOTE_E5, D_05B}, {NOTE_D5, D_05B}, {NOTE_C5, D_15B}, {NOTE_C5, D_05B},
  // Ô 47 (C): quay Rê5 - bánh Fa5 - xe Mi5 - luân Rê5 - hồi Đô5 - hoàng Đô5
  {NOTE_D5, D_05B}, {NOTE_F5, D_05B}, {NOTE_E5, D_05B}, {NOTE_D5, D_05B}, {NOTE_C5, D_15B}, {NOTE_C5, D_05B},
  // Ô 48 (Am): hôn Mi5 - khuất Sol5 - sau Fa5 - lưng Mi5 - đồi Rê5 - ôi Rê5 - vỡ Mi5 - tan Đô5
  {NOTE_E5, D_05B}, {NOTE_G5, D_05B}, {NOTE_F5, D_05B}, {NOTE_E5, D_05B}, {NOTE_D5, D_05B}, {NOTE_D5, D_05B}, {NOTE_E5, D_05B}, {NOTE_C5, D_05B},
  // Ô 49 (Dm): rồi La4 - ôi Rê5 - vỡ Mi5 - tan Đô5 - rồi La4 - một Đô5 - ngày Đô5
  {NOTE_A4, D_05B}, {NOTE_D5, D_05B}, {NOTE_E5, D_05B}, {NOTE_C5, D_05B}, {NOTE_A4, D_10B}, {NOTE_C5, D_05B}, {NOTE_C5, D_05B},

  // --- PHẦN 5: PRE-CHORUS 2 & LẶP LẠI (Ô nhịp 50 - 57) ---
  // Ô 50 (Dm): buồn Rê5 - mây Fa5 - tím La5 - em Rê5 - Rê5
  {NOTE_D5, D_10B}, {NOTE_F5, D_05B}, {NOTE_A5, D_10B}, {NOTE_D5, D_05B}, {NOTE_D5, D_10B},
  // Ô 51 (C): về Đô5 - thôn Rê5 - làng Đô5 - mẹ La4
  {NOTE_C5, D_10B}, {NOTE_D5, D_05B}, {NOTE_C5, D_20B}, {NOTE_A4, D_05B},
  // Ô 52 (Bb): cha Rê5 - của Đô5 - em Rê5 - vỡ Fa5 - Rê5
  {NOTE_D5, D_10B}, {NOTE_C5, D_05B}, {NOTE_D5, D_10B}, {NOTE_F5, D_10B}, {NOTE_D5, D_05B},
  // Ô 53 (A): òa La4 - giọt Đô5 - lệ Đô5 - chạnh Đô5
  {NOTE_A4, D_25B}, {NOTE_C5, D_05B}, {NOTE_C5, D_05B}, {NOTE_C5, D_05B},
  // Ô 54 (Dm): lòng Rê5 - em Fa5 - khóc La5 - thương Rê5 - Rê5
  {NOTE_D5, D_10B}, {NOTE_F5, D_05B}, {NOTE_A5, D_10B}, {NOTE_D5, D_05B}, {NOTE_D5, D_10B},
  // Ô 55 (C): người Đô5 - sang Rê5 - đò Đô5 - Hồng La4
  {NOTE_C5, D_10B}, {NOTE_D5, D_05B}, {NOTE_C5, D_20B}, {NOTE_A4, D_05B},
  // Ô 56 (Bb): Nhan Rê5 - Bạc Đô5 - Phận Rê5 - Sóng Fa5
  {NOTE_D5, D_10B}, {NOTE_C5, D_05B}, {NOTE_D5, D_15B}, {NOTE_F5, D_10B},
  // Ô 57 (A): Gió Mi5 - Ôi Mi5 - phút Fa5 - giây Mi5 - tương Rê5
  {NOTE_E5, D_20B}, {NOTE_E5, D_05B}, {NOTE_F5, D_05B}, {NOTE_E5, D_05B}, {NOTE_D5, D_05B},

  // --- PHẦN 6: KHUNG KẾT SỐ 2 & OUTRO (Ô nhịp 58 - 73) ---
  // Ô 58 (Khung số 2 - Dm): em Đô5 - kiếp Mi5 - sau... Rê5 - Fa5
  {NOTE_C5, D_05B}, {NOTE_E5, D_10B}, {NOTE_D5, D_15B}, {NOTE_F5, D_10B},
  // Ô 59 (Bb): Sol5 - Fa5 - Mi5 - Rê5
  {NOTE_G5, D_15B}, {NOTE_F5, D_05B}, {NOTE_E5, D_10B}, {NOTE_D5, D_10B},
  // Ô 60 (C): Mi5 - Rê5 - Đô5
  {NOTE_E5, D_25B}, {NOTE_D5, D_10B}, {NOTE_C5, D_05B},
  // Ô 61 (Am): Rê5 - Mi5 - Sol4 - La4
  {NOTE_D5, D_25B}, {NOTE_E5, D_05B}, {NOTE_G4, D_05B}, {NOTE_A4, D_05B},
  // Ô 62 (Dm): La4 - Fa5
  {NOTE_A4, D_30B}, {NOTE_F5, D_10B},
  // Ô 63 (Bb): Sol5 - Fa5 - Mi5 - Rê5
  {NOTE_G5, D_15B}, {NOTE_F5, D_05B}, {NOTE_E5, D_10B}, {NOTE_D5, D_10B},
  // Ô 64 (C): Mi5 - Rê5 - Đô5
  {NOTE_E5, D_25B}, {NOTE_D5, D_10B}, {NOTE_C5, D_05B},
  // Ô 65 (Am): Rê5 - Mi5 - Sol4 - La4
  {NOTE_D5, D_25B}, {NOTE_E5, D_05B}, {NOTE_G4, D_05B}, {NOTE_A4, D_05B},
  // Ô 66 (Dm): Rê5 - La5
  {NOTE_D5, D_30B}, {NOTE_A5, D_10B},
  // Ô 67 (Bb): Sib5 - La5 - Sol5 - Fa5
  {NOTE_AS5, D_15B}, {NOTE_A5, D_05B}, {NOTE_G5, D_10B}, {NOTE_F5, D_10B},
  // Ô 68 (C): Sol5 - Fa5 - Mi5
  {NOTE_G5, D_25B}, {NOTE_F5, D_10B}, {NOTE_E5, D_05B},
  // Ô 69 (Am): Fa5 - Sol5 - La4 - Đô5
  {NOTE_F5, D_25B}, {NOTE_G5, D_05B}, {NOTE_A4, D_05B}, {NOTE_C5, D_05B},
  // Ô 70 (Dm): La4 - La5
  {NOTE_A4, D_30B}, {NOTE_A5, D_10B},
  // Ô 71 (Bb): Sib5 - La5 - Sol5 - Fa5
  {NOTE_AS5, D_15B}, {NOTE_A5, D_05B}, {NOTE_G5, D_10B}, {NOTE_F5, D_10B},
  // Ô 72 (C): Sol5 - Fa5 - Mi5 - Rê5
  {NOTE_G5, D_25B}, {NOTE_F5, D_05B}, {NOTE_E5, D_05B}, {NOTE_D5, D_05B},
  // Ô 73 (Am): Mi5 - Fa5 - Sol5 - La4 - La4 - Đô5
  {NOTE_E5, D_05B}, {NOTE_F5, D_15B}, {NOTE_G5, D_05B}, {NOTE_A4, D_05B}, {NOTE_A4, D_05B}, {NOTE_C5, D_05B},
  // Kết bài: Rê5 ngân dài rồi nghỉ
  {NOTE_D5, D_40B},
  {NOTE_REST, D_20B}
};
const size_t BAC_PHAN_MELODY_LEN = sizeof(bacPhanMelody) / sizeof(bacPhanMelody[0]);

// ============================================================================
// TASK CHUYÊN BIỆT CHO CÒI: NHỊP ĐIỆU CHUẨN XÁC TUYỆT ĐỐI (ZERO JITTER)
// ============================================================================
static void sleepAbortable(uint16_t totalMs) {
  uint16_t elapsed = 0;
  while (elapsed < totalMs) {
    bool active = false;
    if (sharedMode == 1) active = true;
    else if (sharedMode == 2 || isSnoozed) active = false;
    else active = (sharedAqiLevel == 2);

    if (!active) {
      noTone(BUZZER_PIN);
      digitalWrite(BUZZER_PIN, LOW);
      return;
    }
    uint16_t step = (totalMs - elapsed > 25) ? 25 : (totalMs - elapsed);
    vTaskDelay(pdMS_TO_TICKS(step));
    elapsed += step;
  }
}

void TaskBuzzerCode(void* pvParameters) {
  size_t idx = 0;
  for (;;) {
    bool shouldBeep = false;
    if (sharedMode == 1) {
      shouldBeep = true;
    } else if (sharedMode == 2 || isSnoozed) {
      shouldBeep = false;
    } else {
      shouldBeep = (sharedAqiLevel == 2);
    }

    if (shouldBeep) {
      sharedBuzzerActive = true;
      uint16_t f = bacPhanMelody[idx].freq;
      uint16_t dur = bacPhanMelody[idx].duration;

      if (f > 0) {
        // Tần số gốc chuẩn giai điệu Bạc Phận (Octave 4-5) + 50% Duty Cycle để âm thanh vang to, rõ ràng
        tone(BUZZER_PIN, f);
        // Ngắt 20ms ở cuối nốt để các nốt liên tiếp tách bạch rõ ràng, giai điệu nảy và giòn
        uint16_t playMs = (dur > 30) ? (dur - 20) : dur;
        uint16_t gapMs  = dur - playMs;
        sleepAbortable(playMs);

        noTone(BUZZER_PIN);
        digitalWrite(BUZZER_PIN, LOW);
        if (gapMs > 0) {
          sleepAbortable(gapMs);
        }
      } else {
        noTone(BUZZER_PIN);
        digitalWrite(BUZZER_PIN, LOW);
        sleepAbortable(dur);
      }

      idx = (idx + 1) % BAC_PHAN_MELODY_LEN;
    } else {
      if (sharedBuzzerActive || idx != 0) {
        noTone(BUZZER_PIN);
        digitalWrite(BUZZER_PIN, LOW);
        idx = 0;
        sharedBuzzerActive = false;
      }
      vTaskDelay(pdMS_TO_TICKS(40));
    }
  }
}

// ============================================================================
// 7. CORE 1 TASK: GIÁM SÁT CẢM BIẾN, OLED & ĐÈN BÁO
// ============================================================================
void TaskSensorCode(void* pvParameters) {
  unsigned long lastDhtTime  = 0;
  unsigned long lastOledTime = 0;
  unsigned long lastGasRead  = 0;

  for (;;) {
    unsigned long now = millis();

    // 1. Đọc MQ-135 liên tục mỗi 100ms
    if (now - lastGasRead >= 100 || lastGasRead == 0) {
      lastGasRead = now;
      int rawGas = analogRead(GAS_PIN);
      sharedGasLevel = rawGas;
      if (rawGas > sharedPeakGas) {
        sharedPeakGas = rawGas;
      }
    }

    // 2. Đọc DHT22 mỗi 2.5s
    if (now - lastDhtTime >= 2500 || lastDhtTime == 0) {
      lastDhtTime = now;
      float t = dht.readTemperature();
      float h = dht.readHumidity();
      if (!isnan(t) && !isnan(h)) {
        sharedTemp = t;
        sharedHumidity = h;
      }
    }

    // 3. Đánh giá chất lượng không khí với Debounce 30s (Tránh nhảy lên nhảy xuống liên tục)
    int rawAqi = 0;
    if (sharedGasLevel >= gasThresholdDanger) {
      rawAqi = 2; // Nguy hiểm (Đỏ, còi báo động)
    } else if (sharedGasLevel >= gasThresholdModerate) {
      rawAqi = 1; // Trung bình / cảnh báo nhẹ (Vàng)
    } else {
      rawAqi = 0; // Tốt / Không khí sạch (Xanh)
    }

    if (sharedTemp > 45.0 && rawAqi == 0) {
      rawAqi = 1;
    }

    static int pendingAqi = -1;
    static unsigned long pendingAqiStartTime = 0;

    if (rawAqi != sharedAqiLevel) {
      if (rawAqi != pendingAqi) {
        pendingAqi = rawAqi;
        pendingAqiStartTime = now;
      } else {
        // Khi khói tăng đột biến lên nguy hiểm: kích hoạt sau 2s để kịp cứu hỏa
        // Khi chuyển đổi trạng thái bình thường: giữ ổn định đủ 30 giây để tránh nhảy lên nhảy xuống
        unsigned long debounceMs = (rawAqi == 2) ? 2000 : 30000;
        if (now - pendingAqiStartTime >= debounceMs) {
          sharedAqiLevel = rawAqi;
          pendingAqi = -1;
        }
      }
    } else {
      pendingAqi = -1;
    }

    // 4. Quản lý trạng thái Tạm tắt còi (Snooze 30s)
    if (isSnoozed) {
      unsigned long elapsed = now - snoozeStartTime;
      if (elapsed >= SNOOZE_DURATION_MS) {
        isSnoozed = false;
        snoozeRemainingSec = 0;
        sharedMode = 0; // Hết 30 giây -> Trở lại Auto
        Serial.println("[SNOOZE] Het 30s! Tu dong khoi phuc AUTO kiem tra lai");
      } else {
        snoozeRemainingSec = (int)((SNOOZE_DURATION_MS - elapsed) / 1000);
        // Nếu trong thời gian tạm tắt mà không khí đã an toàn:
        if (sharedAqiLevel == 0) {
          isSnoozed = false;
          snoozeRemainingSec = 0;
          sharedMode = 0;
        }
      }
    } else {
      snoozeRemainingSec = 0;
      // Auto-Rearm thông thường khi hết khói
      if (sharedMode == 2 && sharedAqiLevel == 0) {
        sharedMode = 0;
      }
    }

    // 5. LED chỉ thị
    digitalWrite(LED_GREEN,  (sharedAqiLevel == 0) ? HIGH : LOW);
    digitalWrite(LED_YELLOW, (sharedAqiLevel == 1) ? HIGH : LOW);
    digitalWrite(LED_RED,    (sharedAqiLevel == 2) ? HIGH : LOW);

    // 6. Vẽ OLED mỗi 500ms
    if (now - lastOledTime >= 500) {
      lastOledTime = now;
      updateOLED();
    }

    // 7. Nút BOOT vật lý (GPIO 0): Nhấn giữ 3 giây để xóa sạch dữ liệu WiFi đã lưu
    if (digitalRead(BOOT_PIN) == LOW) {
      unsigned long pressStart = millis();
      int lastSec = -1;
      while (digitalRead(BOOT_PIN) == LOW) {
        int elapsedSec = (millis() - pressStart) / 1000;
        if (elapsedSec != lastSec && elapsedSec < 3) {
          lastSec = elapsedSec;
          u8g2.clearBuffer();
          u8g2.setFont(u8g2_font_helvB08_tr);
          u8g2.drawStr(10, 22, "GIU NUT BOOT...");
          char cntStr[32];
          snprintf(cntStr, sizeof(cntStr), "Xoa WiFi sau: %d giay", 3 - elapsedSec);
          u8g2.drawStr(5, 42, cntStr);
          u8g2.sendBuffer();
        }
        if (millis() - pressStart >= 3000) {
          u8g2.clearBuffer();
          u8g2.setFont(u8g2_font_helvB08_tr);
          u8g2.drawStr(5, 25, "DA XOA DATA WIFI!");
          u8g2.drawStr(5, 45, "Khoi dong lai AP...");
          u8g2.sendBuffer();

          tone(BUZZER_PIN, 1200); delay(100); noTone(BUZZER_PIN); delay(80);
          tone(BUZZER_PIN, 1800); delay(250); noTone(BUZZER_PIN);

          prefs.remove("wifi_ssid");
          prefs.remove("wifi_pass");
          currentSSID = "";
          currentPass = "";

          WiFi.disconnect(true, true);
          WiFi.softAPdisconnect(true);
          delay(800);
          ESP.restart();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
      }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ============================================================================
// 8. CORE 0 TASK: MẠNG, WEBSOCKET, WEB SERVER & SUPABASE CLOUD IOT
// ============================================================================
void TaskNetworkCode(void* pvParameters) {
  unsigned long lastSupabaseSend = 0;
  unsigned long lastSupabaseCheck = 0;
  unsigned long lastWiFiCheck = 0;
  unsigned long lastWsBroadcast = 0;

  for (;;) {
    unsigned long now = millis();

    // 1. Phục vụ Web Server & WebSocket (hoạt động ở cả STA và SoftAP)
    if (sharedWifiConnected || isAPMode) {
      server.handleClient();
      webSocket.loop();

      // Broadcast telemetry mỗi 200ms cho các client
      if (now - lastWsBroadcast >= 200) {
        lastWsBroadcast = now;
        if (webSocket.connectedClients() > 0) {
          char jsonBuf[384];
          buildTelemetryJson(jsonBuf, sizeof(jsonBuf));
          webSocket.broadcastTXT(jsonBuf);
        }
      }
    }

    // 2. Quản lý kết nối WiFi STA
    if (!isAPMode) {
      if (WiFi.status() == WL_CONNECTED) {
        if (!sharedWifiConnected) {
          sharedWifiConnected = true;
          currentIPStr = WiFi.localIP().toString();
          Serial.print("\r\n==================================================\r\n");
          Serial.print("[WIFI STA] DA KET NOI MANG THANH CONG!\r\n");
          Serial.print("[WIFI STA] DIA CHI IP ESP32 : ");
          Serial.println(currentIPStr);
          Serial.print("[WIFI STA] LINK TRUY CAP    : http://");
          Serial.println(currentIPStr);
          Serial.println("[WIFI STA] HOAC DUNG DOMAIN : http://esp32.local");
          Serial.print("==================================================\r\n\r\n");

          MDNS.begin("esp32");
          server.begin();
          webSocket.begin();
        }
      } else {
        sharedWifiConnected = false;
        // Nếu đã thử kết nối STA quá 15 giây mà không được -> Chuyển sang SoftAP
        if (now - wifiConnectStartTime >= WIFI_STA_TIMEOUT) {
          Serial.println("[WIFI STA] Khong the ket noi WiFi sau 15s! Chuyen sang SoftAP de cau hinh...");
          startAPMode();
        } else if (now - lastWiFiCheck >= 3000) {
          lastWiFiCheck = now;
          Serial.print(".");
        }
      }
    }

    // 3. Gửi Telemetry lên Supabase Cloud định kỳ 8s (khi có mạng Internet STA)
    if (sharedWifiConnected && !isAPMode && (now - lastSupabaseSend >= 8000 || lastSupabaseSend == 0)) {
      lastSupabaseSend = now;

      int gasToSend = (sharedPeakGas > 0) ? sharedPeakGas : sharedGasLevel;
      sharedPeakGas = sharedGasLevel;

      WiFiClientSecure client;
      client.setInsecure();
      client.setTimeout(3);

      HTTPClient http;
      String insertUrl = String(supabaseUrl) + "/rest/v1/air_telemetry";
      if (http.begin(client, insertUrl)) {
        http.addHeader("Content-Type", "application/json");
        http.addHeader("apikey", supabaseKey);
        http.addHeader("Authorization", String("Bearer ") + supabaseKey);
        http.addHeader("Prefer", "return=minimal");
        http.setTimeout(3000);

        char jsonPayload[320];
        snprintf(jsonPayload, sizeof(jsonPayload),
          "{\"temp\":%.1f,\"humidity\":%.1f,\"gas\":%d,\"peak\":%d,\"aqi\":%d,\"mode\":%d,\"ip\":\"%s\",\"danger_thresh\":%d,\"buzzer\":%s}",
          sharedTemp, sharedHumidity, gasToSend, sharedPeakGas, sharedAqiLevel, sharedMode,
          currentIPStr.c_str(), gasThresholdDanger, sharedBuzzerActive ? "true" : "false"
        );

        int httpCode = http.POST(jsonPayload);
        if (httpCode == HTTP_CODE_CREATED || httpCode == HTTP_CODE_OK || httpCode == 204) {
          Serial.printf("[SUPABASE] Day telemetry OK (Code %d) | Gas: %d | Thresh: %d\r\n", httpCode, gasToSend, gasThresholdDanger);
        } else {
          Serial.printf("[SUPABASE] Loi POST: %d\r\n", httpCode);
        }
        http.end();
      }
    }

    // 3b. Lưu dữ liệu vào SQLite/Local Cache khi không có mạng hoặc đang ở chế độ SoftAP (ARP)
    static unsigned long lastOfflineCacheLog = 0;
    if (!sharedWifiConnected || isAPMode) {
      if (now - lastOfflineCacheLog >= 15000 || lastOfflineCacheLog == 0) {
        lastOfflineCacheLog = now;
        int gasToSave = (sharedPeakGas > 0) ? sharedPeakGas : sharedGasLevel;
        appendSqlRecord(sharedTemp, sharedHumidity, gasToSave, sharedPeakGas, sharedAqiLevel, sharedMode, gasThresholdDanger, sharedBuzzerActive);
      }
    } else {
      // 3c. Đồng bộ dữ liệu từ SQLite/Local Cache lên Supabase khi có mạng
      static unsigned long lastSqlSync = 0;
      if (sqlCacheCount > 0 && (now - lastSqlSync >= 8000 || lastSqlSync == 0)) {
        lastSqlSync = now;
        syncSqlCacheToSupabase();
      }
    }

    // 4. Đọc lệnh điều khiển từ xa từ Supabase (air_controls) mỗi 3s
    static int lastCloudMode = -1;
    static bool lastCloudSnooze = false;
    static int lastCloudThresh = -1;
    if (sharedWifiConnected && !isAPMode && (now - lastSupabaseCheck >= 3000)) {
      lastSupabaseCheck = now;

      WiFiClientSecure client;
      client.setInsecure();
      client.setTimeout(2);

      HTTPClient http;
      String readUrl = String(supabaseUrl) + "/rest/v1/air_controls?id=eq.1&select=mode,snooze,danger_thresh";
      if (http.begin(client, readUrl)) {
        http.addHeader("apikey", supabaseKey);
        http.addHeader("Authorization", String("Bearer ") + supabaseKey);
        http.setTimeout(2000);

        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
          String payload = http.getString();
          int idxMode = payload.indexOf("\"mode\":");
          int idxSnooze = payload.indexOf("\"snooze\":");
          int idxThresh = payload.indexOf("\"danger_thresh\":");

          if (idxMode >= 0) {
            int newMode = payload.substring(idxMode + 7).toInt();
            bool newSnooze = (idxSnooze >= 0 && payload.substring(idxSnooze + 9).startsWith("true"));
            int newThresh = (idxThresh >= 0) ? payload.substring(idxThresh + 16).toInt() : gasThresholdDanger;

            if (lastCloudMode == -1) {
              lastCloudMode = newMode;
              lastCloudSnooze = newSnooze;
              lastCloudThresh = newThresh;
            } else if (newMode != lastCloudMode || newSnooze != lastCloudSnooze || (newThresh > 0 && newThresh != lastCloudThresh)) {
              lastCloudMode = newMode;
              lastCloudSnooze = newSnooze;
              lastCloudThresh = newThresh;

              if (newSnooze) {
                sharedMode = 2;
                isSnoozed = true;
                snoozeStartTime = millis();
                snoozeRemainingSec = 30;
              } else {
                sharedMode = newMode;
                isSnoozed = false;
                snoozeRemainingSec = 0;
              }
              if (newThresh >= 100 && newThresh <= 4000) {
                gasThresholdDanger = newThresh;
                gasThresholdModerate = newThresh / 2;
                prefs.putInt("thresh_dang", newThresh);
              }
              Serial.printf("[SUPABASE CONTROL] Cap nhat Cloud: Mode=%d, Snooze=%s, Thresh=%d\r\n",
                sharedMode, isSnoozed ? "true" : "false", gasThresholdDanger);
            }
          }
        }
        http.end();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ============================================================================
// 9. SETUP & LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(GAS_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(BOOT_PIN, INPUT_PULLUP);

  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_YELLOW, LOW);
  digitalWrite(LED_RED, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(21, 22);
  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(6, 15, "AIR MONITOR IoT");
  u8g2.drawHLine(0, 18, 128);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(8, 33, "Khoi dong NVS...");
  u8g2.drawStr(6, 47, "WiFi + Captive AP");
  u8g2.drawStr(10, 60, "OLED 0.96 SSD1306");
  u8g2.sendBuffer();

  // Khoi tao NVS Preferences & Doc WiFi da luu
  prefs.begin("air_cfg", false);
  currentSSID = prefs.getString("wifi_ssid", "");
  currentPass = prefs.getString("wifi_pass", "");
  gasThresholdDanger = prefs.getInt("thresh_dang", 500);
  gasThresholdModerate = gasThresholdDanger / 2; // Nguong trung binh = 1/2 nguong cao nhat

  dht.begin();

  // Khởi tạo LittleFS cho SQLite / Local Storage
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] Khoi tao LittleFS loi!");
  } else {
    initSqlCache();
  }

  // Đăng ký Web Server Endpoints
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/data", HTTP_GET, handleApiData);
  server.on("/api/cmd", HTTP_GET, handleApiCmd);
  server.on("/api/threshold", HTTP_GET, handleApiThreshold);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/wifi/save", HTTP_GET, handleWifiSave);
  server.on("/api/wifi/save", HTTP_POST, handleWifiSave);
  server.on("/api/wifi/reset", HTTP_GET, handleWifiReset);
  server.on("/api/wifi/reset", HTTP_POST, handleWifiReset);
  server.on("/api/sql-cache", HTTP_GET, handleApiSqlCache);
  server.on("/api/sql-clear", HTTP_POST, handleApiSqlClear);
  server.on("/api/sql-clear", HTTP_GET, handleApiSqlClear);
  server.onNotFound(handleNotFound);

  // Cấu hình sự kiện WebSocket
  webSocket.onEvent(webSocketEvent);

  // Khoi dong ket noi WiFi STA / SoftAP truoc khi kich hoat cac Task mang FreeRTOS
  tryConnectSTA(currentSSID, currentPass);

  // Khoi tao FreeRTOS Dual-Core & Dedicated Buzzer Task
  xTaskCreatePinnedToCore(TaskSensorCode, "TaskSensor", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskBuzzerCode, "TaskBuzzer", 3072, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskNetworkCode, "TaskNetwork", 8192, NULL, 1, NULL, 0);

  Serial.println("[SYSTEM] He thong Dual-Core khoi chay thanh cong!");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
