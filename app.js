const $ = (id) => document.getElementById(id);

let API_BASE = "";
const SENSOR_REFRESH_MS = 250;
const STATUS_REFRESH_MS = 2000;
const DRIVE_REPEAT_MS = 200;
let cooldownUntil = 0;
let sensorsBusy = false;
let statusBusy = false;
let driveInterval = null;
let activeDrive = null;
let activeDrivePointerId = null;
let driveSessionId = 0;

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

  return "";
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
    const data = await fetchJson("/api/sensors");
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

function setDriveActive(direction) {
  activeDrive = direction;
  ui.driveButtons.forEach((btn) => {
    btn.classList.toggle("active", btn.dataset.drive === direction);
  });
}

function clearDriveActive() {
  activeDrive = null;
  activeDrivePointerId = null;
  ui.driveButtons.forEach((btn) => btn.classList.remove("active"));
}

async function sendDrive(direction, updateStatus = true) {
  if (!API_BASE) {
    setCommandStatus("No API address");
    return false;
  }

  if (isMixedContentBlocked()) {
    setCommandStatus(mixedContentMessage());
    return false;
  }

  try {
    await fetchJson(`/api/${direction}`, { method: "POST" });
    if (updateStatus) {
      setCommandStatus(`${direction} drive command sent`);
    }
    return true;
  } catch (err) {
    if (updateStatus) {
      setCommandStatus(`${direction} drive failed (${err.message})`);
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
  clearDriveActive();

  if (sendStop) {
    sendDrive("stop", true).then(() => {
      refreshSensors();
    });
  }
}

async function startDriveLoop(direction, pointerId, button) {
  if (activeDrive === direction && activeDrivePointerId === pointerId && driveInterval) {
    return;
  }

  stopDriveLoop(false);
  const sessionId = ++driveSessionId;
  activeDrivePointerId = pointerId;
  setDriveActive(direction);
  if (button?.setPointerCapture && pointerId !== undefined && pointerId !== null) {
    try {
      button.setPointerCapture(pointerId);
    } catch {
      // Ignore capture failures and rely on the watchdog on the ESP32.
    }
  }

  const ok = await sendDrive(direction, true);
  if (!ok || driveSessionId !== sessionId || activeDrive !== direction || activeDrivePointerId !== pointerId) {
    clearDriveActive();
    if (ok) {
      sendDrive("stop", false);
    }
    return;
  }

  driveInterval = setInterval(() => {
    if (driveSessionId !== sessionId || activeDrive !== direction) {
      clearInterval(driveInterval);
      driveInterval = null;
      return;
    }
    sendDrive(direction, false);
  }, DRIVE_REPEAT_MS);
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
    await fetchJson(`/api/${action}`, { method: "POST" });
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
    await startDriveLoop(direction, event.pointerId, btn);
  };
  const stop = (event) => {
    event.preventDefault();
    if (activeDrive === direction && (activeDrivePointerId === null || event.pointerId === activeDrivePointerId)) {
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
    if (activeDrive) {
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
    clearUI();
    setOnlineState("offline", "No API connectivity");
    setCommandStatus("API address cleared.");
  });

  refreshStatus();
  refreshSensors();
  setInterval(refreshStatus, STATUS_REFRESH_MS);
  setInterval(refreshSensors, SENSOR_REFRESH_MS);
}

bootstrap();
