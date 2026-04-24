/*
  ESP32 REST bridge for a Roomba over the Open Interface serial port.

  What this sketch does:
  - boots the Roomba into START + SAFE mode
  - exposes a small HTTP API for status, sensors, and basic actions
  - caches sensor snapshots so the dashboard can poll without hammering UART
  - optionally protects mutating endpoints with an API key

  Before uploading:
  - set WIFI_SSID and WIFI_PASS
  - confirm UART wiring to the Roomba OI port
*/

#include <WiFi.h>
#include <WebServer.h>

namespace {

constexpr int ROOMBA_RX = 16;
constexpr int ROOMBA_TX = 17;

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* WIFI_HOSTNAME = "roomba-dashboard";
const char* API_KEY = "";

constexpr uint32_t ROOMBA_BAUD = 115200;
constexpr uint16_t ROOMBA_READ_TIMEOUT_MS = 400;
constexpr uint8_t ROOMBA_QUERY_LIST_OPCODE = 142;
constexpr uint8_t ROOMBA_START_OPCODE = 128;
constexpr uint8_t ROOMBA_SAFE_OPCODE = 131;
constexpr uint8_t ROOMBA_SPOT_OPCODE = 134;
constexpr uint8_t ROOMBA_CLEAN_OPCODE = 135;
constexpr uint8_t ROOMBA_STOP_OPCODE = 173;

constexpr unsigned long SENSOR_INTERVAL_MS = 5000UL;
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000UL;
constexpr unsigned long WIFI_SETUP_TIMEOUT_MS = 20000UL;
constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 10000UL;

constexpr uint8_t PACKET_BUMPS_AND_WHEEL_DROPS = 7;
constexpr uint8_t PACKET_CLIFF_LEFT = 9;
constexpr uint8_t PACKET_CLIFF_FRONT_LEFT = 10;
constexpr uint8_t PACKET_CLIFF_FRONT_RIGHT = 11;
constexpr uint8_t PACKET_CLIFF_RIGHT = 12;
constexpr uint8_t PACKET_BUTTONS = 18;
constexpr uint8_t PACKET_CHARGING_STATE = 21;
constexpr uint8_t PACKET_VOLTAGE = 22;
constexpr uint8_t PACKET_CURRENT = 23;

constexpr int SENSOR_QUERY_RETRIES = 5;
constexpr uint16_t QUERY_RETRY_DELAY_MS = 40;
constexpr uint16_t QUERY_RESPONSE_DELAY_MS = 30;
constexpr uint16_t ROOMBA_WAKE_DELAY_MS = 120;

HardwareSerial roombaSerial(1);
WebServer server(80);

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

SensorSnapshot cachedSensors;
unsigned long lastSensorReadAt = 0;
unsigned long lastWifiAttemptAt = 0;
unsigned long lastCommandAt = 0;
const char* lastCommand = "none";
bool roombaReady = false;

void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type,Authorization");
}

void sendJson(int code, const String& body) {
  addCorsHeaders();
  server.send(code, "application/json", body);
}

void sendUnauthorized() {
  sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
}

void sendActionResponse(const char* action) {
  char body[48];
  snprintf(body, sizeof(body), "{\"ok\":true,\"action\":\"%s\"}", action);
  sendJson(200, body);
}

String buildStatusJson() {
  String body;
  body.reserve(128);
  body += F("{\"ok\":true,\"roomba_ready\":");
  body += roombaReady ? F("true") : F("false");
  body += F(",\"ip\":\"");
  body += WiFi.localIP().toString();
  body += F("\",\"last_command\":\"");
  body += lastCommand;
  body += F("\",\"last_command_ms\":");
  body += String(lastCommandAt);
  body += F(",\"uptime_ms\":");
  body += String(millis());
  body += '}';
  return body;
}

String buildSensorJson(const SensorSnapshot& snapshot) {
  String body;
  body.reserve(320);
  body += F("{\"ok\":true,\"battery_mV\":");
  body += String(snapshot.batteryMv);
  body += F(",\"charging_state\":");
  body += String(snapshot.chargingState);
  body += F(",\"current_mA\":");
  body += String(snapshot.currentMa);
  body += F(",\"buttons\":");
  body += String(snapshot.buttons);
  body += F(",\"bump_left\":");
  body += snapshot.bumperLeft ? F("true") : F("false");
  body += F(",\"bump_right\":");
  body += snapshot.bumperRight ? F("true") : F("false");
  body += F(",\"bumper_left\":");
  body += snapshot.bumperLeft ? F("true") : F("false");
  body += F(",\"bumper_right\":");
  body += snapshot.bumperRight ? F("true") : F("false");
  body += F(",\"cliff_left\":");
  body += snapshot.cliffLeft ? F("true") : F("false");
  body += F(",\"cliff_front_left\":");
  body += snapshot.cliffFrontLeft ? F("true") : F("false");
  body += F(",\"cliff_front_right\":");
  body += snapshot.cliffFrontRight ? F("true") : F("false");
  body += F(",\"cliff_right\":");
  body += snapshot.cliffRight ? F("true") : F("false");
  body += F(",\"last_command\":\"");
  body += lastCommand;
  body += F("\"}");
  return body;
}

