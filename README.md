# cloudphone
# Roomba Pro Dashboard ל-CloudPhone

דשבורד מובייל מלא לשליטה ב-Roomba דרך REST API (מיועד ל-ESP32 עם SCI).

## יכולות עיקריות
- שליחת פקודות בזמן אמת: `Clean`, `Spot`, `Safe`, `Stop`.
- ניטור חיישנים בלייב (`/api/sensors`) אחת לשנייה.
- תצוגת מצב סוללה (mV + אחוז משוער), מצב טעינה, זרם וכפתורים.
- סטטוס חיישנים קריטיים: `bumper_left`, `bumper_right`, `cliff`.
- ממשק רספונסיבי מלא + ניווט מקלדת (מתאים גם ל-CloudPhone).

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

## ניווט (מקלדת)
- `ArrowDown` / `ArrowUp` – מעבר בין כפתורי שליטה.
- `Enter` – שליחת פקודה מסומנת.


## עדיין לא עובד? פתרון תקלות מהיר
- אם מופיע "חסר יעד API" — הזן כתובת ESP32 בחלק העליון ולחץ "שמור כתובת".
- ודא שהטלפון וה-ESP32 באותה רשת מקומית.
- בדיקה מהירה: פתח בדפדפן `http://<esp32-ip>/api/status` וודא שמתקבל JSON.
- אם שינית IP ל-ESP32, עדכן את הכתובת במסך ולחץ שוב "שמור כתובת".
