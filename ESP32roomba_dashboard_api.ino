/*
  ESP32 REST bridge for a Roomba over the Open Interface serial port.

  What this sketch does:
  - boots the Roomba into START + SAFE mode
  - exposes a small HTTP API for status, sensors, and basic actions
  - exposes hold-to-drive endpoints for forward, back, left, and right movement
  - caches sensor snapshots so the dashboard can poll without hammering UART
  - optionally protects mutating endpoints with an API key

  Before uploading:
  - (optional) change WIFI_AP_SSID / WIFI_AP_PASS
  - confirm UART wiring to the Roomba OI port
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WebSocketsServer.h>

namespace {

constexpr int ROOMBA_RX = 16;
constexpr int ROOMBA_TX = 17;

// ── Wi‑Fi (Access Point mode) ────────────────────────────────────────────────
// The ESP32 will create its own Wi‑Fi network.
// Connect your phone/PC to WIFI_AP_SSID, then browse to http://192.168.4.1
// and set the dashboard address to that IP.
const char* WIFI_AP_SSID = "Roomba-886";
const char* WIFI_AP_PASS = "roomba886";  // 8+ chars required by WPA2
const char* WIFI_HOSTNAME = "roomba-dashboard";
const char* API_KEY = "";

constexpr uint32_t ROOMBA_BAUD = 115200;
constexpr uint16_t ROOMBA_READ_TIMEOUT_MS = 400;
constexpr uint8_t ROOMBA_QUERY_LIST_OPCODE = 142;
constexpr uint8_t ROOMBA_START_OPCODE = 128;
constexpr uint8_t ROOMBA_SAFE_OPCODE = 131;
constexpr uint8_t ROOMBA_SPOT_OPCODE = 134;
constexpr uint8_t ROOMBA_CLEAN_OPCODE = 135;
constexpr uint8_t ROOMBA_DRIVE_OPCODE = 137;
constexpr uint8_t ROOMBA_STOP_OPCODE = 173;

constexpr unsigned long SENSOR_INTERVAL_MS = 500UL;
constexpr unsigned long WIFI_SETUP_TIMEOUT_MS = 6000UL;
constexpr uint16_t DNS_PORT = 53;

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

constexpr int16_t DRIVE_FORWARD_MM_S = 200;
constexpr int16_t DRIVE_BACK_MM_S = -200;
constexpr int16_t DRIVE_STOP_MM_S = 0;
constexpr int16_t DRIVE_STRAIGHT_RADIUS = static_cast<int16_t>(0x8000);
// A very small radius (~1mm) is close to in-place turning and can feel twitchy.
// Use a wider arc for better control and stability.
constexpr int16_t DRIVE_LEFT_RADIUS = 220;
constexpr int16_t DRIVE_RIGHT_RADIUS = -220;

// Extra margin for Wi‑Fi jitter / browser scheduling while holding a button.
constexpr unsigned long DRIVE_COMMAND_TIMEOUT_MS = 1200UL;

HardwareSerial roombaSerial(1);
WebServer server(80);
DNSServer dnsServer;
WebSocketsServer wsServer(81);

// ── Embedded dashboard files (served by the ESP32) ───────────────────────────
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en" dir="ltr">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover" />
  <title>Roomba 886 Dashboard</title>
  <link rel="stylesheet" href="style.css" />
</head>
<body>
  <main class="app">

    <!-- Header -->
    <section class="card wide">
      <div class="header">
        <h1 class="title">Roomba 886 Dashboard</h1>
        <div class="status-chip">
          <span class="dot" id="dot"></span>
          <span id="conn">Checking connection...</span>
        </div>
      </div>
      <p class="mini" id="lastUpdate">No data received yet.</p>
    </section>

    <!-- Status -->
    <section class="card">
      <h2 class="section-title">Status</h2>
      <div class="metrics">
        <div class="metric">
          <h3>Battery</h3>
          <div class="value"><span id="batteryPercent">--</span>%</div>
          <div class="mini"><span id="batteryMv">--</span> mV</div>
          <div class="battery-bar"><div class="battery-fill" id="batteryFill"></div></div>
        </div>
        <div class="metric">
          <h3>Charging State</h3>
          <div class="value" id="chargingState">--</div>
          <div class="mini">Code: <span id="chargingCode">--</span></div>
        </div>
        <div class="metric">
          <h3>Current</h3>
          <div class="value"><span id="currentMa">--</span> mA</div>
          <div class="mini">Negative = discharging</div>
        </div>
        <div class="metric">
          <h3>Buttons</h3>
          <div class="value" id="buttons">--</div>
          <div class="mini">Bitmap value</div>
        </div>
      </div>
    </section>

    <!-- Controls -->
    <section class="card">
      <h2 class="section-title">Controls</h2>
      <div class="controls">
        <button class="primary" data-action="clean">Clean</button>
        <button class="alt"     data-action="spot">Spot</button>
        <button                 data-action="safe">Safe</button>
        <button class="warn"    data-action="stop">Stop</button>
      </div>

      <div class="drive-title">Drive</div>
      <div class="mini">Hold a direction button to move. Release to stop.</div>
      <div class="speed-control" aria-label="Drive speed control">
        <div class="speed-row">
          <div class="speed-label">Speed</div>
          <div class="speed-value"><span id="speedValue">200</span> mm/s</div>
        </div>
        <input id="speedSlider" class="speed-slider" type="range" min="50" max="350" step="10" value="200" />
        <div class="mini">Tip: you can hold two directions together (e.g. Forward + Right).</div>
      </div>
      <div class="drive-pad">
        <div class="drive-pad-spacer"></div>
        <button class="drive-btn" data-drive="forward"  type="button">▲<span>Forward</span></button>
        <div class="drive-pad-spacer"></div>
        <button class="drive-btn" data-drive="left"     type="button">◀<span>Left</span></button>
        <button class="drive-btn stop" data-drive-stop  type="button">■<span>Stop</span></button>
        <button class="drive-btn" data-drive="right"    type="button">▶<span>Right</span></button>
        <div class="drive-pad-spacer"></div>
        <button class="drive-btn" data-drive="back"     type="button">▼<span>Back</span></button>
        <div class="drive-pad-spacer"></div>
      </div>
      <div class="mini" id="commandStatus">Ready.</div>
    </section>

    <!-- Sensors -->
    <section class="card wide">
      <h2 class="section-title">Sensors</h2>
      <div class="mini">Live data from <code>/api/sensors</code>.</div>
      <div class="sensor-grid" id="sensorGrid"></div>
    </section>

    <!-- Connection — at bottom -->
    <section class="card wide connection-card">
      <h2 class="section-title">⚙ Connection</h2>
      <div class="api-config">
        <div class="quick-steps">Enter the ESP32 address, save it, then test the connection.</div>
        <div class="api-row">
          <input class="api-input" id="apiBaseInput" type="url"
                 placeholder="http://192.168.4.1" inputmode="url" />
          <button class="api-btn primary" id="saveApiBase"  type="button">Save</button>
          <button class="api-btn alt"     id="testApiBase"  type="button">Test</button>
          <button class="api-btn"         id="clearApiBase" type="button">Clear</button>
        </div>
        <div class="mini">Active address: <span id="apiBaseDisplay">--</span></div>
      </div>
    </section>

    <footer class="footer wide">Roomba 886 Dashboard</footer>
  </main>

  <script src="app.js"></script>
</body>
</html>
)rawliteral";

const char STYLE_CSS[] PROGMEM = R"rawliteral(
/* Roomba 886 Dashboard — Responsive styles */
:root {
  --bg:       #060b16;
  --card:     #101a2d;
  --card-alt: #0f172b;
  --txt:      #eef3ff;
  --muted:    #9bacd0;
  --ok:       #29d178;
  --warn:     #f5b642;
  --bad:      #ff5d73;
  --accent:   #7c9bff;
  --accent2:  #58d0ff;
  --line:     #223157;

  --radius-lg: 16px;
  --radius-md: 12px;
  --radius-sm: 10px;
  --gap:       12px;
  --pad:       14px;
}

