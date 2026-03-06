#include <WiFi.h>
#include <WebServer.h>

#define ROOMBA_RX 16
#define ROOMBA_TX 17

// עדכן לפי הרשת שלך
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

HardwareSerial RoombaSerial(1);
WebServer server(80);

// מצב בסיסי להצגה בדשבורד
uint32_t lastCmdAt = 0;
const char* lastCmd = "none";

int fakeBatteryMv = 15840;
int fakeCurrentMa = -420;
int fakeChargingState = 2;
int fakeButtons = 0;
bool fakeBumperLeft = false;
bool fakeBumperRight = false;
bool fakeCliff = false;

void sendRoombaCmd(uint8_t opcode, const char* label) {
  RoombaSerial.write(opcode);
  lastCmdAt = millis();
  lastCmd = label;
}

void handleCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleOptions() {
  handleCors();
  server.send(204);
}

void handleStatus() {
  handleCors();
  String body = "{";
  body += "\"ok\":true,";
  body += "\"mode\":\"safe\",";
  body += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  body += "\"last_command\":\"" + String(lastCmd) + "\",";
  body += "\"uptime_ms\":" + String(millis());
  body += "}";
  server.send(200, "application/json", body);
}

void handleSensors() {
  // דוגמה דינמית בסיסית; אפשר להחליף בקריאה אמיתית מחיישני רומבה
  if (millis() % 5000 < 50) {
    fakeBumperLeft = !fakeBumperLeft;
  }

  handleCors();
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
  server.send(200, "application/json", body);
}

void handleClean() {
  sendRoombaCmd(135, "clean");
  handleCors();
  server.send(200, "application/json", "{\"ok\":true,\"action\":\"clean\"}");
}

void handleSpot() {
  sendRoombaCmd(134, "spot");
  handleCors();
  server.send(200, "application/json", "{\"ok\":true,\"action\":\"spot\"}");
}

void handleSafe() {
  sendRoombaCmd(131, "safe");
  handleCors();
  server.send(200, "application/json", "{\"ok\":true,\"action\":\"safe\"}");
}

void handleStop() {
  sendRoombaCmd(173, "stop");
  handleCors();
  server.send(200, "application/json", "{\"ok\":true,\"action\":\"stop\"}");
}

void setup() {
  Serial.begin(115200);

  RoombaSerial.begin(115200, SERIAL_8N1, ROOMBA_RX, ROOMBA_TX);
  delay(2000);

  // אתחול Roomba
  RoombaSerial.write(128);   // START
  delay(100);
  RoombaSerial.write(131);   // SAFE MODE
  delay(100);

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

  // API routes שדשבורד מצפה להם
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/sensors", HTTP_GET, handleSensors);

  server.on("/api/clean", HTTP_POST, handleClean);
  server.on("/api/spot", HTTP_POST, handleSpot);
  server.on("/api/safe", HTTP_POST, handleSafe);
  server.on("/api/stop", HTTP_POST, handleStop);

  // CORS preflight
  server.on("/api/status", HTTP_OPTIONS, handleOptions);
  server.on("/api/sensors", HTTP_OPTIONS, handleOptions);
  server.on("/api/clean", HTTP_OPTIONS, handleOptions);
  server.on("/api/spot", HTTP_OPTIONS, handleOptions);
  server.on("/api/safe", HTTP_OPTIONS, handleOptions);
  server.on("/api/stop", HTTP_OPTIONS, handleOptions);

  server.begin();
  Serial.println("HTTP API ready");
}

void loop() {
  server.handleClient();
}
