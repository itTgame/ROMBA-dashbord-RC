/*
  roomba_dashboard_api_fixed.ino
  Updated build with reliability improvements:
  - handleNotFound fix
  - sensor caching via sampleSensorsIfDue + cached handleSensors responses
  - queryPacketWithRetries with longer timeout and more attempts
  - ensureWifi(bool) with optional forced reconnect
  - optional API key support (Bearer / ?key=)
  - CORS + OPTIONS support
  - slower polling / increased timeouts (SENSOR_INTERVAL = 5000ms)
  - expanded Serial debug output
  Before upload: set WIFI_SSID / WIFI_PASS
*/

#include <WiFi.h>
#include <WebServer.h>
// English comment
// #include <ArduinoOTA.h>
// #include <ESPmDNS.h>

#define ROOMBA_RX 16
#define ROOMBA_TX 17

// English comment
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// English comment
// English comment
const uint32_t ROOMBA_BAUD = 115200;
uint16_t ROOMBA_READ_TIMEOUT_MS = 400; // English comment

HardwareSerial RoombaSerial(1);
WebServer server(80);

// English comment
uint32_t lastCmdAt = 0;
const char* lastCmd = "none";
bool roombaReady = false;

// English comment
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

// English comment
SensorSnapshot cachedSensors;
unsigned long lastSensorRead = 0;
const unsigned long SENSOR_INTERVAL = 5000UL; // English comment

// English comment
const char* API_KEY = ""; // English comment

// English comment
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

// English comment
void writeCmd(uint8_t opcode) {
  RoombaSerial.write(opcode);
  RoombaSerial.flush();
}

void sendRoombaCmd(uint8_t opcode, const char* label) {
  writeCmd(opcode);
  lastCmdAt = millis();
  lastCmd = label;
}

// English comment
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

// English comment
bool queryPacketWithRetries(uint8_t packetId, uint8_t* out, size_t len, int maxTries = 5, uint16_t timeoutMs = 400) {
  for (int attempt = 0; attempt < maxTries; ++attempt) {
// English comment
    while (RoombaSerial.available()) RoombaSerial.read();

// English comment
    RoombaSerial.write(142);
    RoombaSerial.write(packetId);
    RoombaSerial.flush();

// English comment
    delay(30);

    if (readExact(out, len, timeoutMs)) {
      return true;
    }

// English comment
    delay(40);
  }
  return false;
}

// English comment
bool queryU8(uint8_t packetId, uint8_t& value) {
  uint8_t b[1] = {0};
  if (!queryPacketWithRetries(packetId, b, 1, 5, ROOMBA_READ_TIMEOUT_MS)) return false;
  value = b[0];
  return true;
}

bool queryU16(uint8_t packetId, uint16_t& value) {
  uint8_t b[2] = {0, 0};
  if (!queryPacketWithRetries(packetId, b, 2, 5, ROOMBA_READ_TIMEOUT_MS)) return false;
  value = (static_cast<uint16_t>(b[0]) << 8) | b[1];
  return true;
}

bool queryI16(uint8_t packetId, int16_t& value) {
  uint16_t raw = 0;
  if (!queryU16(packetId, raw)) return false;
  value = static_cast<int16_t>(raw);
  return true;
}

