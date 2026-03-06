# Roomba Pro Dashboard

דשבורד מובייל מלא לשליטה ב-Roomba דרך REST API (מיועד ל-ESP32 עם SCI).

## יכולות עיקריות
- שליחת פקודות בזמן אמת: `Clean`, `Spot`, `Safe`, `Stop`.
- ניטור חיישנים בלייב (`/api/sensors`) כל ~3 שניות.
- תצוגת מצב סוללה (mV + אחוז משוער), מצב טעינה, זרם וכפתורים.
- תצוגה מלאה של כל החיישנים שחוזרים מ-`/api/sensors` (דינמי לפי ה-JSON מהשרת).
- ממשק רספונסיבי מלא + ניווט מקלדת לדשבורד מובייל.

## REST API צפוי מהשרת (ESP32)
- `GET /api/status`
- `GET /api/sensors`
- `POST /api/clean`
- `POST /api/spot`
- `POST /api/safe`
- `POST /api/stop`

## דוגמת JSON ל-`/api/sensors`
```json
{
  "battery_mV": 15840,
  "charging_state": 2,
  "current_mA": -520,
  "buttons": 1,
  "bumper_left": false,
  "bumper_right": true,
  "cliff": false
}
```

## איך מפעילים מהר? (2 דקות)
1. מצא את כתובת ה-IP של ה-ESP32 באותה רשת Wi‑Fi (לדוגמה: `192.168.1.50`).
2. פתח את הדשבורד.
3. בכרטיס **"הגדרת חיבור API"** הזן: `http://<ESP32-IP>`.
4. לחץ **"שמור כתובת"** ואז **"בדוק חיבור"**.
5. אם למעלה מופיע **"מחובר לרומבה"** — זה עובד ✅

## התקנה
1. העלה את הפרויקט לריפו GitHub ציבורי.
2. עבור ל-Settings → Pages.
3. בחר branch `main` ותיקייה `/root`.
4. פתח את הדשבורד דרך כתובת ה-Pages.
5. במסך "הגדרת חיבור API" הזן את כתובת ה-ESP32 (לדוגמה `http://192.168.1.50`) ולחץ **"שמור כתובת"**.
6. אחרי שמירה, הכתובת נשמרת אוטומטית במכשיר (`localStorage`) כך שאין צורך להקליד שוב בכל פתיחה.
7. אופציונלי: אפשר גם לפתוח ישירות עם פרמטר ב-URL:
   `https://<user>.github.io/<repo>/?apiBase=http://192.168.1.50`
   (נתמך גם alias של `?api=...`).
8. שמור את כתובת ה-Pages לשימוש יומיומי מהטלפון/דפדפן.

## ניווט (מקלדת)
- `ArrowDown` / `ArrowUp` – מעבר בין כפתורי שליטה.
- `Enter` – שליחת פקודה מסומנת.
- `C` – שליחת `Clean`.
- `S` – שליחת `Spot`.
- `X` – שליחת `Stop`.


## עדיין לא עובד? פתרון תקלות מהיר
- אם מופיע "חסר יעד API" — הזן כתובת ESP32 בחלק העליון ולחץ "שמור כתובת".
- ודא שהטלפון וה-ESP32 באותה רשת מקומית.
- בדיקה מהירה: פתח בדפדפן `http://<esp32-ip>/api/status` וודא שמתקבל JSON.
- אם שינית IP ל-ESP32, עדכן את הכתובת במסך ולחץ שוב "שמור כתובת".


## מבנה קבצים
- `index.html` – מבנה הדשבורד בלבד.
- `style.css` – כל העיצוב של הדשבורד.
- `app.js` – לוגיקת התחברות API, polling, רינדור חיישנים ושליחת פקודות.

## קוד ESP32 תואם לדשבורד (מומלץ)
כדי שהכול יעבוד בצורה מלאה מול הדשבורד, השתמש בסקיצה: `esp32/roomba_dashboard_api.ino`.

הסקיצה מספקת בדיוק את ה-Endpoints שה-UI דורש:
- `GET /api/status`
- `GET /api/sensors`
- `POST /api/clean`
- `POST /api/spot`
- `POST /api/safe`
- `POST /api/stop`

### מה צריך לעדכן לפני העלאה ל-ESP32
1. בקובץ `esp32/roomba_dashboard_api.ino` עדכן:
   - `WIFI_SSID`
   - `WIFI_PASS`
2. העלה את הסקיצה ל-ESP32.
3. בדוק ב-Serial Monitor מה כתובת ה-IP שהתקבלה.
4. בדשבורד הזן `http://<ESP32-IP>` ולחץ "שמור כתובת" ואז "בדוק חיבור".

## קוד בסיסי בלבד (ללא REST API)
אם רוצים רק בדיקת שליחת פקודה סריאלית, אפשר להשתמש גם בקובץ: `esp32/roomba_basic_start.ino`.



## בדיקות התאמה אחרי העלאה ל-ESP32
1. פתח בדפדפן: `http://<ESP32-IP>/api/status` – צריך לחזור JSON עם `ok: true`.
2. פתח בדפדפן: `http://<ESP32-IP>/api/sensors` – צריך לחזור JSON עם חיישנים (`battery_mV`, `charging_state` וכו').
3. בדוק פקודה ידנית (למשל עם curl):
   `curl -X POST http://<ESP32-IP>/api/clean`
4. בדשבורד, לחץ "בדוק חיבור" ואז שלח פקודת Clean/Spot/Stop.

> מומלץ להתחיל עם `esp32/roomba_dashboard_api.ino` לפרויקט המלא,
> ולהשתמש ב-`esp32/roomba_basic_start.ino` רק לבדיקת SCI בסיסית.


## ביצועים ואמינות
- Polling חיישנים: כל 3 שניות (`SENSORS_REFRESH_MS = 3000`).
- Polling סטטוס: כל 10 שניות (`STATUS_REFRESH_MS = 10000`).
- לכל בקשת API יש timeout של 5 שניות (`AbortController`) כדי למנוע תקיעות.
- רינדור חיישנים נעשה עם יצירת אלמנטים בטוחה (ללא `innerHTML` לערכי API) כדי למנוע XSS.
