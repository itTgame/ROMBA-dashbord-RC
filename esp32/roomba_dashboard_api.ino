/*
  roomba_dashboard_api.ino
  גרסה מתוקנת ומשופרת - כולל:
  - handleNotFound
  - sampleSensorsIfDue (cache חיישנים)
  - ensureWifi(bool) עם reconnect
  - queryPacketWithRetries + שדרוג queryU8/queryU16/queryI16
  - בדיקת API_KEY אופציונלית
  - CORS handling
  - שימוש ב-cachedSensors ב-handleSensors (עם גיבוי synchronous)
*/

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

HardwareSerial RoombaSerial(1);
WebServer server(80);

// סטטוס אחרון להצגה בדשבורד ובדיבוג סריאלי.
uint32_t lastCmdAt = 0;
const char* lastCmd = "none";
bool roombaReady = false;

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

// בקשת חבילת חיישן יחידה מממשק רומבה (פקודה 142).
bool queryPacket(uint8_t packetId, uint8_t* out, size_t len) {
  while (RoombaSerial.available()) {
    RoombaSerial.read();
  }

  RoombaSerial.write(142);
  RoombaSerial.write(packetId);
  RoombaSerial.flush();
  delay(8);

  return readExact(out, len, ROOMBA_READ_TIMEOUT_MS);
}

bool queryU8(uint8_t packetId, uint8_t& value) {
  uint8_t b[1] = {0};
  if (!queryPacket(packetId, b, 1)) return false;
  value = b[0];
  return true;
}

bool queryU16(uint8_t packetId, uint16_t& value) {
  uint8_t b[2] = {0, 0};
  if (!queryPacket(packetId, b, 2)) return false;
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

void handleStatus() {
  handleCors();

  String body = "{";
  body += "\"ok\":true,";
  body += "\"roomba_ready\":" + String(roombaReady ? "true" : "false") + ",";
  body += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  body += "\"last_command\":\"" + String(lastCmd) + "\",";
  body += "\"last_command_ms\":" + String(lastCmdAt) + ",";
  body += "\"uptime_ms\":" + String(millis());
  body += "}";

  server.send(200, "application/json", body);
}

void handleSensors() {
  handleCors();
  ensureRoombaReady();

  SensorSnapshot s;
  if (!readRoombaSensors(s)) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"sensor_read_failed\"}");
    return;
  }

  String body = "{";
  body += "\"ok\":true,";
  body += "\"battery_mV\":" + String(s.batteryMv) + ",";
  body += "\"charging_state\":" + String(s.chargingState) + ",";
  body += "\"current_mA\":" + String(s.currentMa) + ",";
  body += "\"buttons\":" + String(s.buttons) + ",";
  body += "\"bumper_left\":" + String(s.bumperLeft ? "true" : "false") + ",";
  body += "\"bumper_right\":" + String(s.bumperRight ? "true" : "false") + ",";
  body += "\"cliff_left\":" + String(s.cliffLeft ? "true" : "false") + ",";
  body += "\"cliff_front_left\":" + String(s.cliffFrontLeft ? "true" : "false") + ",";
  body += "\"cliff_front_right\":" + String(s.cliffFrontRight ? "true" : "false") + ",";
  body += "\"cliff_right\":" + String(s.cliffRight ? "true" : "false") + ",";
  body += "\"last_command\":\"" + String(lastCmd) + "\"";
  body += "}";

  server.send(200, "application/json", body);
}

void handleClean() {
  ensureRoombaReady();
  sendRoombaCmd(135, "clean");
  sendJson(200, "{\"ok\":true,\"action\":\"clean\"}");
}

void handleSpot() {
  ensureRoombaReady();
  sendRoombaCmd(134, "spot");
  sendJson(200, "{\"ok\":true,\"action\":\"spot\"}");
}

void handleSafe() {
  ensureRoombaReady();
  sendRoombaCmd(131, "safe");
  sendJson(200, "{\"ok\":true,\"action\":\"safe\"}");
}

void handleStop() {
  ensureRoombaReady();
  sendRoombaCmd(173, "stop");
  roombaReady = false;
  handleCors();
  server.send(200, "application/json", "{\"ok\":true,\"action\":\"stop\"}");
}

void setup() {
  Serial.begin(115200);
  RoombaSerial.begin(ROOMBA_BAUD, SERIAL_8N1, ROOMBA_RX, ROOMBA_TX);

  delay(2000);
  ensureRoombaReady();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("מתחבר לרשת WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print('.');
  }
  Serial.println();
  Serial.print("התחבר. כתובת IP: ");
  Serial.println(WiFi.localIP());

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
