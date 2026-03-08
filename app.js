const $ = (id) => document.getElementById(id);

let API_BASE = "";
const REFRESH_MS = 1500;
let cooldownUntil = 0;
let sensorsBusy = false;
let statusBusy = false;

// מיפוי קודי טעינה שמגיעים מהרומבה לטקסט ידידותי בעברית.
const chargingLabels = {
  0: "לא בטעינה",
  1: "שיקום סוללה",
  2: "טעינה מלאה",
  3: "טעינת תחזוקה",
  4: "ממתין",
  5: "תקלה בטעינה"
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

// קבלת כתובת API לפי קדימות: פרמטר בכתובת > localStorage.
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
  ui.sensorGrid.innerHTML = '<div class="mini">אין נתוני חיישנים.</div>';
}

function batteryColor(pct) {
  if (pct < 25) return "#ff6262";
  if (pct < 60) return "#ffd95a";
  return "#3ee089";
}

// אומדן אחוז סוללה לרומבה על בסיס סוללת ליתיום 4S.
function estimateBatteryPercent(mv) {
  const pct = ((mv - 14000) / (16800 - 14000)) * 100;
  return Math.max(0, Math.min(100, Math.round(pct)));
}

function toDisplayValue(value) {
  if (typeof value === "boolean") return value ? "פועל" : "כבוי";
  if (Array.isArray(value)) return value.join(", ");
  if (value && typeof value === "object") {
    try {
      return JSON.stringify(value);
    } catch {
      return "[אובייקט]";
    }
  }

  return value ?? "--";
}

function renderAllSensors(data) {
  const keys = Object.keys(data || {}).sort();
  if (!keys.length) {
    ui.sensorGrid.innerHTML = '<div class="mini">לא התקבלו נתוני חיישנים.</div>';
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
  ui.chargingState.textContent = chargingLabels[code] || "לא ידוע";
  ui.buttons.textContent = Number(data.buttons ?? 0);

  renderAllSensors(data);
  ui.lastUpdate.textContent = `עודכן: ${new Date().toLocaleTimeString("he-IL")}`;
}

async function fetchJson(path, options = {}) {
  const res = await fetch(`${API_BASE}${path}`, {
    headers: { Accept: "application/json" },
    ...options
  });

  if (!res.ok) throw new Error(`HTTP ${res.status}`);

  const ct = res.headers.get("content-type") || "";
  return ct.includes("application/json") ? res.json() : {};
}

async function refreshStatus() {
  if (statusBusy) return;
  if (!API_BASE) {
    ui.commandStatus.textContent = "אין כתובת API";
    setOnlineState("offline", "אין תקשורת API");
    return;
  }

  statusBusy = true;
  try {
    await fetchJson("/api/status");
    setOnlineState("online", "מחובר לרומבה");
  } catch {
    setOnlineState("offline", "אין תקשורת API");
    clearUI();
  } finally {
    statusBusy = false;
  }
}

async function refreshSensors() {
  if (sensorsBusy) return;
  if (!API_BASE) {
    ui.commandStatus.textContent = "אין כתובת API";
    return;
  }

  sensorsBusy = true;
  ui.lastUpdate.textContent = "טוען נתונים...";
  try {
    const data = await fetchJson("/api/sensors");
    applySensorData(data);
  } catch (err) {
    ui.lastUpdate.textContent = `שגיאת קריאה: ${err.message}`;
    clearUI();
  } finally {
    sensorsBusy = false;
  }
}

function setButtonsDisabled(disabled) {
  ui.actionButtons.forEach((btn) => {
    btn.disabled = disabled;
  });
}

async function sendAction(action) {
  if (!API_BASE) {
    ui.commandStatus.textContent = "אין כתובת API";
    return;
  }

  const now = Date.now();
  if (now < cooldownUntil) {
    ui.commandStatus.textContent = "ממתין זמן צינון קצר...";
    return;
  }

  setButtonsDisabled(true);
  ui.commandStatus.textContent = `שולח ${action}...`;

  try {
    await fetchJson(`/api/${action}`, { method: "POST" });
    ui.commandStatus.textContent = `✅ נשלחה פקודת ${action}`;
  } catch (err) {
    ui.commandStatus.textContent = `❌ פקודת ${action} נכשלה (${err.message})`;
  } finally {
    cooldownUntil = Date.now() + 300;
    setTimeout(() => setButtonsDisabled(false), 300);
  }
}

async function testConnection() {
  ui.commandStatus.textContent = "בודק חיבור ל-API...";
  await refreshStatus();
  await refreshSensors();

  if (ui.conn.textContent.includes("מחובר")) {
    ui.commandStatus.textContent = "✅ החיבור תקין, אפשר לשלוח פקודות.";
  }
}

function bootstrap() {
  setApiBase(resolveApiBase(), false);

  const params = new URLSearchParams(window.location.search);
  if (params.get("apiBase") || params.get("api")) {
    localStorage.setItem("roombaApiBase", API_BASE);
    window.history.replaceState({}, "", window.location.pathname);
  }

  if (!API_BASE) {
    ui.commandStatus.textContent = "הזן כתובת API ולחץ שמור.";
  }

  ui.actionButtons.forEach((btn) => {
    btn.addEventListener("click", () => sendAction(btn.dataset.action));
  });

  ui.saveApiBase.addEventListener("click", async () => {
    const normalized = normalizeApiBase(ui.apiBaseInput.value);
    if (!normalized) {
      ui.commandStatus.textContent = "יש להזין כתובת API לפני שמירה.";
      return;
    }

    if (!/^https?:\/\//i.test(normalized)) {
      ui.commandStatus.textContent = "כתובת חייבת להתחיל ב-http:// או https://";
      return;
    }

    setApiBase(normalized, true);
    await testConnection();
  });

  ui.testApiBase.addEventListener("click", testConnection);

  ui.clearApiBase.addEventListener("click", () => {
    setApiBase("", true);
    clearUI();
    setOnlineState("offline", "אין תקשורת API");
    ui.commandStatus.textContent = "כתובת API נוקתה.";
  });

  // ניווט מקלדת נוח לשימוש במסכים ללא עכבר.
  const keyOrder = ["clean", "spot", "safe", "stop"];
  let keyFocus = 0;
  const refreshKeyFocus = () => {
    ui.actionButtons.forEach((b, i) => {
      b.style.outline = i === keyFocus ? "2px solid #a8c7ff" : "none";
    });
  };

  document.addEventListener("keydown", (e) => {
    if (e.key === "ArrowDown") {
      keyFocus = (keyFocus + 1) % keyOrder.length;
      refreshKeyFocus();
    } else if (e.key === "ArrowUp") {
      keyFocus = (keyFocus - 1 + keyOrder.length) % keyOrder.length;
      refreshKeyFocus();
    } else if (e.key === "Enter") {
      sendAction(keyOrder[keyFocus]);
    }
  });

  refreshKeyFocus();
  refreshStatus();
  refreshSensors();
  setInterval(refreshStatus, REFRESH_MS * 3);
  setInterval(refreshSensors, REFRESH_MS);
}

bootstrap();