/* ── Reset ── */
*, *::before, *::after { box-sizing: border-box; }
body {
  margin: 0;
  padding: env(safe-area-inset-top, 12px) 12px
           env(safe-area-inset-bottom, 12px);
  background: radial-gradient(circle at top, #1a2850 0%, var(--bg) 52%);
  color: var(--txt);
  font-family: "Segoe UI", "Heebo", Arial, sans-serif;
  min-height: 100dvh;
  -webkit-tap-highlight-color: transparent;
}

/* ── Grid layout ──
   Mobile:  1 column
   Desktop: 2 equal columns, .wide spans both
*/
.app {
  max-width: 980px;
  margin: 0 auto;
  display: grid;
  gap: var(--gap);
  grid-template-columns: 1fr;          /* mobile default */
  padding-bottom: 12px;
}
@media (min-width: 700px) {
  .app { grid-template-columns: 1fr 1fr; }
  .wide { grid-column: 1 / -1; }
}

/* ── Card ── */
.card {
  background: linear-gradient(180deg, var(--card) 0%, var(--card-alt) 100%);
  border: 1px solid var(--line);
  border-radius: var(--radius-lg);
  padding: var(--pad);
  box-shadow: 0 8px 22px rgba(0,0,0,.25);
}
.connection-card { opacity: .92; }

/* ── Header ── */
.header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 10px;
  flex-wrap: wrap;
}
.title        { margin: 0; font-size: clamp(.95rem, 2.5vw, 1.15rem); }
.section-title { margin: 0 0 10px; font-size: clamp(.95rem, 2.5vw, 1.1rem); font-weight: 700; }

/* ── Status chip ── */
.status-chip {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  border-radius: 999px;
  border: 1px solid var(--line);
  padding: 4px 10px;
  font-size: .8rem;
  color: var(--muted);
  white-space: nowrap;
}
.dot {
  width: 8px; height: 8px;
  border-radius: 50%;
  background: var(--warn);
  box-shadow: 0 0 8px var(--warn);
  flex-shrink: 0;
}
.dot.online  { background: var(--ok);  box-shadow: 0 0 8px var(--ok); }
.dot.offline { background: var(--bad); box-shadow: 0 0 8px var(--bad); }

/* ── Metrics grid ── */
.metrics {
  display: grid;
  gap: 10px;
  grid-template-columns: repeat(2, minmax(0, 1fr));
}
.metric {
  border: 1px solid var(--line);
  border-radius: var(--radius-md);
  padding: 10px;
  background: rgba(255,255,255,.02);
}
.metric h3    { margin: 0; font-size: .75rem; color: var(--muted); font-weight: 600; }
.metric .value { margin-top: 6px; font-size: clamp(1.1rem, 3vw, 1.35rem); font-weight: 700; }
.mini {
  margin-top: 6px;
  font-size: .74rem;
  color: var(--muted);
  line-height: 1.5;
}

/* ── Battery bar ── */
.battery-bar {
  margin-top: 8px;
  width: 100%; height: 8px;
  border-radius: 8px;
  overflow: hidden;
  background: #1b2a4c;
  border: 1px solid #2f477c;
}
.battery-fill {
  width: 0%; height: 100%;
  border-radius: 8px;
  transition: width .4s ease, background .4s ease;
}

/* ── Action buttons ── */
.controls {
  display: grid;
  gap: 8px;
  grid-template-columns: repeat(2, minmax(0, 1fr));
}