void writeCommand(uint8_t opcode) {
  roombaSerial.write(opcode);
  roombaSerial.flush();
}

void sendRoombaCommand(uint8_t opcode, const char* label) {
  writeCommand(opcode);
  lastCommandAt = millis();
  lastCommand = label;
}

void flushRoombaInput() {
  while (roombaSerial.available()) {
    roombaSerial.read();
  }
}

bool readExact(uint8_t* buffer, size_t length, uint16_t timeoutMs) {
  const unsigned long startedAt = millis();
  size_t index = 0;

  while (index < length && (millis() - startedAt) < timeoutMs) {
    if (roombaSerial.available()) {
      buffer[index++] = static_cast<uint8_t>(roombaSerial.read());
    }
  }

  return index == length;
}

bool queryPacketWithRetries(uint8_t packetId, uint8_t* output, size_t length,
                            int maxTries = SENSOR_QUERY_RETRIES,
                            uint16_t timeoutMs = ROOMBA_READ_TIMEOUT_MS) {
  for (int attempt = 0; attempt < maxTries; ++attempt) {
    flushRoombaInput();

    roombaSerial.write(ROOMBA_QUERY_LIST_OPCODE);
    roombaSerial.write(packetId);
    roombaSerial.flush();
    delay(QUERY_RESPONSE_DELAY_MS);

    if (readExact(output, length, timeoutMs)) {
      return true;
    }

    delay(QUERY_RETRY_DELAY_MS);
  }

  return false;
}

bool queryU8(uint8_t packetId, uint8_t& value) {
  uint8_t data[1] = {0};
  if (!queryPacketWithRetries(packetId, data, sizeof(data))) {
    return false;
  }

  value = data[0];
  return true;
}

bool queryU16(uint8_t packetId, uint16_t& value) {
  uint8_t data[2] = {0, 0};
  if (!queryPacketWithRetries(packetId, data, sizeof(data))) {
    return false;
  }

  value = (static_cast<uint16_t>(data[0]) << 8) | data[1];
  return true;
}

bool queryI16(uint8_t packetId, int16_t& value) {
  uint16_t raw = 0;
  if (!queryU16(packetId, raw)) {
    return false;
  }

  value = static_cast<int16_t>(raw);
  return true;
}

bool readRoombaSensors(SensorSnapshot& snapshot) {
  uint8_t bumps = 0;
  uint8_t cliffLeft = 0;
  uint8_t cliffFrontLeft = 0;
  uint8_t cliffFrontRight = 0;
  uint8_t cliffRight = 0;

  if (!queryU8(PACKET_CHARGING_STATE, snapshot.chargingState)) return false;
  if (!queryI16(PACKET_CURRENT, snapshot.currentMa)) return false;
  if (!queryU16(PACKET_VOLTAGE, snapshot.batteryMv)) return false;
  if (!queryU8(PACKET_BUTTONS, snapshot.buttons)) return false;
  if (!queryU8(PACKET_BUMPS_AND_WHEEL_DROPS, bumps)) return false;
  if (!queryU8(PACKET_CLIFF_LEFT, cliffLeft)) return false;
  if (!queryU8(PACKET_CLIFF_FRONT_LEFT, cliffFrontLeft)) return false;
  if (!queryU8(PACKET_CLIFF_FRONT_RIGHT, cliffFrontRight)) return false;
  if (!queryU8(PACKET_CLIFF_RIGHT, cliffRight)) return false;

  snapshot.bumperRight = (bumps & 0x01) != 0;
  snapshot.bumperLeft = (bumps & 0x02) != 0;
  snapshot.cliffLeft = cliffLeft != 0;
  snapshot.cliffFrontLeft = cliffFrontLeft != 0;
  snapshot.cliffFrontRight = cliffFrontRight != 0;
  snapshot.cliffRight = cliffRight != 0;
  snapshot.ok = true;
  return true;
}

void ensureRoombaReady() {
  if (roombaReady) {
    return;
  }

  writeCommand(ROOMBA_START_OPCODE);
  delay(ROOMBA_WAKE_DELAY_MS);
  writeCommand(ROOMBA_SAFE_OPCODE);
  delay(ROOMBA_WAKE_DELAY_MS);

  roombaReady = true;
  lastCommand = "safe";
  lastCommandAt = millis();
}

