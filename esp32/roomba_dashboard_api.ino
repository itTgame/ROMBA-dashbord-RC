#include <WiFi.h>
#include <WebServer.h>

#define ROOMBA_RX 16
#define ROOMBA_TX 17

// החלף בפרטי הרשת המקומית לפני העלאה ל-ESP32.
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// מהירות תקשורת סטנדרטית לממשק הפקודות של רומבה.
const uint32_t ROOMBA_BAUD = 115200;
const uint16_t ROOMBA_READ_TIMEOUT_MS = 120;

// קצב דגימת חיישנים ברקע (cache) ותדירות בדיקת Wi-Fi.
const uint32_t SENSOR_SAMPLE_MS = 2000;
const uint32_t WIFI_RETRY_MS = 10000;

// אבטחה בסיסית אופציונלית: השאר ריק כדי לבטל דרישת מפתח.
const char* API_KEY = "";

HardwareSerial RoombaSerial(1);
WebServer server(80);

// סטטוס אחרון להצגה בדשבורד ובדיבוג סריאלי.
uint32_t lastCmdAt = 0;
const char* lastCmd = "none";
bool roombaReady = false;

uint32_t lastWifiRetryAt = 0;
uint32_t lastSampleAt = 0;
uint32_t lastSensorSuccessAt = 0;
uint8_t consecutiveSensorFailures = 0;

// הצהרות פרוטוטייפ מפורשות כדי למנוע כשלים בהפקת פרוטוטייפ אוטומטית בסביבות Arduino מסוימות.
void handleNotFound();
void sampleSensorsIfDue();
void ensureWifi(bool forceNow = false);

struct SensorSnapshot {
  bool ok = false;
  uint8_t chargingState = 0;
  int16_t currentMa = 0;
  uint16_t batteryMv = 0;
  uint8_t buttons = 0;
  bool bumperLeft = false;
  bool bumperRight = false;
  bool cliffLeft = false;
  bool cliffFrontLeft = false;
  bool cliffFrontRight = false;
  bool cliffRight = false;
};

// מטמון snapshot שמוחזר ל-HTTP בלי להעמיס את קריאות הסריאל בזמן בקשה.
SensorSnapshot lastSnapshot;

void handleCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type,Authorization");
}

void sendJson(int code, const String& body) {
  handleCors();
  server.send(code, "application/json", body);
}

void handleOptions() {
  handleCors();
  server.send(204);
}

bool isAuthEnabled() {
  return API_KEY != nullptr && strlen(API_KEY) > 0;
}

bool checkApiKey() {
  if (!isAuthEnabled()) return true;
  if (!server.hasHeader("Authorization")) return false;

  String auth = server.header("Authorization");
  if (!auth.startsWith("Bearer ")) return false;
  String token = auth.substring(7);
  return token == API_KEY;
}

bool rejectIfUnauthorized() {
  if (checkApiKey()) return false;
  sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
  return true;
}

void handleNotFound() {
  Serial.println("404: " + server.uri());

  String body = "{";
  body += "\"ok\":false,";
  body += "\"error\":\"not_found\",";
  body += "\"path\":\"" + server.uri() + "\"";
  body += "}";

  sendJson(404, body);
}

void writeCmd(uint8_t opcode) {
  RoombaSerial.write(opcode);
  RoombaSerial.flush();
}

void sendRoombaCmd(uint8_t opcode, const char* label) {
  writeCmd(opcode);
  lastCmdAt = millis();
  lastCmd = label;
}

// קריאת מספר בתים מדויק מהפורט הסריאלי עם מגבלת זמן קצרה.
bool readExact(uint8_t* buffer, size_t len, uint16_t timeoutMs) {
  uint32_t start = millis();
  size_t i = 0;
  while (i < len && (millis() - start) < timeoutMs) {
    if (RoombaSerial.available()) {
      buffer[i++] = static_cast<uint8_t>(RoombaSerial.read());
    }
  }
  return i == len;
}

// קריאת חבילת חיישן עם נסיונות חוזרים לאמינות גבוהה יותר.
bool queryPacketWithRetries(uint8_t packetId, uint8_t* out, size_t len, int maxTries = 3) {
  for (int attempt = 0; attempt < maxTries; ++attempt) {
    while (RoombaSerial.available()) {
      RoombaSerial.read();
    }

    RoombaSerial.write(142);
    RoombaSerial.write(packetId);
    RoombaSerial.flush();
    delay(8);

    if (readExact(out, len, ROOMBA_READ_TIMEOUT_MS)) {
      return true;
    }

    delay(15);
  }
  return false;
}