button {
  border: 1px solid var(--line);
  background: #172547;
  color: var(--txt);
  border-radius: var(--radius-md);
  padding: 12px 8px;
  font-size: .93rem;
  font-weight: 700;
  cursor: pointer;
  min-height: 52px;
  transition: opacity .15s, transform .1s;
  touch-action: manipulation;
}
button:active:not(:disabled) { opacity: .8; transform: scale(.97); }
button.primary { background: linear-gradient(180deg, var(--accent) 0%, #3d73dc 100%); border-color: transparent; }
button.warn    { background: linear-gradient(180deg, #ff7f90 0%, #dc4a62 100%);  border-color: transparent; }
button.alt     { background: linear-gradient(180deg, var(--accent2) 0%, #2799d7 100%); border-color: transparent; }
button:disabled { opacity: .5; cursor: not-allowed; transform: none; }

/* ── Drive pad ── */
.drive-title {
  margin-top: 16px;
  font-size: .78rem;
  font-weight: 700;
  color: var(--muted);
  text-transform: uppercase;
  letter-spacing: .06em;
}

/* ── Speed control ── */
.speed-control {
  margin-top: 10px;
  border: 1px solid var(--line);
  border-radius: var(--radius-md);
  padding: 10px;
  background: rgba(255,255,255,.02);
}
.speed-row {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: 10px;
}
.speed-label {
  font-size: .75rem;
  font-weight: 700;
  color: var(--muted);
  text-transform: uppercase;
  letter-spacing: .06em;
}
.speed-value {
  font-size: .85rem;
  font-weight: 700;
  color: var(--txt);
}
.speed-slider {
  width: 100%;
  margin-top: 8px;
}
.drive-pad {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 8px;
  margin-top: 10px;
}
.drive-pad-spacer { min-height: 52px; }

.drive-btn {
  min-height: 60px;
  background: #1a2a4d;
  border-radius: var(--radius-md);
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 3px;
  font-size: 1.1rem;
  touch-action: none;
  user-select: none;
  -webkit-user-select: none;
}
.drive-btn span { font-size: .7rem; font-weight: 600; color: var(--muted); }
.drive-btn.stop  { background: linear-gradient(180deg, #ff7f90 0%, #dc4a62 100%); border-color: transparent; }
.drive-btn.stop span { color: rgba(255,255,255,.7); }
.drive-btn.active {
  box-shadow: 0 0 0 2px rgba(88,208,255,.45) inset;
  background: linear-gradient(180deg, var(--accent2) 0%, #2799d7 100%);
  border-color: transparent;
}
.drive-btn.active span { color: rgba(255,255,255,.8); }

/* ── Sensor grid ── */
.sensor-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(110px, 1fr));
  gap: 8px;
  margin-top: 10px;
}
@media (min-width: 700px) {
  .sensor-grid { grid-template-columns: repeat(auto-fill, minmax(130px, 1fr)); }
}
.sensor-item {
  border: 1px solid var(--line);
  padding: 8px 10px;
  border-radius: var(--radius-sm);
  background: rgba(255,255,255,.03);
}
.sensor-name  { font-size: .68rem; color: var(--muted); word-break: break-word; }
.sensor-value { font-size: .88rem; font-weight: 700; margin-top: 4px; word-break: break-all; }

/* ── API / Connection ── */
.api-config { display: grid; gap: 10px; }
.api-row {
  display: flex;
  gap: 8px;
  flex-wrap: wrap;
}
.api-input {
  flex: 1;
  min-width: 180px;
  border: 1px solid var(--line);
  background: #0f1a34;
  color: var(--txt);
  border-radius: var(--radius-sm);
  padding: 10px 12px;
  font-size: .9rem;
  direction: ltr;
  text-align: left;
}
.api-btn {
  min-height: 44px;
  padding: 8px 14px;
  font-size: .85rem;
  border-radius: var(--radius-sm);
}
.quick-steps {
  color: var(--muted);
  font-size: .82rem;
  line-height: 1.6;
}

/* ── Footer ── */
.footer {
  color: var(--muted);
  font-size: .74rem;
  text-align: center;
  padding: 8px 0 4px;
}

/* ── Mobile tweaks (phones < 420px) ── */
@media (max-width: 420px) {
  body { padding: 8px; }
  :root { --gap: 10px; --pad: 12px; }

  .controls { grid-template-columns: repeat(2, 1fr); }

  .drive-btn { min-height: 64px; font-size: 1.2rem; }

  .api-row { flex-direction: column; }
  .api-input { min-width: unset; width: 100%; }
  .api-btn   { width: 100%; }

  .sensor-grid { grid-template-columns: repeat(auto-fill, minmax(90px, 1fr)); }
}
)rawliteral";

const char APP_JS[] PROGMEM = R"rawliteral(
const $ = (id) => document.getElementById(id);

let API_BASE = "";
function defaultApiBase() {
  try {
    if (window.location.protocol === "http:" || window.location.protocol === "https:") {
      if (window.location.hostname === "192.168.4.1") return window.location.origin;
      if (window.location.hostname === "roomba-dashboard") return window.location.origin;
    }
  } catch {
    // Ignore and fall back to the AP IP below.
  }
  return "http://192.168.4.1";
}
const SENSOR_REFRESH_MS = 250;
const STATUS_REFRESH_MS = 2000;
const DRIVE_REPEAT_MS = 200;
const DRIVE_STRAIGHT_RADIUS = -32768; // 0x8000 in int16
const DRIVE_ARC_RADIUS = 220;
let cooldownUntil = 0;
let sensorsBusy = false;
let statusBusy = false;
let driveInterval = null;
let driveSessionId = 0;
let ws = null;
let wsConnected = false;
let wsEverConnected = false;
let wsReconnectTimer = null;
const drivePointers = new Map(); // pointerId -> direction
let driveFb = null; // "forward" | "back" | null
let driveLr = null; // "left" | "right" | null
let driveSpeed = 200;

const chargingLabels = {
  0: "Not charging",
  1: "Reconditioning",
  2: "Full charging",
  3: "Trickle charging",
  4: "Waiting",
  5: "Charging fault"
};

const ui = {
  dot: $("dot"),
  conn: $("conn"),
  lastUpdate: $("lastUpdate"),
  batteryPercent: $("batteryPercent"),
  batteryMv: $("batteryMv"),
  batteryFill: $("batteryFill"),
  chargingState: $("chargingState"),
  chargingCode: $("chargingCode"),
  currentMa: $("currentMa"),
  buttons: $("buttons"),
  sensorGrid: $("sensorGrid"),
  commandStatus: $("commandStatus"),
  actionButtons: [...document.querySelectorAll("button[data-action]")],
  driveButtons: [...document.querySelectorAll("button[data-drive]")],
  driveStop: document.querySelector("button[data-drive-stop]"),
  speedSlider: $("speedSlider"),
  speedValue: $("speedValue"),
  apiBaseInput: $("apiBaseInput"),
  apiBaseDisplay: $("apiBaseDisplay"),
  saveApiBase: $("saveApiBase"),
  testApiBase: $("testApiBase"),
  clearApiBase: $("clearApiBase")
};

function normalizeApiBase(value) {
  if (!value) return "";
  return value.trim().replace(/\/+$/, "");
}

function resolveApiBase() {
  const params = new URLSearchParams(window.location.search);
  const fromQuery = params.get("apiBase") || params.get("api");
  if (fromQuery) return normalizeApiBase(fromQuery);

  const fromStorage = localStorage.getItem("roombaApiBase");
  if (fromStorage) return normalizeApiBase(fromStorage);

  return defaultApiBase();
}

function setApiBase(value, save = false) {
  const normalized = normalizeApiBase(value);
  API_BASE = normalized;

  if (save) {
    if (normalized) {
      localStorage.setItem("roombaApiBase", normalized);
    } else {
      localStorage.removeItem("roombaApiBase");
    }
  }

  ui.apiBaseInput.value = normalized;
  ui.apiBaseDisplay.textContent = normalized || "--";

  reconnectWebSocket();
}

function wsUrlFromApiBase() {
  if (!API_BASE) return "";
  const u = new URL(API_BASE);
  const proto = u.protocol === "https:" ? "wss:" : "ws:";
  return `${proto}//${u.hostname}:81/`;
}

function disconnectWebSocket() {
  if (wsReconnectTimer) {
    clearTimeout(wsReconnectTimer);
    wsReconnectTimer = null;
  }
  if (ws) {
    try { ws.close(); } catch {}
  }
  ws = null;
  wsConnected = false;
}

function reconnectWebSocket() {
  disconnectWebSocket();
  if (!API_BASE) return;

  let url = "";
  try {
    url = wsUrlFromApiBase();
  } catch {
    return;
  }

  try {
    ws = new WebSocket(url);
  } catch {
    ws = null;
    return;
  }

  ws.addEventListener("open", () => {
    wsConnected = true;
    wsEverConnected = true;
    setCommandStatus("Live updates connected.");
  });

  ws.addEventListener("close", () => {
    wsConnected = false;
    wsReconnectTimer = setTimeout(() => reconnectWebSocket(), 1500);
  });

  ws.addEventListener("error", () => {
    wsConnected = false;
  });

  ws.addEventListener("message", (event) => {
    let msg;
    try {
      msg = JSON.parse(event.data);
    } catch {
      return;
    }

    if (msg?.type === "status") {
      if (msg.ok) setOnlineState("online", "Connected to Roomba");
      else setOnlineState("offline", "No API connectivity");
      return;
    }

    if (msg?.type === "sensors") {
      applySensorData(msg);
    }
  });
}

function isMixedContentBlocked() {
  if (!API_BASE) return false;
  try {
    const baseUrl = new URL(API_BASE);
    return window.location.protocol === "https:" && baseUrl.protocol === "http:";
  } catch {
    return false;
  }
}

function mixedContentMessage() {
  return "Blocked by browser (Mixed Content): an HTTPS page cannot call an HTTP API. Run this dashboard over local HTTP or expose the ESP32 API over HTTPS.";
}

function setCommandStatus(text) {
  ui.commandStatus.textContent = text;
}

function setOnlineState(state, text) {
  ui.dot.classList.remove("online", "offline");
  if (state === "online") ui.dot.classList.add("online");
  if (state === "offline") ui.dot.classList.add("offline");
  ui.conn.textContent = text;
}

function clearUI() {
  ui.batteryPercent.textContent = "--";
  ui.batteryMv.textContent = "--";
  ui.currentMa.textContent = "--";
  ui.buttons.textContent = "--";
  ui.chargingCode.textContent = "--";
  ui.chargingState.textContent = "--";
  ui.batteryFill.style.width = "0%";
  ui.sensorGrid.innerHTML = '<div class="mini">No sensor data.</div>';
}

function batteryColor(pct) {
  if (pct < 25) return "#ff6262";
  if (pct < 60) return "#ffd95a";
  return "#3ee089";
}

function estimateBatteryPercent(mv) {
  const pct = ((mv - 14000) / (16800 - 14000)) * 100;
  return Math.max(0, Math.min(100, Math.round(pct)));
}

function toDisplayValue(value) {
  if (typeof value === "boolean") return value ? "On" : "Off";
  if (Array.isArray(value)) return value.join(", ");
  if (value && typeof value === "object") {
    try {
      return JSON.stringify(value);
    } catch {
      return "[Object]";
    }
  }

  return value ?? "--";
}

function renderAllSensors(data) {
  const keys = Object.keys(data || {}).sort();
  if (!keys.length) {
    ui.sensorGrid.innerHTML = '<div class="mini">No sensor payload received.</div>';
    return;
  }

  ui.sensorGrid.innerHTML = keys.map((key) => `
    <div class="sensor-item">
      <div class="sensor-name">${key}</div>
      <div class="sensor-value">${toDisplayValue(data[key])}</div>
    </div>
  `).join("");
}

function applySensorData(data) {
  const mv = Number(data.battery_mV ?? 0);
  const pct = estimateBatteryPercent(mv);
  const current = Number(data.current_mA ?? 0);
  const code = Number(data.charging_state ?? -1);

  ui.batteryPercent.textContent = pct;
  ui.batteryMv.textContent = mv || "--";
  ui.batteryFill.style.width = `${pct}%`;
  ui.batteryFill.style.background = batteryColor(pct);
  ui.currentMa.textContent = Number.isFinite(current) ? current : "--";
  ui.chargingCode.textContent = Number.isFinite(code) ? code : "--";
  ui.chargingState.textContent = chargingLabels[code] || "Unknown";
  ui.buttons.textContent = Number(data.buttons ?? 0);

  renderAllSensors(data);
  ui.lastUpdate.textContent = `Updated: ${new Date().toLocaleTimeString("en-US")}`;
}

async function fetchJson(path, options = {}) {
  const res = await fetch(`${API_BASE}${path}`, {
    headers: { Accept: "application/json" },
    cache: "no-store",
    ...options
  });

  if (!res.ok) throw new Error(`HTTP ${res.status}`);

  const ct = res.headers.get("content-type") || "";
  return ct.includes("application/json") ? res.json() : {};
}

async function refreshStatus() {
  if (statusBusy) return false;
  if (!API_BASE) {
    setCommandStatus("No API address");
    setOnlineState("offline", "No API connectivity");
    return false;
  }

  if (isMixedContentBlocked()) {
    setCommandStatus(mixedContentMessage());
    setOnlineState("offline", "Mixed Content blocked");
    clearUI();
    return false;
  }

  statusBusy = true;
  try {
    await fetchJson("/api/status");
    setOnlineState("online", "Connected to Roomba");
    return true;
  } catch {
    setOnlineState("offline", "No API connectivity");
    clearUI();
    return false;
  } finally {
    statusBusy = false;
  }
}

async function refreshSensors() {
  if (sensorsBusy) return false;
  if (!API_BASE) {
    setCommandStatus("No API address");
    return false;
  }

  if (isMixedContentBlocked()) {
    setCommandStatus(mixedContentMessage());
    ui.lastUpdate.textContent = "Cannot load sensors due to Mixed Content.";
    clearUI();
    return false;
  }

  sensorsBusy = true;
  try {
    const data = await fetch(`${API_BASE}/api/sensors`, {
      headers: { Accept: "application/json" },
      cache: "no-store"
    }).then((res) => {
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      return res.json();
    });
    applySensorData(data);
    return true;
  } catch (err) {
    ui.lastUpdate.textContent = `Read error: ${err.message}`;
    clearUI();
    return false;
  } finally {
    sensorsBusy = false;
  }
}

function setButtonsDisabled(disabled) {
  ui.actionButtons.forEach((btn) => {
    btn.disabled = disabled;
  });
}

function updateDriveButtonHighlights() {
  ui.driveButtons.forEach((btn) => {
    const dir = btn.dataset.drive;
    const active = dir === driveFb || dir === driveLr;
    btn.classList.toggle("active", active);
  });
}

function computeDriveStateFromPointers() {
  let fb = null;
  let lr = null;
  for (const dir of drivePointers.values()) {
    if (dir === "forward" || dir === "back") fb = dir;
    if (dir === "left" || dir === "right") lr = dir;
  }
  driveFb = fb;
  driveLr = lr;
  updateDriveButtonHighlights();
}

function setDriveSpeed(value, save = true) {
  const v = Number(value);
  if (!Number.isFinite(v)) return;
  const clamped = Math.max(50, Math.min(350, Math.round(v / 10) * 10));
  driveSpeed = clamped;
  if (ui.speedSlider) ui.speedSlider.value = String(clamped);
  if (ui.speedValue) ui.speedValue.textContent = String(clamped);
  if (save) localStorage.setItem("roombaDriveSpeed", String(clamped));
}

async function sendDriveCustom(velocity, radius, updateStatus = true) {
  if (!API_BASE) {
    setCommandStatus("No API address");
    return false;
  }

  if (isMixedContentBlocked()) {
    setCommandStatus(mixedContentMessage());
    return false;
  }

  try {
    const vel = Math.trunc(velocity);
    const rad = Math.trunc(radius);
    await fetch(`${API_BASE}/api/drive?vel=${encodeURIComponent(vel)}&rad=${encodeURIComponent(rad)}`, { method: "POST", cache: "no-store" }).then((res) => {
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
    });
    if (updateStatus) {
      const parts = [driveFb, driveLr].filter(Boolean);
      const label = parts.length ? parts.join("+") : "stop";
      setCommandStatus(`Drive: ${label} @ ${driveSpeed} mm/s`);
    }
    return true;
  } catch (err) {
    if (updateStatus) {
      setCommandStatus(`Drive failed (${err.message})`);
    }
    return false;
  }
}

function stopDriveLoop(sendStop = true) {
  driveSessionId += 1;
  if (driveInterval) {
    clearInterval(driveInterval);
    driveInterval = null;
  }
  drivePointers.clear();
  computeDriveStateFromPointers();

  if (sendStop) {
    sendDriveCustom(0, DRIVE_STRAIGHT_RADIUS, true).then(() => {
      refreshSensors();
    });
  }
}

function computeVelocityRadius() {
  const spd = driveSpeed;
  const fb = driveFb;
  const lr = driveLr;

  if (!fb && !lr) {
    return { vel: 0, rad: DRIVE_STRAIGHT_RADIUS };
  }

  if (!fb && lr) {
    const rad = lr === "left" ? 1 : -1;
    return { vel: spd, rad };
  }

  const vel = fb === "back" ? -spd : spd;
  const rad = lr
    ? (lr === "left" ? DRIVE_ARC_RADIUS : -DRIVE_ARC_RADIUS)
    : DRIVE_STRAIGHT_RADIUS;
  return { vel, rad };
}

async function ensureDriveLoopRunning() {
  const sessionId = ++driveSessionId;
  if (driveInterval) return;

  const tick = () => {
    if (driveSessionId !== sessionId) {
      clearInterval(driveInterval);
      driveInterval = null;
      return;
    }
    computeDriveStateFromPointers();
    const { vel, rad } = computeVelocityRadius();
    sendDriveCustom(vel, rad, false);
  };

  computeDriveStateFromPointers();
  const { vel, rad } = computeVelocityRadius();
  const ok = await sendDriveCustom(vel, rad, true);
  if (!ok || driveSessionId !== sessionId) {
    return;
  }

  driveInterval = setInterval(tick, DRIVE_REPEAT_MS);
}

async function sendAction(action) {
  if (!API_BASE) {
    setCommandStatus("No API address");
    return;
  }

  if (isMixedContentBlocked()) {
    setCommandStatus(mixedContentMessage());
    return;
  }

  const now = Date.now();
  if (now < cooldownUntil) {
    setCommandStatus("Waiting...");
    return;
  }

  stopDriveLoop(false);
  setButtonsDisabled(true);
  setCommandStatus(`Sending ${action}...`);

  try {
    await fetch(`${API_BASE}/api/${action}`, { method: "POST", cache: "no-store" }).then((res) => {
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
    });
    setCommandStatus(`${action} command sent`);
    await refreshStatus();
    await refreshSensors();
  } catch (err) {
    setCommandStatus(`${action} command failed (${err.message})`);
  } finally {
    cooldownUntil = Date.now() + 300;
    setTimeout(() => setButtonsDisabled(false), 300);
  }
}

async function testConnection() {
  setCommandStatus("Testing API connection...");
  const statusOk = await refreshStatus();
  const sensorsOk = await refreshSensors();

  if (statusOk && sensorsOk) {
    setCommandStatus("Connection is healthy.");
  }
}

function bindDriveButton(btn) {
  const direction = btn.dataset.drive;
  const start = async (event) => {
    event.preventDefault();
    drivePointers.set(event.pointerId, direction);
    if (btn?.setPointerCapture) {
      try {
        btn.setPointerCapture(event.pointerId);
      } catch {}
    }
    await ensureDriveLoopRunning();
  };
  const stop = (event) => {
    event.preventDefault();
    drivePointers.delete(event.pointerId);
    computeDriveStateFromPointers();
    if (drivePointers.size === 0) {
      stopDriveLoop(true);
    }
  };

  btn.addEventListener("pointerdown", start);
  btn.addEventListener("pointerup", stop);
  btn.addEventListener("pointercancel", stop);
  btn.addEventListener("lostpointercapture", stop);
  btn.addEventListener("contextmenu", (event) => event.preventDefault());
}

function bootstrap() {
  setApiBase(resolveApiBase(), false);
  reconnectWebSocket();

  const savedSpeed = Number(localStorage.getItem("roombaDriveSpeed") || "");
  setDriveSpeed(Number.isFinite(savedSpeed) && savedSpeed ? savedSpeed : 200, false);
  if (ui.speedSlider) {
    ui.speedSlider.addEventListener("input", () => setDriveSpeed(ui.speedSlider.value, false));
    ui.speedSlider.addEventListener("change", () => setDriveSpeed(ui.speedSlider.value, true));
  }

  const params = new URLSearchParams(window.location.search);
  if (params.get("apiBase") || params.get("api")) {
    localStorage.setItem("roombaApiBase", API_BASE);
    window.history.replaceState({}, "", window.location.pathname);
  }

  if (!API_BASE) {
    setCommandStatus("Enter an API address and click Save.");
  }

  ui.actionButtons.forEach((btn) => {
    btn.addEventListener("click", () => sendAction(btn.dataset.action));
  });

  ui.driveButtons.forEach(bindDriveButton);
  ui.driveStop.addEventListener("click", () => stopDriveLoop(true));

  window.addEventListener("pointerup", () => {
    if (drivePointers.size) {
      stopDriveLoop(true);
    }
  });
  window.addEventListener("blur", () => stopDriveLoop(true));
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) {
      stopDriveLoop(true);
    }
  });

  ui.saveApiBase.addEventListener("click", async () => {
    const normalized = normalizeApiBase(ui.apiBaseInput.value);
    if (!normalized) {
      setCommandStatus("Enter an API address first.");
      return;
    }

    if (!/^https?:\/\//i.test(normalized)) {
      setCommandStatus("Address must start with http:// or https://");
      return;
    }

    setApiBase(normalized, true);
    if (isMixedContentBlocked()) {
      setCommandStatus(mixedContentMessage());
      return;
    }
    await testConnection();
  });

  ui.testApiBase.addEventListener("click", testConnection);

  ui.clearApiBase.addEventListener("click", () => {
    stopDriveLoop(false);
    setApiBase("", true);
    disconnectWebSocket();
    clearUI();
    setOnlineState("offline", "No API connectivity");
    setCommandStatus("API address cleared.");
  });

  refreshStatus();
  refreshSensors();
  setInterval(refreshStatus, STATUS_REFRESH_MS);
  setInterval(() => {
    if (!wsConnected) {
      refreshSensors();
    }
  }, SENSOR_REFRESH_MS);
}

bootstrap();
)rawliteral";

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
unsigned long lastCommandAt = 0;
const char* lastCommand = "none";
bool roombaReady = false;
bool driveMotionActive = false;
unsigned long lastDriveCommandAt = 0;

void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type,Authorization");
}

void sendJson(int code, const String& body) {
  addCorsHeaders();
  server.send(code, "application/json", body);
}

void sendProgmem(int code, const char* contentType, const char* progmemBody) {
  server.send_P(code, contentType, progmemBody);
}

void sendRedirectToRoot() {
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void sendUnauthorized() {
  sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
}

void sendActionResponse(const char* action) {
  char body[64];
  snprintf(body, sizeof(body), "{\"ok\":true,\"action\":\"%s\"}", action);
  sendJson(200, body);
}

String buildStatusJson() {
  String body;
  body.reserve(128);
  body += F("{\"ok\":true,\"roomba_ready\":");
  body += roombaReady ? F("true") : F("false");
  body += F(",\"ip\":\"");
  body += WiFi.softAPIP().toString();
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

String buildWsStatusJson(bool ok) {
  String body;
  body.reserve(128);
  body += F("{\"type\":\"status\",\"ok\":");
  body += ok ? F("true") : F("false");
  body += F(",\"roomba_ready\":");
  body += roombaReady ? F("true") : F("false");
  body += F(",\"ip\":\"");
  body += WiFi.softAPIP().toString();
  body += F("\",\"uptime_ms\":");
  body += String(millis());
  body += '}';
  return body;
}

String buildWsSensorsJson(const SensorSnapshot& snapshot) {
  // Reuse the normal JSON fields, add a message type.
  String body;
  body.reserve(360);
  body += F("{\"type\":\"sensors\",");
  const String payload = buildSensorJson(snapshot);
  // payload is {"ok":true,...}
  body += payload.substring(1);
  return body;
}

void writeCommand(uint8_t opcode) {
  roombaSerial.write(opcode);
  roombaSerial.flush();
}

void invalidateSensorCache() {
  cachedSensors.ok = false;
  lastSensorReadAt = 0;
}

void sendRoombaCommand(uint8_t opcode, const char* label) {
  writeCommand(opcode);
  lastCommandAt = millis();
  lastCommand = label;
  invalidateSensorCache();
}

void sendDriveCommand(int16_t velocity, int16_t radius, const char* label) {
  roombaSerial.write(ROOMBA_DRIVE_OPCODE);
  roombaSerial.write(static_cast<uint8_t>((velocity >> 8) & 0xFF));
  roombaSerial.write(static_cast<uint8_t>(velocity & 0xFF));
  roombaSerial.write(static_cast<uint8_t>((radius >> 8) & 0xFF));
  roombaSerial.write(static_cast<uint8_t>(radius & 0xFF));
  roombaSerial.flush();
  lastCommandAt = millis();
  lastCommand = label;
  driveMotionActive = velocity != DRIVE_STOP_MM_S;
  lastDriveCommandAt = driveMotionActive ? lastCommandAt : 0;
  // Driving endpoints can be hit very frequently (hold-to-drive). Invalidating
  // the sensor cache forces extra UART reads that can interfere with smooth motion.
  // Sensors are still sampled on their own schedule.
}

void stopDriveMotionIfTimedOut() {
  if (!driveMotionActive) {
    return;
  }

  const unsigned long now = millis();
  if ((now - lastDriveCommandAt) <= DRIVE_COMMAND_TIMEOUT_MS) {
    return;
  }

  sendDriveCommand(DRIVE_STOP_MM_S, DRIVE_STRAIGHT_RADIUS, "drive_timeout");
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

void broadcastStatus(bool ok) {
  String payload = buildWsStatusJson(ok);
  wsServer.broadcastTXT(payload);
}

void broadcastSensorsIfAvailable() {
  if (!cachedSensors.ok) return;
  String payload = buildWsSensorsJson(cachedSensors);
  wsServer.broadcastTXT(payload);
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
  const unsigned long interval = driveMotionActive ? (SENSOR_INTERVAL_MS * 2UL) : SENSOR_INTERVAL_MS;
  if (lastSensorReadAt != 0 && (now - lastSensorReadAt) < interval) {
    return;
  }

  lastSensorReadAt = now;

  SensorSnapshot snapshot;
  if (readRoombaSensors(snapshot)) {
    cachedSensors = snapshot;
    Serial.println(F("sampleSensorsIfDue: snapshot updated"));
    broadcastSensorsIfAvailable();
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

void startAccessPoint(unsigned long timeoutMs) {
  WiFi.mode(WIFI_AP);
  WiFi.setHostname(WIFI_HOSTNAME);

  Serial.print(F("Starting AP: "));
  Serial.println(WIFI_AP_SSID);

  const unsigned long startedAt = millis();
  while (!WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS) && (millis() - startedAt) < timeoutMs) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.getMode() == WIFI_AP && WiFi.softAPIP().toString() != F("0.0.0.0")) {
    Serial.print(F("AP ready, IP: "));
    Serial.println(WiFi.softAPIP());
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  } else {
    Serial.println(F("AP start failed (check SSID/pass)."));
  }
}

void handleOptions() {
  addCorsHeaders();
  server.send(204);
}

void handleCaptive() {
  // OS captive portal detection endpoints.
  sendRedirectToRoot();
}

void handleIndex() {
  sendProgmem(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleStyle() {
  sendProgmem(200, "text/css; charset=utf-8", STYLE_CSS);
}

void handleScript() {
  sendProgmem(200, "application/javascript; charset=utf-8", APP_JS);
}

void handleNoContent() {
  server.send(204);
}

void handleNotFound() {
  const String path = server.uri();
  if (!path.startsWith("/api/")) {
    sendRedirectToRoot();
    return;
  }

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
  broadcastStatus(true);
}

void handleSensors() {
  if (!ensureAuthorized()) {
    return;
  }

  ensureRoombaReady();
  sampleSensorsIfDue();
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
  broadcastSensorsIfAvailable();
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
  sendDriveCommand(DRIVE_STOP_MM_S, DRIVE_STRAIGHT_RADIUS, "stop");
  sendActionResponse("stop");
}

void handleForward() {
  if (!ensureAuthorized()) {
    return;
  }

  ensureRoombaReady();
  sendDriveCommand(DRIVE_FORWARD_MM_S, DRIVE_STRAIGHT_RADIUS, "forward");
  sendActionResponse("forward");
}

void handleBack() {
  if (!ensureAuthorized()) {
    return;
  }

  ensureRoombaReady();
  sendDriveCommand(DRIVE_BACK_MM_S, DRIVE_STRAIGHT_RADIUS, "back");
  sendActionResponse("back");
}

void handleLeft() {
  if (!ensureAuthorized()) {
    return;
  }

  ensureRoombaReady();
  sendDriveCommand(DRIVE_FORWARD_MM_S, DRIVE_LEFT_RADIUS, "left");
  sendActionResponse("left");
}

void handleRight() {
  if (!ensureAuthorized()) {
    return;
  }

  ensureRoombaReady();
  sendDriveCommand(DRIVE_FORWARD_MM_S, DRIVE_RIGHT_RADIUS, "right");
  sendActionResponse("right");
}

void handleDrive() {
  if (!ensureAuthorized()) {
    return;
  }

  if (!server.hasArg("vel") || !server.hasArg("rad")) {
    sendJson(400, "{\"ok\":false,\"error\":\"missing_vel_or_rad\"}");
    return;
  }

  const long velLong = server.arg("vel").toInt();
  const long radLong = server.arg("rad").toInt();

  // Conservative validation for stability/safety.
  if (velLong < -500 || velLong > 500) {
    sendJson(400, "{\"ok\":false,\"error\":\"velocity_out_of_range\"}");
    return;
  }

  const bool isStraight = radLong == -32768L;
  if (!isStraight && (radLong < -2000L || radLong > 2000L)) {
    sendJson(400, "{\"ok\":false,\"error\":\"radius_out_of_range\"}");
    return;
  }

  ensureRoombaReady();
  sendDriveCommand(static_cast<int16_t>(velLong), static_cast<int16_t>(radLong), "drive");

  char body[96];
  snprintf(body, sizeof(body), "{\"ok\":true,\"action\":\"drive\",\"vel\":%ld,\"rad\":%ld}", velLong, radLong);
  sendJson(200, body);
}

void registerRoute(const char* path, HTTPMethod method, WebServer::THandlerFunction handler) {
  server.on(path, method, handler);
  server.on(path, HTTP_OPTIONS, handleOptions);
}

}  // namespace

void setup() {
  Serial.begin(115200);

  roombaSerial.begin(ROOMBA_BAUD, SERIAL_8N1, ROOMBA_RX, ROOMBA_TX);
  roombaSerial.setTimeout(ROOMBA_READ_TIMEOUT_MS);

  delay(2000);
  ensureRoombaReady();
  startAccessPoint(WIFI_SETUP_TIMEOUT_MS);

  // Serve the embedded dashboard website directly from the ESP32
  registerRoute("/", HTTP_GET, handleIndex);
  registerRoute("/index.html", HTTP_GET, handleIndex);
  registerRoute("/style.css", HTTP_GET, handleStyle);
  registerRoute("/app.js", HTTP_GET, handleScript);
  registerRoute("/favicon.ico", HTTP_GET, handleNoContent);

  // Captive portal endpoints (Android / iOS / Windows)
  registerRoute("/generate_204", HTTP_GET, handleCaptive);
  registerRoute("/gen_204", HTTP_GET, handleCaptive);
  registerRoute("/hotspot-detect.html", HTTP_GET, handleCaptive);
  registerRoute("/library/test/success.html", HTTP_GET, handleCaptive);
  registerRoute("/connecttest.txt", HTTP_GET, handleCaptive);
  registerRoute("/ncsi.txt", HTTP_GET, handleCaptive);
  registerRoute("/fwlink", HTTP_GET, handleCaptive);

  registerRoute("/api/status", HTTP_GET, handleStatus);
  registerRoute("/api/sensors", HTTP_GET, handleSensors);
  registerRoute("/api/clean", HTTP_POST, handleClean);
  registerRoute("/api/spot", HTTP_POST, handleSpot);
  registerRoute("/api/safe", HTTP_POST, handleSafe);
  registerRoute("/api/stop", HTTP_POST, handleStop);
  registerRoute("/api/drive", HTTP_POST, handleDrive);
  registerRoute("/api/forward", HTTP_POST, handleForward);
  registerRoute("/api/back", HTTP_POST, handleBack);
  registerRoute("/api/left", HTTP_POST, handleLeft);
  registerRoute("/api/right", HTTP_POST, handleRight);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println(F("HTTP server ready."));

  wsServer.begin();
  wsServer.onEvent([](uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
    (void)payload;
    (void)length;
    if (type == WStype_CONNECTED) {
      String statusPayload = buildWsStatusJson(true);
      wsServer.sendTXT(num, statusPayload);
      if (cachedSensors.ok) {
        String sensorsPayload = buildWsSensorsJson(cachedSensors);
        wsServer.sendTXT(num, sensorsPayload);
      }
    }
  });

  sampleSensorsIfDue();
}

void loop() {
  dnsServer.processNextRequest();
  wsServer.loop();
  server.handleClient();
  stopDriveMotionIfTimedOut();
  sampleSensorsIfDue();
  delay(10);
}
