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

## התקנה
1. העלה את הפרויקט לריפו GitHub ציבורי.
2. עבור ל-Settings → Pages.
3. בחר branch `main` ותיקייה `/root`.
4. פתח את הדשבורד עם פרמטר `apiBase` שמצביע ל-ESP32, למשל:
   `https://<user>.github.io/<repo>/?apiBase=http://192.168.1.50`
   (אפשר גם `?api=...`). הערך נשמר אוטומטית ב-`localStorage` לשימושים הבאים.
5. הכנס את כתובת ה-Pages ב-CloudPhone Console.

## ניווט (מקלדת)
- `ArrowDown` / `ArrowUp` – מעבר בין כפתורי שליטה.
- `Enter` – שליחת פקודה מסומנת.
#.

## הוראות התקנה
1. העלה את הקוד הזה ל־GitHub בריפו ציבורי.
2. עבור ל־Settings → Pages.
3. בחר את ה־branch `main` ותיקייה `/root`.
4. שמור → תקבל כתובת ציבורית בסגנון:
5. הכנס את הכתובת הזו ב־CloudPhone Console כ־URL של האפליקציה.