bool queryU8(uint8_t packetId, uint8_t& value) {
  uint8_t b[1] = {0};
  if (!queryPacketWithRetries(packetId, b, 1, 3)) return false;
  value = b[0];
  return true;
}

bool queryU16(uint8_t packetId, uint16_t& value) {
  uint8_t b[2] = {0, 0};
  if (!queryPacketWithRetries(packetId, b, 2, 3)) return false;
  value = (static_cast<uint16_t>(b[0]) << 8) | b[1];
  return true;
}

bool queryI16(uint8_t packetId, int16_t& value) {
  uint16_t raw = 0;
  if (!queryU16(packetId, raw)) return false;
  value = static_cast<int16_t>(raw);
  return true;
}

bool readRoombaSensors(SensorSnapshot& s) {
  uint8_t bumps = 0;

  if (!queryU8(21, s.chargingState)) return false;   // מצב טעינה
  if (!queryI16(23, s.currentMa)) return false;      // זרם טעינה/פריקה
  if (!queryU16(22, s.batteryMv)) return false;      // מתח סוללה במיליוולט
  if (!queryU8(18, s.buttons)) return false;         // כפתורים
  if (!queryU8(7, bumps)) return false;              // באמפרים/גלגלים

  uint8_t cliffLeft = 0;
  uint8_t cliffFrontLeft = 0;
  uint8_t cliffFrontRight = 0;
  uint8_t cliffRight = 0;

  if (!queryU8(9, cliffLeft)) return false;
  if (!queryU8(10, cliffFrontLeft)) return false;
  if (!queryU8(11, cliffFrontRight)) return false;
  if (!queryU8(12, cliffRight)) return false;

  s.bumperRight = (bumps & 0x01) != 0;
  s.bumperLeft = (bumps & 0x02) != 0;
  s.cliffLeft = cliffLeft != 0;
  s.cliffFrontLeft = cliffFrontLeft != 0;
  s.cliffFrontRight = cliffFrontRight != 0;
  s.cliffRight = cliffRight != 0;
  s.ok = true;
  return true;
}

void ensureRoombaReady() {
  if (roombaReady) return;

  writeCmd(128);  // כניסה למצב התחלתי של ממשק רומבה.
  delay(100);
  writeCmd(131);  // מעבר למצב בטוח להפעלת פקודות.
  delay(100);
  roombaReady = true;
  lastCmd = "safe";
  lastCmdAt = millis();
}

void ensureWifi(bool forceNow) {
  if (WiFi.status() == WL_CONNECTED) return;

  uint32_t now = millis();
  if (!forceNow && (now - lastWifiRetryAt) < WIFI_RETRY_MS) return;
  lastWifiRetryAt = now;

  Serial.println("WiFi מנותק, מנסה התחברות מחדש...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  const uint32_t WIFI_TIMEOUT = 10000;
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi התחבר מחדש: " + WiFi.localIP().toString());
  } else {
    Serial.println("ניסיון התחברות WiFi נכשל, ננסה שוב בהמשך.");
  }
}

// דוגם חיישנים ברקע ושומר snapshot. HTTP רק מחזיר cache מוכן.
void sampleSensorsIfDue() {
  uint32_t now = millis();
  if (now - lastSampleAt < SENSOR_SAMPLE_MS) return;
  lastSampleAt = now;

  ensureRoombaReady();

  SensorSnapshot s;
  const int MAX_TRIES = 2;
  bool ok = false;
  for (int t = 0; t < MAX_TRIES; ++t) {
    if (readRoombaSensors(s)) {
      ok = true;
      break;
    }
    delay(20);
  }

  if (ok) {
    lastSnapshot = s;
    lastSnapshot.ok = true;
    lastSensorSuccessAt = now;
    consecutiveSensorFailures = 0;
    return;
  }

  // שומרים last known value אבל מסמנים שה-snapshot הנוכחי לא עדכני.
  consecutiveSensorFailures++;
  if (consecutiveSensorFailures >= 3) {
    lastSnapshot.ok = false;
  }
}

bool isCommandRateLimited() {
  uint32_t now = millis();
  return (now - lastCmdAt) < 250;
}

void handleStatus() {
  if (rejectIfUnauthorized()) return;

  String body = "{";
  body += "\"ok\":true,";
  body += "\"roomba_ready\":" + String(roombaReady ? "true" : "false") + ",";
  body += "\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  body += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  body += "\"last_command\":\"" + String(lastCmd) + "\",";
  body += "\"last_command_ms\":" + String(lastCmdAt) + ",";
  body += "\"last_sensor_success_ms\":" + String(lastSensorSuccessAt) + ",";
  body += "\"uptime_ms\":" + String(millis());
  body += "}";

  sendJson(200, body);
}

