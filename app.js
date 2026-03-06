/*
  ======================================================
  Roomba Pro Dashboard - Client Runtime
  ======================================================
  קובץ זה מנהל את:
  1) מציאת כתובת ה-API (Query / localStorage / גלובלי)
  2) Polling ל-/api/status ול-/api/sensors
  3) שליחת פקודות שליטה לרומבה
  4) רינדור חיישנים דינמי לכל מפתח שחוזר מהשרת
  5) UX: הודעות מצב, cooldown, keyboard navigation
*/

/** מנקה כתובת API (רווחים + slash בסוף). */
function normalizeApiBase(value) {
  if (!value) return "";
  return value.trim().replace(/\/+$/, "");
}

/**
 * סדר קדימויות למציאת API:
 * 1) Query param: ?apiBase= או ?api=
 * 2) localStorage
 * 3) window.ROBOT_API_BASE / window.API_BASE
 */
function resolveApiBase() {
  const params = new URLSearchParams(window.location.search);
  const fromQuery = params.get("apiBase") || params.get("api");
  if (fromQuery) return normalizeApiBase(fromQuery);

  const fromStorage = localStorage.getItem("roombaApiBase");
  if (fromStorage) return normalizeApiBase(fromStorage);

  const fromGlobal = window.ROBOT_API_BASE || window.API_BASE;
  if (fromGlobal) return normalizeApiBase(fromGlobal);

  return "";
}

let API_BASE = resolveApiBase();
const REFRESH_MS = 1500;

/** תרגום קודי טעינה לתיאור קריא */
const chargingLabels = {
  0: "Not Charging",
  1: "Reconditioning",
  2: "Full Charging",
  3: "Trickle Charging",
  4: "Waiting",
  5: "Charging Fault"
};

/** Helper קצר ל-DOM */
const $ = (id) => document.getElementById(id);

/** כל האלמנטים שהקוד עובד איתם */
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

function isGithubPagesHost() {
  return window.location.hostname.endsWith("github.io");
}

/** בדיקת חיבור ידנית ע"י המשתמש */
async function testConnection() {
  ui.commandStatus.textContent = "בודק חיבור ל-API...";
  await refreshStatus();
  await refreshSensors();
  if (ui.conn.textContent.includes("מחובר")) {
    ui.commandStatus.textContent = "✅ החיבור תקין, אפשר לשלוח פקודות.";
  }
}

/**
 * מעדכן API_BASE בזיכרון וב-UI, ואופציונלית שומר localStorage.
 */
function setApiBase(value, persist = true) {
  API_BASE = normalizeApiBase(value);
  if (persist) {
    if (API_BASE) {
      localStorage.setItem("roombaApiBase", API_BASE);
    } else {
      localStorage.removeItem("roombaApiBase");
    }
  }
  ui.apiBaseInput.value = API_BASE;
  ui.apiBaseDisplay.textContent = API_BASE || "לא הוגדרה";
}

setApiBase(API_BASE, false);

// אם הגיע apiBase מה-URL, נשמור וננקה query string מהכתובת
const params = new URLSearchParams(window.location.search);
if (params.get("apiBase") || params.get("api")) {
  localStorage.setItem("roombaApiBase", API_BASE);
  window.history.replaceState({}, "", window.location.pathname);
}

if (!API_BASE && isGithubPagesHost()) {
  ui.commandStatus.textContent = "ברוך הבא 👋 כדי להתחיל: הזן כתובת ESP32, לחץ 'שמור כתובת', ואז 'בדוק חיבור'.";
}

let cooldownUntil = 0;
let sensorsBusy = false;

/** איפוס תצוגת מדדים במקרה שגיאת תקשורת */
function clearUI() {
  ui.batteryPercent.textContent = "--";
  ui.batteryMv.textContent = "--";
  ui.currentMa.textContent = "--";
  ui.chargingCode.textContent = "--";
  ui.chargingState.textContent = "--";
  ui.buttons.textContent = "--";
  ui.batteryFill.style.width = "0%";
  ui.batteryFill.style.background = "linear-gradient(90deg, #ff6262 0%, #ffd95a 40%, #3ee089 100%)";
  ui.sensorGrid.innerHTML = '<div class="mini">אין נתוני חיישנים להצגה.</div>';
}

/** אומדן אחוז סוללה לפי מתח (התאמה גסה ל-4S Li-ion). */
function estimateBatteryPercent(mv) {
  const pct = ((mv - 14000) / (16800 - 14000)) * 100;
  return Math.max(0, Math.min(100, Math.round(pct)));
}

function setOnlineState(state, text) {
  ui.dot.classList.remove("online", "offline");
  if (state === "online") ui.dot.classList.add("online");
  if (state === "offline") ui.dot.classList.add("offline");
  ui.conn.textContent = text;
}

/** ממיר ערכי חיישנים ל-UI בצורה בטוחה וקריאה. */
function toDisplayValue(value) {
  if (typeof value === "boolean") return value ? "ON" : "OFF";
  if (Array.isArray(value)) return value.join(", ");
  if (value && typeof value === "object") {
    try {
      return JSON.stringify(value);
    } catch {
      return "[object]";
    }
  }
  return value ?? "--";
}

