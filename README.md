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
8. הכנס את כתובת ה-Pages ב-CloudPhone Console.

## הנחיות נוספות בעברית (ESP32 + רומבה)
1. פתח את `esp32/roomba_dashboard_api.ino` ועדכן `WIFI_SSID` ו-`WIFI_PASS` לערכים האמיתיים שלך.
2. ודא שחיבור UART בין ESP32 לרומבה נכון: `RX=GPIO16`, `TX=GPIO17` (בהתאם לקוד).
3. העלה את הסקץ' ל-ESP32 דרך Arduino IDE או PlatformIO.
4. פתח Serial Monitor בקצב `115200` ואמת שהמודול התחבר לרשת והדפיס כתובת IP.
5. בדשבורד, שמור את כתובת ה-API בפורמט `http://<ip>` ולחץ "בדוק חיבור".
6. אם `/api/sensors` מחזיר `sensor_read_failed`, בדוק שוב:
   - חיווט TX/RX,
   - קרקע משותפת (GND),
   - שהרומבה במצב START/SAFE.
7. מומלץ להתחיל עם `esp32/roomba_basic_start.ino` כדי לוודא שהרומבה מגיבה לפקודות בסיסיות לפני הפעלת API מלא.