void handleSensors() {
  if (rejectIfUnauthorized()) return;

  if (!lastSnapshot.ok) {
    sendJson(503, "{\"ok\":false,\"error\":\"no_recent_snapshot\"}");
    return;
  }

  String body = "{";
  body += "\"ok\":true,";
  body += "\"battery_mV\":" + String(lastSnapshot.batteryMv) + ",";
  body += "\"charging_state\":" + String(lastSnapshot.chargingState) + ",";
  body += "\"current_mA\":" + String(lastSnapshot.currentMa) + ",";
  body += "\"buttons\":" + String(lastSnapshot.buttons) + ",";
  body += "\"bumper_left\":" + String(lastSnapshot.bumperLeft ? "true" : "false") + ",";
  body += "\"bumper_right\":" + String(lastSnapshot.bumperRight ? "true" : "false") + ",";
  body += "\"cliff_left\":" + String(lastSnapshot.cliffLeft ? "true" : "false") + ",";
  body += "\"cliff_front_left\":" + String(lastSnapshot.cliffFrontLeft ? "true" : "false") + ",";
  body += "\"cliff_front_right\":" + String(lastSnapshot.cliffFrontRight ? "true" : "false") + ",";
  body += "\"cliff_right\":" + String(lastSnapshot.cliffRight ? "true" : "false") + ",";
  body += "\"last_command\":\"" + String(lastCmd) + "\"";
  body += "}";

  sendJson(200, body);
}

void handleClean() {
  if (rejectIfUnauthorized()) return;
  if (isCommandRateLimited()) {
    sendJson(429, "{\"ok\":false,\"error\":\"rate_limited\"}");
    return;
  }

  ensureRoombaReady();
  sendRoombaCmd(135, "clean");
  sendJson(200, "{\"ok\":true,\"action\":\"clean\"}");
}

void handleSpot() {
  if (rejectIfUnauthorized()) return;
  if (isCommandRateLimited()) {
    sendJson(429, "{\"ok\":false,\"error\":\"rate_limited\"}");
    return;
  }

  ensureRoombaReady();
  sendRoombaCmd(134, "spot");
  sendJson(200, "{\"ok\":true,\"action\":\"spot\"}");
}

void handleSafe() {
  if (rejectIfUnauthorized()) return;
  if (isCommandRateLimited()) {
    sendJson(429, "{\"ok\":false,\"error\":\"rate_limited\"}");
    return;
  }

  ensureRoombaReady();
  sendRoombaCmd(131, "safe");
  sendJson(200, "{\"ok\":true,\"action\":\"safe\"}");
}

void handleStop() {
  if (rejectIfUnauthorized()) return;
  if (isCommandRateLimited()) {
    sendJson(429, "{\"ok\":false,\"error\":\"rate_limited\"}");
    return;
  }

  ensureRoombaReady();
  sendRoombaCmd(173, "stop");
  roombaReady = false;
  sendJson(200, "{\"ok\":true,\"action\":\"stop\"}");
}

void setup() {
  Serial.begin(115200);
  RoombaSerial.begin(ROOMBA_BAUD, SERIAL_8N1, ROOMBA_RX, ROOMBA_TX);
  delay(2000);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("roomba-dashboard");
  ensureWifi(true);

  ensureRoombaReady();
  sampleSensorsIfDue();

  // הנתיבים שהדשבורד צורך לצורך סנכרון מלא.
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/sensors", HTTP_GET, handleSensors);
  server.on("/api/clean", HTTP_POST, handleClean);
  server.on("/api/spot", HTTP_POST, handleSpot);
  server.on("/api/safe", HTTP_POST, handleSafe);
  server.on("/api/stop", HTTP_POST, handleStop);

  // מענה CORS עבור דפדפנים (כולל GitHub Pages ו-CloudPhone).
  server.on("/api/status", HTTP_OPTIONS, handleOptions);
  server.on("/api/sensors", HTTP_OPTIONS, handleOptions);
  server.on("/api/clean", HTTP_OPTIONS, handleOptions);
  server.on("/api/spot", HTTP_OPTIONS, handleOptions);
  server.on("/api/safe", HTTP_OPTIONS, handleOptions);
  server.on("/api/stop", HTTP_OPTIONS, handleOptions);

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("שרת HTTP מוכן.");
}

void loop() {
  server.handleClient();
  sampleSensorsIfDue();
  ensureWifi(false);
}