/** רינדור דינמי של כל מפתחות /api/sensors */
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

/**
 * מעדכן את המדדים הראשיים + רינדור כל החיישנים.
 */
function applySensorData(data) {
  const mv = Number(data.battery_mV ?? 0);
  const pct = estimateBatteryPercent(mv);
  const current = Number(data.current_mA ?? 0);
  const code = Number(data.charging_state ?? -1);

  ui.batteryPercent.textContent = pct;
  ui.batteryMv.textContent = mv || "--";
  ui.batteryFill.style.width = `${pct}%`;

  // צבע סוללה דינמי לפי אחוז
  if (pct < 25) {
    ui.batteryFill.style.background = "#ff6262";
  } else if (pct < 60) {
    ui.batteryFill.style.background = "#ffd95a";
  } else {
    ui.batteryFill.style.background = "#3ee089";
  }

  ui.currentMa.textContent = Number.isFinite(current) ? current : "--";
  ui.chargingCode.textContent = Number.isFinite(code) ? code : "--";
  ui.chargingState.textContent = chargingLabels[code] || "Unknown";
  ui.buttons.textContent = Number(data.buttons ?? 0);

  renderAllSensors(data);

  const now = new Date();
  ui.lastUpdate.textContent = `עודכן: ${now.toLocaleTimeString("he-IL")}`;
}

/**
 * בקשת JSON כללית לכל קריאות ה-API.
 */
async function fetchJson(path, options = {}) {
  if (!API_BASE && isGithubPagesHost()) {
    throw new Error("API base לא הוגדר");
  }

  const res = await fetch(`${API_BASE}${path}`, {
    headers: { Accept: "application/json" },
    ...options
  });

  if (!res.ok) {
    throw new Error(`HTTP ${res.status}`);
  }

  const ct = res.headers.get("content-type") || "";
  return ct.includes("application/json") ? res.json() : {};
}

/** בדיקת זמינות שרת */
async function refreshStatus() {
  if (!API_BASE) {
    setOnlineState("offline", "אין כתובת API");
    return;
  }

  try {
    await fetchJson("/api/status");
    setOnlineState("online", "מחובר לרומבה");
  } catch {
    setOnlineState("offline", "אין תקשורת API");
  }
}

/** טעינת חיישנים מחזורית עם הגנה מהצפה */
async function refreshSensors() {
  if (!API_BASE) {
    ui.commandStatus.textContent = "אין כתובת API";
    clearUI();
    return;
  }

  if (sensorsBusy) return;
  sensorsBusy = true;
  ui.lastUpdate.textContent = "טוען נתונים...";

  try {
    const data = await fetchJson("/api/sensors");
    applySensorData(data);
  } catch (err) {
    clearUI();
    ui.lastUpdate.textContent = `שגיאת קריאה: ${err.message}`;
  } finally {
    sensorsBusy = false;
  }
}

function setButtonsDisabled(disabled) {
  ui.actionButtons.forEach((btn) => {
    btn.disabled = disabled;
  });
}

/** שליחת פקודת שליטה לרומבה */
async function sendAction(action) {
  if (!API_BASE) {
    ui.commandStatus.textContent = "אין כתובת API";
    return;
  }

  const now = Date.now();
  if (now < cooldownUntil) {
    ui.commandStatus.textContent = "ממתין cooldown קצר...";
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

// חיבור כפתורי הפעולה לפונקציית sendAction
ui.actionButtons.forEach((btn) => {
  btn.addEventListener("click", () => sendAction(btn.dataset.action));
});

// שמירה + בדיקה מיידית
ui.saveApiBase.addEventListener("click", async () => {
  const raw = ui.apiBaseInput.value;
  const normalized = normalizeApiBase(raw);

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

// בדיקה ידנית ללא שינוי כתובת
ui.testApiBase.addEventListener("click", async () => {
  if (!API_BASE) {
    ui.commandStatus.textContent = "יש להזין כתובת API לפני בדיקת חיבור.";
    return;
  }
  await testConnection();
});

// ניקוי כתובת API שמורה
ui.clearApiBase.addEventListener("click", () => {
  setApiBase("", true);
  ui.commandStatus.textContent = "כתובת API נוקתה.";
  if (isGithubPagesHost()) {
    setOnlineState("offline", "חסר יעד API");
  }
  clearUI();
});

// תמיכה בניווט מקלדת בין כפתורי שליטה
const keyOrder = ["clean", "spot", "safe", "stop"];
let keyFocus = 0;

function refreshKeyFocus() {
  ui.actionButtons.forEach((btn, i) => {
    btn.style.outline = i === keyFocus ? "2px solid #a8c7ff" : "none";
  });
}

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

/** אתחול ראשוני + polling */
async function bootstrap() {
  refreshKeyFocus();
  await refreshStatus();
  await refreshSensors();
  setInterval(refreshStatus, REFRESH_MS * 3);
  setInterval(refreshSensors, REFRESH_MS);
}

bootstrap();