void sampleSensorsIfDue() {
  const unsigned long now = millis();
  if (lastSensorReadAt != 0 && (now - lastSensorReadAt) < SENSOR_INTERVAL_MS) {
    return;
  }

  lastSensorReadAt = now;

  SensorSnapshot snapshot;
  if (readRoombaSensors(snapshot)) {
    cachedSensors = snapshot;
    Serial.println(F("sampleSensorsIfDue: snapshot updated"));
  } else {
    cachedSensors.ok = false;
    Serial.println(F("Warning: failed to read Roomba sensors"));
  }
}

bool isAuthorized() {
  if (API_KEY[0] == '\0') {
    return true;
  }

  if (server.hasHeader("Authorization")) {
    const String auth = server.header("Authorization");
    const String prefix = F("Bearer ");
    if (auth.startsWith(prefix) && auth.substring(prefix.length()).equals(API_KEY)) {
      return true;
    }
  }

  return server.hasArg("key") && server.arg("key") == String(API_KEY);
}

bool ensureAuthorized() {
  if (isAuthorized()) {
    return true;
  }

  sendUnauthorized();
  return false;
}

void connectWifi(bool forceReconnect, unsigned long timeoutMs) {
  const unsigned long now = millis();

  if (!forceReconnect && WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (!forceReconnect && lastWifiAttemptAt != 0 &&
      (now - lastWifiAttemptAt) < WIFI_RETRY_INTERVAL_MS) {
    return;
  }

  if (forceReconnect) {
    Serial.println(F("WiFi reconnect requested"));
    WiFi.disconnect(true, true);
    delay(200);
  }

  lastWifiAttemptAt = now;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(WIFI_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print(F("Connecting to WiFi"));
  const unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startedAt) < timeoutMs) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("WiFi connected, IP: "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("WiFi connect failed; will retry in loop"));
  }
}

void handleOptions() {
  addCorsHeaders();
  server.send(204);
}

void handleNotFound() {
  const String path = server.uri();
  Serial.println("404: " + path);

  String body;
  body.reserve(64 + path.length());
  body += F("{\"ok\":false,\"error\":\"not_found\",\"path\":\"");
  body += path;
  body += F("\"}");
  sendJson(404, body);
}

void handleStatus() {
  sendJson(200, buildStatusJson());
}

void handleSensors() {
  if (!ensureAuthorized()) {
    return;
  }

  ensureRoombaReady();
  if (!cachedSensors.ok) {
    SensorSnapshot snapshot;
    if (readRoombaSensors(snapshot)) {
      cachedSensors = snapshot;
    } else {
      sendJson(503, "{\"ok\":false,\"error\":\"no_recent_snapshot\"}");
      return;
    }
  }

  sendJson(200, buildSensorJson(cachedSensors));
}

void handleClean() {
  if (!ensureAuthorized()) {
    return;
  }

  ensureRoombaReady();
  sendRoombaCommand(ROOMBA_CLEAN_OPCODE, "clean");
  sendActionResponse("clean");
}

void handleSpot() {
  if (!ensureAuthorized()) {
    return;
  }

  ensureRoombaReady();
  sendRoombaCommand(ROOMBA_SPOT_OPCODE, "spot");
  sendActionResponse("spot");
}

void handleSafe() {
  if (!ensureAuthorized()) {
    return;
  }

  ensureRoombaReady();
  sendRoombaCommand(ROOMBA_SAFE_OPCODE, "safe");
  sendActionResponse("safe");
}

void handleStop() {
  if (!ensureAuthorized()) {
    return;
  }

  ensureRoombaReady();
  sendRoombaCommand(ROOMBA_STOP_OPCODE, "stop");
  roombaReady = false;
  sendActionResponse("stop");
}

void registerRoute(const char* path, HTTPMethod method, THandlerFunction handler) {
  server.on(path, method, handler);
  server.on(path, HTTP_OPTIONS, handleOptions);
}

}  // namespace

void setup() {
  Serial.begin(115200);

  // UART1 is dedicated to the Roomba Open Interface port.
  roombaSerial.begin(ROOMBA_BAUD, SERIAL_8N1, ROOMBA_RX, ROOMBA_TX);
  roombaSerial.setTimeout(ROOMBA_READ_TIMEOUT_MS);

  // Give the Roomba serial interface time to wake up before sending START.
  delay(2000);
  ensureRoombaReady();
  connectWifi(false, WIFI_SETUP_TIMEOUT_MS);

  registerRoute("/api/status", HTTP_GET, handleStatus);
  registerRoute("/api/sensors", HTTP_GET, handleSensors);
  registerRoute("/api/clean", HTTP_POST, handleClean);
  registerRoute("/api/spot", HTTP_POST, handleSpot);
  registerRoute("/api/safe", HTTP_POST, handleSafe);
  registerRoute("/api/stop", HTTP_POST, handleStop);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println(F("HTTP server ready."));

  // Prime the cache once so the dashboard can render immediately.
  sampleSensorsIfDue();
}

void loop() {
  server.handleClient();
  sampleSensorsIfDue();
  connectWifi(false, WIFI_CONNECT_TIMEOUT_MS);
  delay(10);
}