// English comment
bool readRoombaSensors(SensorSnapshot& s) {
  uint8_t bumps = 0;

  if (!queryU8(21, s.chargingState)) return false; // English comment
  if (!queryI16(23, s.currentMa)) return false; // English comment
  if (!queryU16(22, s.batteryMv)) return false; // English comment
  if (!queryU8(18, s.buttons)) return false; // English comment
  if (!queryU8(7, bumps)) return false; // English comment

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

// English comment
void ensureRoombaReady() {
  if (roombaReady) return;

  writeCmd(128);  // START
  delay(120);
  writeCmd(131);  // SAFE
  delay(120);
  roombaReady = true;
  lastCmd = "safe";
  lastCmdAt = millis();
}

// English comment
void sampleSensorsIfDue() {
  unsigned long now = millis();
  if (now - lastSensorRead < SENSOR_INTERVAL) return;
  lastSensorRead = now;

  SensorSnapshot s;
  if (readRoombaSensors(s)) {
    cachedSensors = s;
    cachedSensors.ok = true;
    Serial.println("sampleSensorsIfDue: snapshot updated");
  } else {
// English comment
    cachedSensors.ok = false;
    Serial.println("Warning: sampleSensorsIfDue() failed to read sensors");
  }
}

// English comment
bool checkApiKey() {
  if (API_KEY == nullptr || API_KEY[0] == '\0') return true; // English comment

  // Authorization: Bearer <token>
  if (server.hasHeader("Authorization")) {
    String auth = server.header("Authorization");
    const String prefix = "Bearer ";
    if (auth.startsWith(prefix)) {
      String token = auth.substring(prefix.length());
      if (token.equals(API_KEY)) return true;
    }
  }
// English comment
  if (server.hasArg("key")) {
    if (server.arg("key") == String(API_KEY)) return true;
  }
  return false;
}

// English comment
void handleNotFound() {
  String path = server.uri();
  Serial.println("404: " + path);

  String body = "{";
  body += "\"ok\":false,";
  body += "\"error\":\"not_found\",";
  body += "\"path\":\"" + path + "\"";
  body += "}";
  sendJson(404, body);
}

// --- WiFi ensure with optional forced reconnect ---
void ensureWifi(bool forceReconnect) {
  if (!forceReconnect && WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (forceReconnect) {
    Serial.println("ensureWifi: forcing reconnect...");
    WiFi.disconnect(true, true);
    delay(200);
  } else {
    Serial.println("ensureWifi: checking connection...");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.print("Connecting to WiFi ");
    Serial.print(WIFI_SSID);
    Serial.print(" ...");
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    unsigned long start = millis();
    const unsigned long WIFI_TIMEOUT = 15000; // 15s
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_TIMEOUT) {
      delay(300);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("WiFi connected, IP: ");
      Serial.println(WiFi.localIP());
      WiFi.setHostname("roomba-dashboard");
// English comment
    } else {
      Serial.println("WiFi connect failed (will retry later).");
    }
  } else {
    Serial.print("WiFi already connected: ");
    Serial.println(WiFi.localIP());
  }
}

// English comment
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
  if (!checkApiKey()) {
    sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }

  handleCors();
  ensureRoombaReady();

// English comment
  if (!cachedSensors.ok) {
    SensorSnapshot s;
    if (readRoombaSensors(s)) {
      cachedSensors = s;
      cachedSensors.ok = true;
    } else {
      sendJson(503, "{\"ok\":false,\"error\":\"no_recent_snapshot\"}");
      return;
    }
  }

  String body = "{";
  body += "\"ok\":true,";
  body += "\"battery_mV\":" + String(cachedSensors.batteryMv) + ",";
  body += "\"charging_state\":" + String(cachedSensors.chargingState) + ",";
  body += "\"current_mA\":" + String(cachedSensors.currentMa) + ",";
  body += "\"buttons\":" + String(cachedSensors.buttons) + ",";
  body += "\"bump_left\":" + String(cachedSensors.bumperLeft ? "true" : "false") + ",";
  body += "\"bump_right\":" + String(cachedSensors.bumperRight ? "true" : "false") + ",";
  body += "\"bumper_left\":" + String(cachedSensors.bumperLeft ? "true" : "false") + ",";
  body += "\"bumper_right\":" + String(cachedSensors.bumperRight ? "true" : "false") + ",";
  body += "\"cliff_left\":" + String(cachedSensors.cliffLeft ? "true" : "false") + ",";
  body += "\"cliff_front_left\":" + String(cachedSensors.cliffFrontLeft ? "true" : "false") + ",";
  body += "\"cliff_front_right\":" + String(cachedSensors.cliffFrontRight ? "true" : "false") + ",";
  body += "\"cliff_right\":" + String(cachedSensors.cliffRight ? "true" : "false") + ",";
  body += "\"last_command\":\"" + String(lastCmd) + "\"";
  body += "}";

  server.send(200, "application/json", body);
}

void handleClean() {
  if (!checkApiKey()) { sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}"); return; }
  ensureRoombaReady();
  sendRoombaCmd(135, "clean");
  sendJson(200, "{\"ok\":true,\"action\":\"clean\"}");
}

void handleSpot() {
  if (!checkApiKey()) { sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}"); return; }
  ensureRoombaReady();
  sendRoombaCmd(134, "spot");
  sendJson(200, "{\"ok\":true,\"action\":\"spot\"}");
}

void handleSafe() {
  if (!checkApiKey()) { sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}"); return; }
  ensureRoombaReady();
  sendRoombaCmd(131, "safe");
  sendJson(200, "{\"ok\":true,\"action\":\"safe\"}");
}

void handleStop() {
  if (!checkApiKey()) { sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}"); return; }
  ensureRoombaReady();
  sendRoombaCmd(173, "stop");
  roombaReady = false;
  sendJson(200, "{\"ok\":true,\"action\":\"stop\"}");
}

// --- setup & loop ---
void setup() {
  Serial.begin(115200);

// English comment
  RoombaSerial.begin(ROOMBA_BAUD, SERIAL_8N1, ROOMBA_RX, ROOMBA_TX);
  RoombaSerial.setTimeout(500); // English comment

  delay(2000);
  ensureRoombaReady();

// English comment
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  unsigned long start = millis();
  const unsigned long SETUP_WIFI_TIMEOUT = 20000;
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < SETUP_WIFI_TIMEOUT) {
    delay(400);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. IP address: ");
    Serial.println(WiFi.localIP());
    WiFi.setHostname("roomba-dashboard");
  } else {
    Serial.println("Failed to connect during setup (can retry later).");
  }

// English comment
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/sensors", HTTP_GET, handleSensors);
  server.on("/api/clean", HTTP_POST, handleClean);
  server.on("/api/spot", HTTP_POST, handleSpot);
  server.on("/api/safe", HTTP_POST, handleSafe);
  server.on("/api/stop", HTTP_POST, handleStop);

// English comment
  server.on("/api/status", HTTP_OPTIONS, handleOptions);
  server.on("/api/sensors", HTTP_OPTIONS, handleOptions);
  server.on("/api/clean", HTTP_OPTIONS, handleOptions);
  server.on("/api/spot", HTTP_OPTIONS, handleOptions);
  server.on("/api/safe", HTTP_OPTIONS, handleOptions);
  server.on("/api/stop", HTTP_OPTIONS, handleOptions);

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP server ready.");

// English comment
  sampleSensorsIfDue();
}

void loop() {
  server.handleClient();
  sampleSensorsIfDue();
  ensureWifi(false);

// English comment
  // ArduinoOTA.handle();

// English comment
  delay(10);
}
