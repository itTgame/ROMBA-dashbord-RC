/*
  ============================================================
  ESP32 + Roomba (SCI) + REST API for Roomba Pro Dashboard
  ============================================================
  סקיצה זו תואמת אחד-לאחד ל-frontend (app.js):
  - GET  /api/status
  - GET  /api/sensors
  - POST /api/clean
  - POST /api/spot
  - POST /api/safe
  - POST /api/stop

  מטרת הסקיצה:
  1) להתחבר ל-WiFi
  2) לייצר API ל-Web Dashboard
  3) לשלוח פקודות SCI בסיסיות לרומבה דרך UART1

  הערות חשובות:
  - זהו בסיס יציב לשילוב. את קריאת החיישנים האמיתית מה-Roomba אפשר להרחיב בהמשך.
  - כרגע חלק מערכי החיישנים מדומים (fake) כדי לאפשר בדיקה מלאה מול הדשבורד.
*/

#include <WiFi.h>
#include <WebServer.h>

// -------------------- Pin Mapping --------------------
#define ROOMBA_RX 16
#define ROOMBA_TX 17

// -------------------- WiFi Credentials --------------------
// החלף לערכים שלך לפני העלאה
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// -------------------- Roomba SCI Opcodes --------------------
const uint8_t ROOMBA_START = 128;
const uint8_t ROOMBA_SAFE  = 131;
const uint8_t ROOMBA_SPOT  = 134;
const uint8_t ROOMBA_CLEAN = 135;
const uint8_t ROOMBA_STOP  = 173;

HardwareSerial RoombaSerial(1);
WebServer server(80);

// -------------------- Runtime State --------------------
uint32_t lastCmdAt = 0;
const char* lastCmd = "none";

// ערכים לדמו (ניתנים להחלפה בקריאה אמיתית מחיישנים)
int fakeBatteryMv = 15840;
int fakeCurrentMa = -420;
int fakeChargingState = 2;
int fakeButtons = 0;
bool fakeBumperLeft = false;
bool fakeBumperRight = false;
bool fakeCliff = false;

// -------------------- Utilities --------------------
void handleCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void sendJson(int code, const String& payload) {
  handleCors();
  server.send(code, "application/json", payload);
}

void handleOptions() {
  handleCors();
  server.send(204);
}

void sendRoombaCmd(uint8_t opcode, const char* label) {
  RoombaSerial.write(opcode);
  lastCmdAt = millis();
  lastCmd = label;
}

// -------------------- API Handlers --------------------
void handleStatus() {
  String body = "{";
  body += "\"ok\":true,";
  body += "\"mode\":\"safe\",";
  body += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  body += "\"last_command\":\"" + String(lastCmd) + "\",";
  body += "\"last_command_ms\":" + String(lastCmdAt) + ",";
  body += "\"uptime_ms\":" + String(millis());
  body += "}";
  sendJson(200, body);
}

void handleSensors() {
  // דינמיות פשוטה לדמו: מחליף bumper_left כל כמה שניות
  if (millis() % 5000 < 50) {
    fakeBumperLeft = !fakeBumperLeft;
  }

  String body = "{";
  body += "\"battery_mV\":" + String(fakeBatteryMv) + ",";
  body += "\"charging_state\":" + String(fakeChargingState) + ",";
  body += "\"current_mA\":" + String(fakeCurrentMa) + ",";
  body += "\"buttons\":" + String(fakeButtons) + ",";
  body += "\"bumper_left\":" + String(fakeBumperLeft ? "true" : "false") + ",";
  body += "\"bumper_right\":" + String(fakeBumperRight ? "true" : "false") + ",";
  body += "\"cliff\":" + String(fakeCliff ? "true" : "false") + ",";
  body += "\"last_command\":\"" + String(lastCmd) + "\"";
  body += "}";
  sendJson(200, body);
}

void handleClean() {
  sendRoombaCmd(ROOMBA_CLEAN, "clean");
  sendJson(200, "{\"ok\":true,\"action\":\"clean\"}");
}

void handleSpot() {
  sendRoombaCmd(ROOMBA_SPOT, "spot");
  sendJson(200, "{\"ok\":true,\"action\":\"spot\"}");
}

void handleSafe() {
  sendRoombaCmd(ROOMBA_SAFE, "safe");
  sendJson(200, "{\"ok\":true,\"action\":\"safe\"}");
}

void handleStop() {
  sendRoombaCmd(ROOMBA_STOP, "stop");
  sendJson(200, "{\"ok\":true,\"action\":\"stop\"}");
}

void handleNotFound() {
  sendJson(404, "{\"ok\":false,\"error\":\"not_found\"}");
}

// -------------------- Setup --------------------
void setup() {
  Serial.begin(115200);

  // אתחול UART לרומבה
  RoombaSerial.begin(115200, SERIAL_8N1, ROOMBA_RX, ROOMBA_TX);
  delay(2000); // נותן זמן ל-ESP לעלות יציב

  // אתחול Roomba למצב START + SAFE (לא מתחיל ניקוי אוטומטית)
  RoombaSerial.write(ROOMBA_START);
  delay(100);
  RoombaSerial.write(ROOMBA_SAFE);
  delay(100);

  // התחברות ל-WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print('.');
  }
  Serial.println();
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());

  // API routes שהדשבורד מצפה להן
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/sensors", HTTP_GET, handleSensors);
  server.on("/api/clean", HTTP_POST, handleClean);
  server.on("/api/spot", HTTP_POST, handleSpot);
  server.on("/api/safe", HTTP_POST, handleSafe);
  server.on("/api/stop", HTTP_POST, handleStop);

  // תמיכה ב-CORS Preflight
  server.on("/api/status", HTTP_OPTIONS, handleOptions);
  server.on("/api/sensors", HTTP_OPTIONS, handleOptions);
  server.on("/api/clean", HTTP_OPTIONS, handleOptions);
  server.on("/api/spot", HTTP_OPTIONS, handleOptions);
  server.on("/api/safe", HTTP_OPTIONS, handleOptions);
  server.on("/api/stop", HTTP_OPTIONS, handleOptions);

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP API ready");
}

// -------------------- Main Loop --------------------
void loop() {
  // טיפול בבקשות HTTP נכנסות
  server.handleClient();
}
